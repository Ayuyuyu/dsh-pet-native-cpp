// 原生桌面宠物：Win32 + D3D11 + DComp + 裁剪版 FFmpeg（VP9/libvpx）。
// 多宠物：共享 D3D device / shader；每宠物独立交换链/RTV/窗口状态，解码器/纹理/掩码按 clip 实例共享。
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM（有符号屏幕坐标）

#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/config.h"
#include "core/decode.h"
#include "core/log.h"
#include "core/mask.h"
#include "core/physics.h"
#include "core/util.h"
#include "platform/win/render_d3d11.h"
#include "platform/win/win_clip_pool.h"
#include "platform/win/win_pet.h"
#include "platform/win/win_pet_ops.h"
#include "platform/win/win_ui.h"
#include "platform/win/win_util.h"

using Microsoft::WRL::ComPtr;
using namespace core;       // NOLINT
using namespace core::log;  // 日志保持原有调用形式
using namespace winpet;     // Pet 与全局状态
using namespace winutil;   // 工具函数保持原有调用形式
using winui::g_menuNames;
using winui::kIdTrayExit;
using winui::kIdTrayToggle;
using winui::kTrayMsg;

// win_pet.h 里 extern 的全局状态集中在单个 TU 定义。
namespace winpet {
std::vector<std::unique_ptr<Pet>> g_pets;
int g_sizePercent = 15;  // 启动时由 core::g_initialSizePercent 覆盖
std::string g_startClip;
}  // namespace winpet

namespace {

constexpr wchar_t kClass[] = L"PetNativeCpp";
// 全局节拍窗口：整个进程只挂一个 16ms 定时器，串行驱动所有宠物。
// 每只宠物各挂一个 16ms 定时器时，N 只就有 N 条 WM_TIMER 在 UI 线程排队，
// 主线程一饱和就表现为画面与碰撞一起卡。
constexpr wchar_t kTickClass[] = L"PetNativeCppTick";
// 自动化测试专用的退出消息。绝不能复用 WM_KEYDOWN：右键菜单会
// SetForegroundWindow(hwnd)，之后用户随便按个键（如截图快捷键里的 A）
// 都会被当成退出指令，表现为「点一下宠物/选个动作程序就没了」。
constexpr UINT kMsgRequestQuit = WM_APP + 101;  // 100 已被 kTrayMsg 占用
constexpr int kMaxPets = 64;  // --pets 上限：50 只实测目标 + 余量
// --monitor=N：1 表示主屏；0/缺省也固定主屏（不轮询）。
int g_monitorChoice = 0;
HWND g_tickHwnd = nullptr;
int g_pendingCreates = 0;  // 启动时分批创建：还剩多少只没建
// 单调宠物 id：关闭中间一只后不能用 g_pets.size() 复用同一个 id，
// 否则日志、菜单与调试状态里会出现两只「pet2」。网格位置另按当前数量计算。
int g_nextPetId = 0;

constexpr int kMaskW = 512;
constexpr int kMaskH = 288;

// 日志落盘实现：公共 core::log 只负责格式化，时间戳与目标由平台注入。
void WinLogSink(std::string_view line) {
  // 日志放 exe 目录，随程序分发；命名与 Linux 端统一为 pet-<平台>.log。
  // 静态复用 ofstream，避免每条日志都重算路径并开关文件。
  // 用 filesystem::path 从宽字符目录构造，CRT 才能按 UTF-16 打开文件；
  // 转成 UTF-8 窄串会按 ANSI 代码页解释，中文安装目录下日志会写不出去。
  static std::ofstream out(std::filesystem::path(ExeDir()) / L"pet-win.log",
                           std::ios::app);
  if (!out) return;
  // tick 时间戳（与 GetTickCount 同源），便于与外部采样/截图对齐。
  out << '[' << GetTickCount() << "] " << line << '\n' << std::flush;
}

// 提升系统定时器精度：默认 15.6ms 粒度会让 16ms 定时器与 41.7ms 帧间隔抖动。
// 用 RAII 保证对象构造后的退出路径都会配对 timeEndPeriod（--probe 在构造前已提前退出）。
class ScopedTimerPeriod {
 public:
  explicit ScopedTimerPeriod(UINT ms) : ms_(ms) { timeBeginPeriod(ms_); }
  ~ScopedTimerPeriod() { timeEndPeriod(ms_); }
  ScopedTimerPeriod(const ScopedTimerPeriod&) = delete;
  ScopedTimerPeriod& operator=(const ScopedTimerPeriod&) = delete;

 private:
  UINT ms_;
};

// 主文件负责：初始化、窗口与消息分发、输入事件、全局节拍调度。
// 帧管线、动作决策、菜单/托盘/工具与 GPU 渲染分别在各 win_*.cpp 与 core/。

// 按枚举顺序收集显示器；主屏永远排第一，保证 --monitor=1 行为稳定。
std::vector<HMONITOR> EnumerateMonitors() {
  std::vector<HMONITOR> out;
  EnumDisplayMonitors(nullptr, nullptr,
                      [](HMONITOR h, HDC, LPRECT, LPARAM p) -> BOOL {
                        reinterpret_cast<std::vector<HMONITOR>*>(p)->push_back(h);
                        return TRUE;
                      },
                      reinterpret_cast<LPARAM>(&out));
  const HMONITOR primary =
      MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
  const auto it = std::find(out.begin(), out.end(), primary);
  if (it != out.end() && it != out.begin()) std::rotate(out.begin(), it, it + 1);
  if (out.empty()) out.push_back(primary);
  return out;
}

// 返回目标显示器；未指定或 --monitor=N 越界时都用主屏。
HMONITOR PickMonitor(int* monitorNo = nullptr) {
  static const std::vector<HMONITOR> monitors = EnumerateMonitors();
  int selected = 0;
  if (g_monitorChoice > 0) {
    if (g_monitorChoice <= static_cast<int>(monitors.size())) {
      selected = g_monitorChoice - 1;
    } else {
      Log("monitor {} out of range (total {}), use primary", g_monitorChoice,
          monitors.size());
    }
  }
  if (monitorNo != nullptr) *monitorNo = selected + 1;
  return monitors[static_cast<size_t>(selected)];
}

bool CreatePet(int index) {
  auto pet = std::make_unique<Pet>();
  pet->index = index;

  int monitorNo = 1;
  HMONITOR mon = PickMonitor(&monitorNo);
  MONITORINFO mi{sizeof(mi)};
  if (!GetMonitorInfoW(mon, &mi)) {
    // 失败时 rcWork 全 0，会把宠物算到屏幕外，用户完全看不到它。
    Log("GetMonitorInfoW failed, fallback to primary screen metrics");
    RECT rc{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    mi.rcWork = rc;
    mi.rcMonitor = rc;
  }
  const int workW = mi.rcWork.right - mi.rcWork.left;
  const int workH = mi.rcWork.bottom - mi.rcWork.top;
  // 新增宠物沿用当前缩放比例（右键调整后 g_sizePercent 已更新）。
  int targetW = workW * g_sizePercent / 100;
  if (targetW < 220) targetW = 220;
  if (targetW > 640) targetW = 640;
  pet->windowW = targetW;
  pet->windowH = targetW * 360 / 640;
  pet->bodyR = pet->windowW;
  pet->bodyB = pet->windowH;
  // 以工作区中心为基准铺开，网格槽位按当前宠物数计算（id 只做标识）：
  // 关闭一只后新增的宠物可以填补空位，视觉布局保持稳定。
  constexpr int kGridCols = 8;
  constexpr int kGridRows = (kMaxPets + kGridCols - 1) / kGridCols;
  const int slot = static_cast<int>(g_pets.size());
  // 先按 55% 取目标间距，再按工作区可用空间收紧：8 行 × 55% 的窗口高
  // 在 1080p 下约 1746px，远超工作区，旧实现钳制后多行完全重叠。
  const int spareW = std::max(0, workW - pet->windowW);
  const int spareH = std::max(0, workH - pet->windowH);
  const int gapX = std::min(pet->windowW * 55 / 100,
                            kGridCols > 1 ? spareW / (kGridCols - 1) : 0);
  const int gapY = std::min(pet->windowH * 55 / 100,
                            kGridRows > 1 ? spareH / (kGridRows - 1) : 0);
  const int gridW = (kGridCols - 1) * gapX + pet->windowW;
  const int gridH = (kGridRows - 1) * gapY + pet->windowH;
  int homeX = mi.rcWork.left + (workW - gridW) / 2 + (slot % kGridCols) * gapX;
  int homeY = mi.rcWork.top + (workH - gridH) / 2 + (slot / kGridCols) * gapY;
  // 兜底钳制：放大比例很大时网格会超出工作区，也不能让窗口跑出去。
  if (homeX < mi.rcWork.left) homeX = mi.rcWork.left;
  if (homeY < mi.rcWork.top) homeY = mi.rcWork.top;
  if (homeX + pet->windowW > mi.rcWork.right) {
    homeX = mi.rcWork.right - pet->windowW;
  }
  if (homeY + pet->windowH > mi.rcWork.bottom) {
    homeY = mi.rcWork.bottom - pet->windowH;
  }

  // 窗口交给 RAII 持有：任一失败路径自动 DestroyWindow，成功路径末尾 release()。
  // 声明在 pet 之后，保证窗口先析构（此时 pet 仍有效，WM_DESTROY 会读 USERDATA）。
  ScopedWindow window(CreateWindowExW(
      WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClass,
      L"pet-native-cpp", WS_POPUP, homeX, homeY, pet->windowW, pet->windowH,
      nullptr, nullptr, GetModuleHandleW(nullptr), pet.get()));
  if (window.get() == nullptr) return false;
  HWND hwnd = window.get();
  pet->hwnd = hwnd;

  if (!RenderCreateTarget(pet->rt, hwnd, pet->windowW, pet->windowH)) {
    return false;
  }

  core::ActionSpec firstSpec = g_startClip.empty()
                                   ? core::PickNextAction(1)
                                   : core::SpecForName(g_startClip);
  if (firstSpec.name.empty()) {
    firstSpec.kind = core::ActionKind::Idle;
    firstSpec.name = g_idleClips.empty() ? std::string("待机呼吸休闲")
                                         : g_idleClips[0];
  }
  if (!winpetops::PlayAction(*pet, firstSpec)) {
    // 指定动作无效或素材缺失时退到待机：直接销毁窗口会让用户看到
    // 「程序启动了但什么都没出现」，比降级播放糟糕得多。
    Log("pet{} first clip failed, fallback to idle", index);
    if (!winpetops::PlayAction(*pet, core::IdleSpec())) {
      Log("pet{} idle fallback failed", index);
      return false;
    }
  }

  if (core::log::Enabled()) {
    const std::wstring title = L"pet" + std::to_wstring(index) + L" | " +
                               std::to_wstring(pet->windowW) + L"x" +
                               std::to_wstring(pet->windowH);
    SetWindowTextW(hwnd, title.c_str());
  }
  g_pets.push_back(std::move(pet));
  // 定时器由主程序的单一全局节拍统一驱动（见 kTickClass），
  // 每只宠物独立挂 16ms 定时器会在多只时把 UI 线程压满。
  winpetops::SyncWindowPos(*g_pets.back());  // 碰撞缓存初值
  ShowWindow(hwnd, SW_SHOWNOACTIVATE);
  // 成功：窗口生命周期交给 g_pets（关闭时 DestroyWindow），RAII 不再接管。
  window.release();
  Log("pet{} created {}x{} at {},{} (monitor={})", index,
      g_pets.back()->windowW, g_pets.back()->windowH, homeX, homeY, monitorNo);
  return true;
}

void TogglePetsVisible() {
  if (g_pets.empty()) return;
  const bool show = !IsWindowVisible(g_pets.front()->hwnd);
  for (const auto& pet : g_pets) {
    ShowWindow(pet->hwnd, show ? SW_SHOWNOACTIVATE : SW_HIDE);
    // 隐藏时同步遮挡状态与实例可见引用，否则解码线程与纹理上传照常满负载跑。
    pet->occluded = !show;
    if (pet->clip != nullptr) {
      winclips::SetVisible(pet->clip, show);
      pet->visible = show;
    }
  }
}

// 退出前先停掉所有解码线程：PostQuitMessage 只是结束消息循环，窗口不会被
// 销毁，WM_DESTROY 里的 Stop 也不会执行，g_pets 全局析构时 joinable 的
// std::thread 会触发 std::terminate（退出码 0xC0000409，且明显卡顿数秒）。
void RequestQuit() {
  // 先停全局节拍：后面的 DestroyWindow / Stop 会派发消息，
  // 不能再让 Tick 进到正在拆解的状态里。
  g_pendingCreates = 0;
  if (g_tickHwnd != nullptr) {
    KillTimer(g_tickHwnd, 1);
    DestroyWindow(g_tickHwnd);
    g_tickHwnd = nullptr;
  }
  for (const auto& p : g_pets) {
    if (p->hwnd != nullptr) {
      ShowWindow(p->hwnd, SW_HIDE);
      // 窗口不会走 WM_DESTROY，先断开 USERDATA，避免 g_pets.clear() 后
      // 若有消息派发到该窗口时读到悬垂的 Pet*。
      SetWindowLongPtrW(p->hwnd, GWLP_USERDATA, 0);
    }
    if (p->visible) winclips::SetVisible(p->clip, false);
    winclips::Release(p->clip);
    p->clip = nullptr;
    p->visible = false;
  }
  // 兜底：任何漏掉的实例都在这里 join，避免静态析构阶段 terminate。
  winclips::StopAll();
  // 托盘宿主可能在 WM_DESTROY 时迁移给别的宠物，必须按 TrayOwner 删除，
  // 否则 g_pets[0] 已不是宿主，NIM_DELETE 静默失败、任务栏图标残留。
  winui::RemoveTray(winui::TrayOwner());
  PostQuitMessage(0);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  Pet* pet = reinterpret_cast<Pet*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
    pet = static_cast<Pet*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pet));
  }
  if (pet == nullptr) return DefWindowProcW(hwnd, msg, wp, lp);

  switch (msg) {
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE: {
      // Alt+Tab、系统菜单或 Esc 会收回鼠标捕获，此后的 WM_LBUTTONUP
      // 不再送达本窗口。不复位 dragging 会让宠物一直跟随鼠标且禁止走路。
      if (pet->dragging) {
        pet->dragging = false;
        pet->dragAnimStarted = false;
        const core::ActionSpec next = core::PickNextAction(pet->facing);
        if (!next.name.empty()) winpetops::PlayAction(*pet, next);
      }
      return 0;
    }
    case WM_LBUTTONDOWN: {
      // 睡觉中点击宠物 = 「起来」：不播点击回应，也不进入拖动。
      if (pet->sleepPhase == SleepPhase::Start ||
          pet->sleepPhase == SleepPhase::Loop) {
        winpetops::ToggleSleep(*pet);
        return 0;
      }
      POINT pt{};
      GetCursorPos(&pt);
      RECT rc{};
      GetWindowRect(hwnd, &rc);
      pet->dragOffset.x = pt.x - rc.left;
      pet->dragOffset.y = pt.y - rc.top;
      pet->dragLastPos = pt;
      pet->dragStartPos = pt;
      pet->dragAnimStarted = false;
      pet->dragLastTick = GetTickCount();
      pet->vx = 0.0;
      pet->vy = 0.0;
      pet->flying = false;
      pet->dragging = true;
      SetCapture(hwnd);
      core::ActionSpec clickSpec;
      clickSpec.kind = core::ActionKind::Click;
      clickSpec.name = core::PickClickAction();
      if (clickSpec.name.empty()) clickSpec.name = core::PickRandom(g_clickClips);
      if (!clickSpec.name.empty()) winpetops::PlayAction(*pet, clickSpec);
      PlayClickSound();
      pet->squashStart = GetTickCount();
      pet->squashActive = true;
      return 0;
    }
    case WM_NCHITTEST: {
      // 透明像素穿透到下层窗口：命中掩码给 HTCLIENT，否则 HTTRANSPARENT
      // （系统会继续查找该点下的其他窗口，效果等价于原先的 SetWindowRgn，
      // 但不再需要每帧重建几百个 HRGN）。坐标是屏幕坐标，先转客户区。
      // GET_X_LPARAM/GET_Y_LPARAM 返回有符号坐标，多显示器负坐标才正确。
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ScreenToClient(hwnd, &pt);
      return winpetops::HitTest(*pet, pt) ? HTCLIENT : HTTRANSPARENT;
    }
    case WM_SETCURSOR:
      // 鼠标落在窗口区域（= 可点击区域）内时显示手型。
      if (LOWORD(lp) == HTCLIENT) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
      }
      return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_MOUSEMOVE:
      if (pet->dragging && (wp & MK_LBUTTON)) {
        POINT pt{};
        GetCursorPos(&pt);
        SetWindowPos(hwnd, nullptr, pt.x - pet->dragOffset.x,
                     pt.y - pet->dragOffset.y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        // 拖动也要同步碰撞缓存，否则松手甩出时用的是旧位置。
        winpetops::SyncWindowPos(*pet);
        // 拖动超过阈值才切到「被鼠标拖拽悬空反馈」并循环播放。
        if (!pet->dragAnimStarted &&
            std::abs(pt.x - pet->dragStartPos.x) +
                    std::abs(pt.y - pet->dragStartPos.y) > 6) {
          pet->dragAnimStarted = true;
          const core::ActionSpec dragSpec = core::MakeDragSpec();
          if (!dragSpec.name.empty()) winpetops::PlayAction(*pet, dragSpec);
        }
        // 低通平滑速度，避免单次采样抖动。
        const DWORD now = GetTickCount();
        const DWORD dt = now - pet->dragLastTick;
        if (dt > 0) {
          const double nx = (pt.x - pet->dragLastPos.x) * 1000.0 / dt;
          const double ny = (pt.y - pet->dragLastPos.y) * 1000.0 / dt;
          pet->vx = pet->vx * 0.7 + nx * 0.3;
          pet->vy = pet->vy * 0.7 + ny * 0.3;
        }
        pet->dragLastPos = pt;
        pet->dragLastTick = now;
      }
      return 0;
    case WM_LBUTTONUP:
      if (pet->dragging) {
        pet->dragging = false;
        ReleaseCapture();
        // 松手前停住超过 120ms 就视为没甩。
        const DWORD now = GetTickCount();
        if (now - pet->dragLastTick > 120) {
          pet->vx = 0.0;
          pet->vy = 0.0;
        }
        if (std::fabs(pet->vx) > 80.0 || std::fabs(pet->vy) > 80.0) {
          pet->flying = true;
          Log("pet{} throw v={:.0f},{:.0f}", pet->index, pet->vx, pet->vy);
        }
        // 松手后从拖拽反馈回到加权随机链。
        if (pet->dragAnimStarted) {
          pet->dragAnimStarted = false;
          const core::ActionSpec next = core::PickNextAction(pet->facing);
          if (!next.name.empty()) winpetops::PlayAction(*pet, next);
        }
      }
      return 0;
    case kMsgRequestQuit:
      RequestQuit();
      return 0;
    case WM_RBUTTONUP:
      winui::BuildPetMenu(pet->hwnd, g_sizePercent,
                          pet->sleepPhase != SleepPhase::Awake);
      return 0;
    case kTrayMsg:
      if (LOWORD(lp) == WM_LBUTTONUP) {
        TogglePetsVisible();
      } else if (LOWORD(lp) == WM_RBUTTONUP) {
        ScopedMenu menu(CreatePopupMenu());
        AppendMenuW(menu.get(), MF_STRING, kIdTrayToggle, L"显示/隐藏");
        AppendMenuW(menu.get(), MF_STRING, kIdTrayExit, L"退出");
        POINT pt{};
        GetCursorPos(&pt);
        // 通知区图标的弹出菜单要求前置 + WM_NULL 收尾，否则在部分
        // Windows 版本上菜单不会随失焦自动消失。
        if (SetForegroundWindow(hwnd)) {
          TrackPopupMenu(menu.get(), TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd,
                         nullptr);
          PostMessageW(hwnd, WM_NULL, 0, 0);
        }
      }
      return 0;
    case WM_COMMAND: {
      const int id = LOWORD(wp);
      const int actionBase = static_cast<int>(winui::kIdActionBase);
      if (id >= actionBase &&
          id < actionBase + static_cast<int>(g_menuNames.size())) {
        pet->dragAnimStarted = false;
        winpetops::PlayAction(*pet, core::SpecForName(g_menuNames[id - actionBase]));
      } else if (id == static_cast<int>(winui::kIdExit)) {
        RequestQuit();
      } else if (id == static_cast<int>(winui::kIdAddPet)) {
        CreatePet(g_nextPetId++);
      } else if (id == static_cast<int>(winui::kIdBigger)) {
        int next = g_sizePercent + 5;
        if (next > 40) next = 40;
        winpetops::ApplySizeAll(next);
      } else if (id == static_cast<int>(winui::kIdSmaller)) {
        int next = g_sizePercent - 5;
        if (next < 10) next = 10;
        winpetops::ApplySizeAll(next);
      } else if (id == static_cast<int>(winui::kIdSleepToggle)) {
        winpetops::ToggleSleep(*pet);
      } else if (id == static_cast<int>(winui::kIdClosePet)) {
        DestroyWindow(hwnd);
      } else if (id == static_cast<int>(kIdTrayToggle)) {
        TogglePetsVisible();
      } else if (id == static_cast<int>(kIdTrayExit)) {
        RequestQuit();
      }
      return 0;
    }
    case WM_DESTROY: {
      // 节拍挂在全局窗口上，这里只把宠物标记为 closing 让 TickAll 跳过。
      // 实例按引用计数回收：还有别的宠物在用时解码线程会继续服务它们。
      pet->closing = true;
      // 托盘宿主被关闭时，把托盘迁到另一只仍存活的宠物，
      // 否则剩余宠物会失去显隐/退出入口（托盘只在启动时创建一次）。
      const bool wasTrayOwner = winui::TrayOwner() == hwnd;
      winui::RemoveTray(hwnd);
      if (wasTrayOwner) {
        for (const auto& other : g_pets) {
          Pet* o = other.get();
          if (o == pet || o->closing || o->hwnd == nullptr) continue;
          winui::CreateTray(o->hwnd);
          break;
        }
      }
      if (pet->visible) winclips::SetVisible(pet->clip, false);
      winclips::Release(pet->clip);
      pet->clip = nullptr;
      pet->visible = false;
      // 只有“已注册进 g_pets 的最后一只”被关闭才退出；创建失败时
      // ScopedWindow 析构也会走到这里，但那只宠物从未入列，不能误退进程。
      const bool registered =
          std::any_of(g_pets.begin(), g_pets.end(),
                      [pet](const std::unique_ptr<Pet>& p) {
                        return p.get() == pet;
                      });
      if (registered && g_pets.size() <= 1) PostQuitMessage(0);
      return 0;
    }
    default:
      return DefWindowProcW(hwnd, msg, wp, lp);
  }
}

// 全局节拍：整个进程只此一个 16ms 定时器，遍历所有宠物执行 Tick。
// 启动时顺带分批建宠物 —— CreatePet 内部要等首帧，50 只一口气建完会把
// UI 线程堵住数秒（观感就是"程序未响应"），每 Tick 建一只平滑得多。
LRESULT CALLBACK TickWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg != WM_TIMER) return DefWindowProcW(hwnd, msg, wp, lp);
  if (g_pendingCreates > 0) {
    --g_pendingCreates;
    CreatePet(g_nextPetId++);
  }
  winpetops::TickAll();
  return 0;
}

int Probe(const std::wstring& wide_path, std::ostream& out) {
  const std::string path = ToUtf8(wide_path);
  core::DecodeState dec;
  if (!core::Open(dec, path)) {
    out << "open failed: " << path << '\n';
    return 1;
  }
  out << "codec=" << dec.codec->codec->name << ' ' << dec.videoW << 'x'
      << dec.videoH << " fps=" << std::fixed << std::setprecision(2) << dec.fps
      << " frames=" << dec.clipFrames << '\n';
  if (!core::DecodeNext(dec)) {
    core::Close(dec);
    return 6;
  }
  const char* name =
      av_get_pix_fmt_name(static_cast<AVPixelFormat>(dec.frame->format));
  out << "first frame: " << dec.frame->width << 'x' << dec.frame->height
      << " pix_fmt=" << (name != nullptr ? name : "?") << '\n';
  core::Close(dec);
  return 0;
}

int PetCountFromCommandLine(std::wstring_view args) {
  const size_t pos = args.find(L"--pets=");
  if (pos == std::wstring_view::npos) return 1;
  int value = 0;
  for (const wchar_t c : args.substr(pos + 7)) {
    if (c < L'0' || c > L'9') break;
    value = value * 10 + static_cast<int>(c - L'0');
    // 边解析边钳制：既挡住手误传入的巨量值，也避免超长数字串溢出 int。
    // 注意这里必须是赋值后 break，而不是直接 break —— 直接跳出循环会把
    // 已经累加出来的大值原样返回，上限形同虚设。
    if (value > kMaxPets) {
      value = kMaxPets;
      break;
    }
  }
  return value > 0 ? value : 1;
}

int MonitorChoiceFromCommandLine(std::wstring_view args) {
  const size_t pos = args.find(L"--monitor=");
  if (pos == std::wstring_view::npos) return 0;
  int value = 0;
  for (const wchar_t c : args.substr(pos + 10)) {
    if (c < L'0' || c > L'9') break;
    value = value * 10 + static_cast<int>(c - L'0');
    // 上限 64：显示器不可能这么多，钳制后仍可安全转 int 并落到越界兜底。
    if (value > kMaxPets) {
      value = kMaxPets;
      break;
    }
  }
  return value;  // 0 = 轮询，1 = 第一台
}

}  // namespace

// COM 初始化：DirectComposition 是 COM API，未初始化的线程上
// DCompositionCreateDevice 会失败，而错误框只会显示笼统的「D3D11 init failed」。
class ScopedCom {
 public:
  ScopedCom()
      : ok_(SUCCEEDED(CoInitializeEx(
            nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {}
  ~ScopedCom() {
    if (ok_) CoUninitialize();
  }
  ScopedCom(const ScopedCom&) = delete;
  ScopedCom& operator=(const ScopedCom&) = delete;
  bool ok() const { return ok_; }

 private:
  bool ok_;
};

LONG WINAPI OnCrash(EXCEPTION_POINTERS* ep) {
  // 崩溃上下文里不能走 std::format / ofstream：堆可能已损坏或正持有加载器锁。
  // 用预分配缓冲 + snprintf + WriteFile，并把异常交还 WER 生成转储。
  static char buf[192];
  const int n = std::snprintf(
      buf, sizeof(buf), "CRASH code=0x%08lX addr=%p\n",
      static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode),
      static_cast<const void*>(ep->ExceptionRecord->ExceptionAddress));
  if (n > 0) {
    OutputDebugStringA(buf);
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h != INVALID_HANDLE_VALUE) {
      DWORD written = 0;
      WriteFile(h, buf, static_cast<DWORD>(n), &written, nullptr);
    }
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR lpCmdLine, int) {
  SetUnhandledExceptionFilter(OnCrash);
  const std::wstring_view args =
      lpCmdLine != nullptr ? std::wstring_view(lpCmdLine) : std::wstring_view{};

  // --probe 不依赖定时器精度，先处理并提前退出（不触碰全局 stdout）。
  if (const size_t probePos = args.find(L"--probe");
      probePos != std::wstring_view::npos) {
    std::wstring path(args.substr(probePos + 7));
    while (!path.empty() && (path.front() == L' ' || path.front() == L'"')) {
      path.erase(path.begin());
    }
    while (!path.empty() && (path.back() == L' ' || path.back() == L'"')) {
      path.pop_back();
    }
    std::ofstream out(std::filesystem::path(ExeDir()) / L"probe.txt");
    const int rc = Probe(path, out);
    out << "probe rc=" << rc << '\n';
    return rc;
  }

  ScopedTimerPeriod timerPeriod(1);
  ScopedCom com;
  if (!com.ok()) {
    MessageBoxW(nullptr, L"COM init failed", L"error", MB_ICONERROR);
    return 6;
  }
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  core::log::Init(args.find(L"--debug") != std::wstring_view::npos,
                  &WinLogSink);
  g_monitorChoice = MonitorChoiceFromCommandLine(args);
  if (const size_t clipPos = args.find(L"--clip=");
      clipPos != std::wstring_view::npos) {
    std::wstring name(args.substr(clipPos + 7));
    if (!name.empty() && name.front() == L'"') {
      // --clip="名字 带空格"：引号内整体作为动作名。
      name.erase(name.begin());
      const size_t end = name.find(L'"');
      if (end != std::wstring::npos) name.resize(end);
    } else {
      // 未加引号时截到下一个 -- 参数，而不是在第一个空格处截断。
      const size_t next = name.find(L" --");
      if (next != std::wstring::npos) name.resize(next);
      while (!name.empty() && name.back() == L' ') name.pop_back();
    }
    g_startClip = ToUtf8(name);
  }

  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = kClass;
  if (!RegisterClassExW(&wc)) return 1;

  WNDCLASSEXW tickClass{sizeof(tickClass)};
  tickClass.lpfnWndProc = TickWndProc;
  tickClass.hInstance = hInst;
  tickClass.lpszClassName = kTickClass;
  if (!RegisterClassExW(&tickClass)) return 1;

  core::LoadConfig(ConfigPath());
  g_sizePercent = core::g_initialSizePercent;
  Log("config: byName={} idle={} clicks={} moves={} size={}%", g_byName.size(),
      g_idleClips.size(), g_clickClips.size(), g_moves.size(), g_sizePercent);
  // 预填动作名表（顺序与 BuildPetMenu 的 addGroup 一致），右键打开菜单时会重建；
  // 预填后外部脚本可以用 WM_COMMAND 直接点播动作。
  g_menuNames.clear();
  auto preload = [](const std::vector<std::string>& items) {
    for (const auto& n : items) g_menuNames.push_back(n);
  };
  preload(g_idleClips);
  preload(g_turnClips);
  preload(g_dragClips);
  preload(g_clickClips);
  preload(g_moveNames);
  for (const auto& cat : g_categories) preload(cat.second);
  for (const auto& ev : g_eventGroups) preload(ev.second);
  if (!RenderInitShared()) {
    MessageBoxW(nullptr, L"D3D11 init failed", L"error", MB_ICONERROR);
    return 2;
  }
  // 隐藏的消息窗口承载唯一节拍；HWND_MESSAGE 保证它不显示、不进任务栏。
  g_tickHwnd = CreateWindowExW(0, kTickClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                               nullptr, hInst, nullptr);
  if (g_tickHwnd == nullptr) {
    MessageBoxW(nullptr, L"create tick window failed", L"error", MB_ICONERROR);
    return 4;
  }

  if (!CreatePet(g_nextPetId++)) {
    MessageBoxW(nullptr, L"create pet failed", L"error", MB_ICONERROR);
    return 3;
  }
  int count = PetCountFromCommandLine(args);
  if (count < 1) count = 1;
  // 首只已经建好，剩下的交给节拍分批创建，避免启动瞬间长时间无响应。
  g_pendingCreates = count - 1;
  Log("create {} pets (limit {})", count, kMaxPets);
  if (SetTimer(g_tickHwnd, 1, 16, nullptr) == 0) {
    MessageBoxW(nullptr, L"start tick timer failed", L"error", MB_ICONERROR);
    return 5;
  }
  if (!g_pets.empty()) winui::CreateTray(g_pets[0]->hwnd);

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
    // 窗口销毁后回收对应宠物（解码线程已在 WM_DESTROY 里 join）。
    g_pets.erase(
        std::remove_if(g_pets.begin(), g_pets.end(),
                       [](const std::unique_ptr<Pet>& p) {
                         return p->closing;
                       }),
        g_pets.end());
  }
  // 关掉节拍窗口再回收：PostQuitMessage 路径下它仍然活着。
  if (g_tickHwnd != nullptr) {
    KillTimer(g_tickHwnd, 1);
    DestroyWindow(g_tickHwnd);
    g_tickHwnd = nullptr;
  }
  // 兜底：不管从哪条路径退出，都先 join 解码线程再析构，
  // 否则 joinable 的 std::thread 在静态析构阶段会触发 terminate。
  winclips::StopAll();
  g_pets.clear();
  return 0;  // ScopedTimerPeriod 析构时配对 timeEndPeriod
}

// MinGW/UCRT64 的 GUI 启动调用 WinMain；MSVC 直接使用 wWinMain。
#ifndef _MSC_VER
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR, int nShow) {
  return wWinMain(hInst, hPrev, GetCommandLineW(), nShow);
}
#endif
