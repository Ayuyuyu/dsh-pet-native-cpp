#ifndef NOMINMAX
#define NOMINMAX  // 避免 windows.h 的 min/max 宏破坏 std::max
#endif
#include "platform/win/win_pet_ops.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <dwmapi.h>

#include "core/decode.h"
#include "core/log.h"
#include "core/mask.h"
#include "core/util.h"
#include "platform/win/render_d3d11.h"
#include "platform/win/win_clip_pool.h"
#include "platform/win/win_util.h"

namespace winpetops {

// ===== 窗口几何 =====

core::MonitorInfo MonitorFor(HWND hwnd) {
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
  core::MonitorInfo out;
  // 四边用工作区：走路时角色身体贴住任务栏上沿而不是跑进任务栏。
  out.left = mi.rcWork.left;
  out.top = mi.rcWork.top;
  out.right = mi.rcWork.right;
  // bottom 的语义是物理显示器底边（rcMonitor），不能与 workBottom 混为一谈；
  // 当前物理计算只读 workBottom，这里仍按字段定义正确赋值。
  out.bottom = mi.rcMonitor.bottom;
  out.workBottom = mi.rcWork.bottom;
  return out;
}

void CollectOthers(const winpet::Pet& self, std::vector<core::Rect>* out) {
  out->clear();
  for (const auto& o : winpet::g_pets) {
    if (o.get() == &self || o->hwnd == nullptr || o->closing) continue;
    out->push_back(o->screenBody);
  }
  // StepWalk 对少量宠物线性扫描即可；不再为「提前退出」维护排序与双 API。
}

void SyncWindowPos(winpet::Pet& pet) {
  RECT rc{};
  if (pet.hwnd == nullptr || !GetWindowRect(pet.hwnd, &rc)) return;
  pet.winX = rc.left;
  pet.winY = rc.top;
  pet.screenBody = core::Rect{rc.left + pet.bodyL, rc.top + pet.bodyT,
                              rc.left + pet.bodyR, rc.top + pet.bodyB};
}

void ApplySize(winpet::Pet& pet, int percent, bool resizePool) {
  HMONITOR mon = MonitorFromWindow(pet.hwnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{sizeof(mi)};
  if (!GetMonitorInfoW(mon, &mi)) {
    core::log::Log("pet{} GetMonitorInfoW failed, skip resize", pet.index);
    return;
  }
  const int workW = mi.rcWork.right - mi.rcWork.left;
  int targetW = workW * percent / 100;
  if (targetW < 220) targetW = 220;
  if (targetW > 640) targetW = 640;
  const int targetH = targetW * 360 / 640;

  RECT rc{};
  if (!GetWindowRect(pet.hwnd, &rc)) {
    core::log::Log("pet{} GetWindowRect failed, skip resize", pet.index);
    return;
  }
  const int nx = (rc.left + rc.right) / 2 - targetW / 2;
  const int ny = (rc.top + rc.bottom) / 2 - targetH / 2;

  const bool sizeChanged = pet.windowW != targetW || pet.windowH != targetH;
  if (!RenderResize(pet.rt, targetW, targetH)) {
    core::log::Log("pet{} resize failed", pet.index);
    return;
  }
  pet.windowW = targetW;
  pet.windowH = targetH;
  pet.needPresent = true;
  pet.forceRegionFrames = 4;  // 缩放后按新尺寸重建区域
  // 缩放要立即反映到解码输出上：否则放大后会用旧分辨率的纹理拉伸显示，
  // 一直模糊到下一次切段。这里原地重建 swscale 目标，不重开解码器。
  if (sizeChanged && resizePool) winclips::ResizeAll(targetW, targetH);
  SetWindowPos(pet.hwnd, nullptr, nx, ny, targetW, targetH,
               SWP_NOZORDER | SWP_NOACTIVATE);
  SyncWindowPos(pet);
  SyncBodyFromClip(pet);
  core::log::Log("pet{} resize {}% {}x{}", pet.index, percent, targetW, targetH);
}

void ApplySizeAll(int percent) {
  winpet::g_sizePercent = percent;
  for (const auto& pet : winpet::g_pets) ApplySize(*pet, percent, false);
  // 整池只重建一次纹理：放在循环里会让每只宠物的缩放都 Stop/Start
  // 其他宠物正在用的解码实例，多宠物同步的相位会被反复打乱。
  if (!winpet::g_pets.empty()) {
    winclips::ResizeAll(winpet::g_pets.front()->windowW,
                        winpet::g_pets.front()->windowH);
  }
}

// ===== 每帧管线 =====

void SyncBodyFromClip(winpet::Pet& pet) {
  const winclips::ClipInstance* inst = pet.clip;
  if (inst == nullptr || inst->outW <= 0 || inst->outH <= 0) return;
  const core::MaskResult& m = inst->mask;
  // 实例输出尺寸 → 宠物窗口尺寸（缩放后两者可能不同）。
  const int l = m.bodyL * pet.windowW / inst->outW;
  const int r = m.bodyR * pet.windowW / inst->outW;
  const int t = m.bodyT * pet.windowH / inst->outH;
  const int b = m.bodyB * pet.windowH / inst->outH;
  if (pet.flip) {
    pet.bodyL = pet.windowW - r;
    pet.bodyR = pet.windowW - l;
  } else {
    pet.bodyL = l;
    pet.bodyR = r;
  }
  pet.bodyT = t;
  pet.bodyB = b;
  pet.screenBody = core::Rect{pet.winX + pet.bodyL, pet.winY + pet.bodyT,
                              pet.winX + pet.bodyR, pet.winY + pet.bodyB};
}

bool HitTest(const winpet::Pet& pet, POINT client) {
  const winclips::ClipInstance* inst = pet.clip;
  // 实例还没就绪（首帧之前）时按「整窗可点」处理，避免刚创建就点不到。
  if (inst == nullptr) return true;
  const int mw = winclips::Tex(*inst).maskW;
  const int mh = winclips::Tex(*inst).maskH;
  const auto& mask = inst->mask.mask;
  const size_t count = static_cast<size_t>(mw) * mh;
  if (mw <= 0 || mh <= 0 || mask.size() < count || pet.windowW <= 0 ||
      pet.windowH <= 0) {
    return true;
  }
  int x = client.x * mw / pet.windowW;
  int y = client.y * mh / pet.windowH;
  if (x < 0 || y < 0 || x >= mw || y >= mh) return false;
  // 共享掩码是 flip=false 算的，镜像动作用时查镜像列。
  if (pet.flip) x = mw - 1 - x;

  // 严格按可见像素命中：透明区一律 HTTRANSPARENT，下面的窗口照常可点。
  // 命中不做额外外扩——BuildMask 的 expand 只作用于窗口区域矩形，
  // 不影响这里的逐像素判断；跨进程窗口靠 UpdateWindowRegion 的
  // SetWindowRgn 真正穿透。
  return mask[static_cast<size_t>(y) * mw + x] != 0;
}

void UpdateWindowRegion(winpet::Pet& pet) {
  const winclips::ClipInstance* inst = pet.clip;
  if (inst == nullptr || pet.hwnd == nullptr) return;
  const int mw = winclips::Tex(*inst).maskW;
  const int mh = winclips::Tex(*inst).maskH;
  if (mw <= 0 || mh <= 0 || pet.windowW <= 0 || pet.windowH <= 0) return;
  const std::vector<char>& mask = inst->mask.mask;
  const size_t count = static_cast<size_t>(mw) * mh;
  const bool maskReady = mask.size() >= count;

  // 节流：掩码几乎没变时跳过 SetWindowRgn。
  // 与画面同源（都在 ConsumeFrame 拿到新帧后调用），差异只在「呼吸这类
  // 亚像素摆动」上，不会出现旧实现那种 region 严重滞后把身体切掉的情况。
  const bool sizeChanged = pet.regionW != mw || pet.regionH != mh;
  bool needUpdate = !maskReady || sizeChanged || pet.forceRegionFrames > 0 ||
                    pet.regionMask.size() != count;
  if (!needUpdate) {
    size_t diff = 0;
    for (size_t i = 0; i < count; ++i) {
      if (mask[i] != pet.regionMask[i]) ++diff;
    }
    // 0.8% 像素变化即重建：毛发/发丝这类细边缘的抖动比整体轮廓小，
    // 2% 阈值时 region 会明显滞后于画面，表现为毛发被切掉一块。
    needUpdate = diff * 125 > count;
  }
  if (!needUpdate) return;
  if (pet.forceRegionFrames > 0) --pet.forceRegionFrames;

  // 逐行游程 → 窗口坐标矩形，并把垂直相邻、x 范围相同的段合并成一条，
  // 500 个逐行矩形通常能压到 20~40 个，ExtCreateRegion 一次搞定。
  std::vector<RECT> rects;
  if (maskReady) {
    rects.reserve(64);
    for (int y = 0; y < mh; ++y) {
      const char* row = mask.data() + static_cast<size_t>(y) * mw;
      int x = 0;
      while (x < mw) {
        if (row[x] == 0) {
          ++x;
          continue;
        }
        const int start = x;
        while (x < mw && row[x] != 0) ++x;
        int l = start * pet.windowW / mw;
        int r = x * pet.windowW / mw;
        if (r <= l) continue;
        // region 比可见像素外扩 1px：覆盖 alpha 1~4 的半透明毛发边缘。
        if (l > 0) --l;
        if (r < pet.windowW) ++r;
        if (pet.flip) {
          const int ml = pet.windowW - r;
          r = pet.windowW - l;
          l = ml;
        }
        const int t = y > 0 ? y * pet.windowH / mh - 1 : 0;
        const int bottom = (y + 1) * pet.windowH / mh + 1;  // +1 消除行间缝隙
        const int b = bottom < pet.windowH ? bottom : pet.windowH;
        if (!rects.empty() && rects.back().left == l && rects.back().right == r &&
            rects.back().bottom >= t) {
          rects.back().bottom = b;
        } else {
          rects.push_back(RECT{l, t, r, b});
        }
      }
    }
  }
  // 全透明帧必须下发空区域：否则窗口保留上一帧的可点区域，
  // 出现「画面已消失却仍然拦截鼠标」的错位。
  const size_t bytes = sizeof(RGNDATAHEADER) + rects.size() * sizeof(RECT);
  std::vector<char> buf(bytes);
  auto* rd = reinterpret_cast<RGNDATA*>(buf.data());
  rd->rdh.dwSize = sizeof(RGNDATAHEADER);
  rd->rdh.iType = RDH_RECTANGLES;
  rd->rdh.nCount = static_cast<DWORD>(rects.size());
  rd->rdh.nRgnSize = static_cast<DWORD>(rects.size() * sizeof(RECT));
  rd->rdh.rcBound = RECT{0, 0, pet.windowW, pet.windowH};
  if (!rects.empty()) {
    std::memcpy(rd->Buffer, rects.data(), rects.size() * sizeof(RECT));
  }

  HRGN hrgn = rects.empty() ? CreateRectRgn(0, 0, 0, 0)
                            : ExtCreateRegion(nullptr, static_cast<DWORD>(bytes), rd);
  if (hrgn == nullptr) return;
  // SetWindowRgn 成功后系统接管 hrgn；失败时我们自己释放。
  if (SetWindowRgn(pet.hwnd, hrgn, TRUE) == 0) {
    DeleteObject(hrgn);
    return;
  }
  pet.regionW = mw;
  pet.regionH = mh;
  pet.regionMask = maskReady ? mask : std::vector<char>{};
}

void UpdateSquash(winpet::Pet& pet) {
  // Q 弹曲线是平台无关状态，绘制交给渲染层。
  if (!pet.squashActive) return;
  const float u = static_cast<float>(GetTickCount() - pet.squashStart) / 220.0f;
  if (u >= 1.0f) {
    pet.squashActive = false;
    pet.scaleY = 1.0f;
    return;
  }
  pet.scaleY = core::SquashCurve(u);
}

void Present(winpet::Pet& pet) {
  if (pet.clip == nullptr) return;
  if (!RenderFrame(pet.rt, winclips::Tex(*pet.clip), pet.windowW, pet.windowH,
                   pet.scaleY,
                   pet.flip, core::log::Enabled())) {
    // 失败（设备丢失/交换链正在恢复）时保留 needPresent，等下一次机会重试。
    return;
  }
  pet.needPresent = false;
}

// 共享实例每帧只换一次帧，同 clip 的所有宠物看到同一个 frameStamp，
// 因此「本帧是否有新画面」由实例决定，而不是各自累加。
// 视频时钟直接取共享实例的 frameStamp / playedFrames；moves 使用独占实例，
// 从首帧开始播放，无需「相对加入点」的偏移换算。
bool ConsumeFrame(winpet::Pet& pet) {
  if (pet.clip == nullptr) return false;
  const unsigned stamp = pet.clip->frameStamp;
  if (stamp == pet.lastStamp) return false;
  pet.lastStamp = stamp;
  SyncBodyFromClip(pet);
  pet.playedFrames = pet.clip->playedFrames;
  // 区域与新帧同源更新，不会滞后于画面（旧实现的「头部被切」就是区域滞后）。
  UpdateWindowRegion(pet);
  return stamp > 0;
}

// ===== 动作状态机 =====

namespace {

core::ActionState StateOf(const winpet::Pet& pet) {
  core::ActionState st;
  st.spec = pet.spec;
  st.sleepPhase = pet.sleepPhase;
  st.facing = pet.facing;
  st.walkFlipped = pet.walkFlipped;
  st.dragMode = pet.dragMode;
  st.nextClipTick = pet.nextClipTick;
  st.clipFrames = pet.clipFrames;
  st.playedFrames = pet.playedFrames;
  st.playStartTick = pet.playStartTick;
  st.playDurationMs = pet.playDurationMs;
  return st;
}

void ApplyStateToPet(winpet::Pet& pet, const core::ActionState& st) {
  pet.spec = st.spec;
  pet.sleepPhase = st.sleepPhase;
  pet.facing = st.facing;
  pet.walkFlipped = st.walkFlipped;
  pet.dragMode = st.dragMode;
  pet.nextClipTick = st.nextClipTick;
}

}  // namespace

bool PlayClip(winpet::Pet& pet, const std::string& name) {
  const auto it = core::g_byName.find(name);
  if (it == core::g_byName.end()) return false;
  const bool loop = pet.spec.loop;
  // 移动/转身必须有完整时间轴：复用一段播到一半的实例会让宠物只走几步就被
  // 切段（转身还会出现画面没转完、facing 已经翻了的错位）。这两类独占实例。
  const bool exclusive = pet.spec.kind == core::ActionKind::Move ||
                         pet.spec.kind == core::ActionKind::Turn;
  winclips::ClipInstance* inst = winclips::Acquire(
      name, winutil::ClipsRootUtf8() + it->second, loop, exclusive, pet.windowW,
      pet.windowH);
  if (inst == nullptr) return false;

  // 先接新实例再放旧的：其余宠物可能还在用同一个实例。
  winclips::ClipInstance* old = pet.clip;
  pet.clip = inst;
  pet.lastStamp = 0;  // 首帧还没被本宠物消费，下一次 Tick 必定重绘
  pet.currentClip = name;
  pet.frameCount = 0;
  pet.clipFrames = inst->clipFrames;
  const double fps = inst->fps > 0 ? inst->fps : 30.0;
  pet.playDurationMs = static_cast<int>(pet.clipFrames * 1000.0 / fps);
  pet.playStartTick = GetTickCount();

  // 朝向要在首帧上传之前定好：镜像动作的第一帧（转身起手）不能沿用旧朝向。
  const auto mv = core::g_moves.find(name);
  // 非移动动作按全局持久 facing 显示（Turn 播完会真正转身），
  // 移动动作的视觉方向必须取自自身 flip 配置，不能继承上次 Turn 残留的 facing。
  pet.flip = (mv != core::g_moves.end()) ? (mv->second.flip != 0)
                                         : (pet.facing == -1);
  pet.moveActive = false;
  pet.moveSpeedPxPerFrame = 0.0;
  if (mv != core::g_moves.end()) {
    pet.moveDirX = mv->second.dirX;
    pet.moveDirY = mv->second.dirY;
    if (pet.moveDirX == 0 && pet.moveDirY == 0) {
      // 老版：未指定方向时随机单轴，上下左右四选一。
      switch (core::RandomInt(0, 3)) {
        case 0: pet.moveDirX = 1; break;
        case 1: pet.moveDirX = -1; break;
        case 2: pet.moveDirY = 1; break;
        default: pet.moveDirY = -1; break;
      }
    }
    const int lo = mv->second.minDist;
    const int hi = mv->second.maxDist > lo ? mv->second.maxDist : lo;
    const int logicalDist = core::RandomInt(lo, hi);
    // 配置距离是逻辑像素，按 DPI 换算成物理像素。
    const UINT dpi = GetDpiForWindow(pet.hwnd);
    pet.moveTotal =
        MulDiv(logicalDist, dpi > 0 ? static_cast<int>(dpi) : 96, 96);
    pet.moveDone = 0;
    // lead/tail/速度由 core 统一计算，两端不再各自实现 1.8 系数。
    const core::MoveTiming timing =
        core::ComputeMoveTiming(mv->second, fps, pet.clipFrames, pet.moveTotal);
    if (timing.moveFrames > 0 && pet.moveTotal > 0) {
      pet.moveActive = true;
      pet.moveLeadFrames = timing.leadFrames;
      pet.moveLastFrames = 0;
      pet.moveSpeedPxPerFrame = timing.speedPxPerFrame;
      core::log::Log(
          "pet{} walk start dist={} dir={},{} lead={} move={} clip={} frames",
          pet.index, pet.moveTotal, pet.moveDirX, pet.moveDirY,
          timing.leadFrames, timing.moveFrames, pet.clipFrames);
    }
  }

  SyncBodyFromClip(pet);
  pet.playedFrames = inst->playedFrames;
  pet.needPresent = true;
  pet.forceRegionFrames = 4;  // 换动作后前几帧强制重建，避免旧区域裁掉新动作
  // 换实例时把「可见登记」从旧实例迁到新实例：遮挡中的宠物不解码，
  // 它加入的实例如果只有它一个引用者，线程会一直停着等它露出来。
  if (old != nullptr) {
    if (pet.visible) winclips::SetVisible(old, false);
    winclips::Release(old);
  }
  pet.visible = false;
  if (!pet.occluded) {
    winclips::SetVisible(inst, true);
    pet.visible = true;
  }

  core::log::Log("pet{} play clip: {} loop={} instances={}", pet.index, name,
                 loop ? 1 : 0, winclips::ActiveCount());
  return true;
}

bool PlayAction(winpet::Pet& pet, const core::ActionSpec& spec) {
  if (spec.name.empty()) return false;
  // 所有派生状态的提交与回滚统一由 core::ActionMachine 决策：
  // 只回滚 spec 会让 dragMode/loop 与实际实例不符，或让 facing 残留镜像。
  core::ActionState st = StateOf(pet);
  const core::ActionState snapshot = core::SnapshotAction(st);
  core::ApplyActionSpec(st, spec);
  ApplyStateToPet(pet, st);
  if (!PlayClip(pet, spec.name)) {
    core::RestoreAction(st, snapshot);
    ApplyStateToPet(pet, st);
    pet.nextClipTick = GetTickCount() + 1000;
    return false;
  }
  return true;
}

void OnClipEnded(winpet::Pet& pet, DWORD nowMs, bool stalled) {
  core::ActionState st = StateOf(pet);
  const core::ActionSpec next = core::NextActionAfterEnd(st, nowMs, stalled);
  ApplyStateToPet(pet, st);  // turn 翻转与睡觉阶段切换在这里写回
  if (next.name.empty()) return;
  if (!PlayAction(pet, next)) {
    // 打开失败：保留旧实例，1 秒后退避重试。绝不能把 pet.clip 置空。
    pet.nextClipTick = nowMs + 1000;
    core::log::Log("pet{} switch to {} failed, retry in 1s", pet.index,
                   next.name);
  }
}

void ToggleSleep(winpet::Pet& pet) {
  const bool sleeping = pet.sleepPhase == winpet::SleepPhase::Start ||
                        pet.sleepPhase == winpet::SleepPhase::Loop;
  core::ActionSpec spec;
  spec.kind = core::ActionKind::Sleep;
  if (sleeping) {
    spec.name = core::PickFromPool(core::g_sleepClips.end);
    pet.sleepPhase = winpet::SleepPhase::End;
  } else {
    spec.name = core::PickFromPool(core::g_sleepClips.start);
    pet.sleepPhase = winpet::SleepPhase::Start;
  }
  // PlayAction 对 Sleep 类型不会清状态；失败时才回滚，避免卡在中间态。
  if (spec.name.empty() || !PlayAction(pet, spec)) {
    pet.sleepPhase = winpet::SleepPhase::Awake;
    core::log::Log("pet{} sleep toggle failed (clips={})", pet.index,
                   static_cast<int>(core::g_sleepClips.start.size()));
    return;
  }
  core::log::Log("pet{} sleep {}", pet.index, sleeping ? "wake" : "sleep");
}

// ===== 每帧 Tick =====

namespace {

// 甩出去的惯性飞行：边界/重力在 core/physics，宠物间碰撞按老版规则在平台层。
void StepFly(winpet::Pet& pet, const core::MonitorInfo& mon) {
  const DWORD now = GetTickCount();
  const double dt = 0.016;  // 固定步长
  int x = pet.winX;
  int y = pet.winY;
  bool bounced = false;
  bool stopped = false;
  // 窗口范围（body 贴住工作区反推）。
  core::WindowBounds bounds;
  bounds.minX = mon.left - pet.bodyL;
  bounds.maxX = mon.right - pet.bodyR;
  bounds.minY = mon.top - pet.bodyT;
  bounds.maxY = mon.workBottom - pet.bodyB; 
  core::StepFly(&pet.vx, &pet.vy, &x, &y, bounds, dt, &bounced, &stopped);
  SetWindowPos(pet.hwnd, nullptr, x, y, 0, 0,
               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  SyncWindowPos(pet);

  // 老版：碰撞发生在位置提交之后，用当前身体框重叠判定；800ms 冷却 +
  // 速度阈值，撞完自己减速、对方获得固定冲量并进入 flying。
  if (now - pet.lastCollisionTick > 800 &&
      (std::fabs(pet.vx) + std::fabs(pet.vy)) > 150.0) {
    // 读缓存矩形，不再逐个 GetWindowRect。
    const core::Rect self = pet.screenBody;
    const int selfCx = x + pet.windowW / 2;
    for (const auto& candidate : winpet::g_pets) {
      winpet::Pet* other = candidate.get();
      if (other == &pet || other->hwnd == nullptr || other->closing) continue;
      if (!core::RectsOverlap(self, other->screenBody)) continue;
      const int otherCx = other->winX + other->windowW / 2;
      const int dir = selfCx < otherCx ? 1 : -1;
      pet.vx = -pet.vx * 0.6;
      pet.vy = -pet.vy * 0.4;
      other->vx = static_cast<double>(dir) * 450.0;
      other->vy = -220.0;
      pet.lastCollisionTick = other->lastCollisionTick = now;
      if (!other->flying) {
        other->flying = true;
      }
      core::log::Log("pet{} fly hit pet{} dir={}", pet.index, other->index,
                     dir);
      break;
    }
  }

  if (stopped) {
    pet.flying = false;
    pet.vx = 0.0;
    pet.vy = 0.0;
  }
  if (bounced) core::log::Log("pet{} fly bounce", pet.index);
}

// 视频帧时钟：转身段（leadFrames）原地，之后每呈现一帧走固定像素，
// 走完 dist 立刻停（tail 段静止）。平移严格跟着画面里的「跑」。
void StepMove(winpet::Pet& pet, int frames, const core::MonitorInfo& mon,
              const std::vector<core::Rect>& others) {
  if (frames < pet.moveLeadFrames) {
    pet.moveLastFrames = frames;
    return;
  }
  const int dFrames = frames - pet.moveLastFrames;
  pet.moveLastFrames = frames;
  if (dFrames <= 0) return;

  int step = static_cast<int>(pet.moveSpeedPxPerFrame * dFrames);
  if (step < 1) step = 1;
  const int remain = pet.moveTotal - pet.moveDone;
  if (step > remain) step = remain;
  if (step <= 0) return;

  // 位置取自碰撞缓存（每次 SetWindowPos 后同步），不再每帧 GetWindowRect。
  // body 四边都是窗口内偏移；StepWalk 自己加上 curX/curY 得到屏幕矩形。
  core::Rect body{pet.bodyL, pet.bodyT, pet.bodyR, pet.bodyB};
  // 动作的首帧/过渡帧可能几乎全透明，掩码只剩顶部一条，body 高度随之变得
  // 极小 —— 边界会按「body 贴住屏幕」反推出一个远超屏幕的位置，宠物能一路
  // 走到工作区之外，被遮挡暂停后用户就再也找不到了。可见区域过小时用整窗兜底。
  if (body.r - body.l < pet.windowW / 4 || body.b - body.t < pet.windowH / 4) {
    body = core::Rect{0, 0, pet.windowW, pet.windowH};
  }
  int nx = pet.winX;
  int ny = pet.winY;
  bool hit_edge = false;
  bool hit_pet = false;
  core::StepWalk(pet.winX, pet.winY, step, pet.moveDirX, pet.moveDirY, body,
                 mon, others, &nx, &ny, &hit_edge, &hit_pet);
  const bool collided = hit_edge || hit_pet;
  if (collided) {
    // 撞边/撞宠物都按老版回弹：位移方向反向一次，视频镜像/朝向不变。
    // 剩余距离 ×0.6，太小就停，所以只会回弹一小段。
    const int left = pet.moveTotal - pet.moveDone;
    const int dx = nx - pet.winX;
    const int dy = ny - pet.winY;
    pet.moveDirX = -pet.moveDirX;
    pet.moveDirY = -pet.moveDirY;
    pet.moveDone = 0;
    pet.moveTotal = static_cast<int>(left * 0.6);
    if (pet.moveTotal < 20) {
      pet.moveActive = false;
      pet.moveSpeedPxPerFrame = 0.0;
    }
    core::log::Log(
        "pet{} walk bounce {} dir={},{} flip={} d={},{} pos={},{} total={}",
        pet.index, hit_edge ? "wall" : "pet", pet.moveDirX, pet.moveDirY,
        pet.flip ? 1 : 0, dx, dy, nx, ny, pet.moveTotal);
  }
  SetWindowPos(pet.hwnd, nullptr, nx, ny, 0, 0,
               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  SyncWindowPos(pet);
  if (!collided) {
    pet.moveDone += step;
    if (pet.moveDone >= pet.moveTotal) {
      pet.moveActive = false;
      core::log::Log("pet{} walk stop done={}", pet.index, pet.moveDone);
    }
  }
}

// 视频播完才切下一段。
// ended 由池在实例级别统一置位，引用它的宠物各自切各自的下一段。
void CheckClipEnd(winpet::Pet& pet) {
  const DWORD nowMs = GetTickCount();
  // 退避期内不重试：切段失败时每秒重试一次即可，不必每帧都去 Open 文件。
  if (nowMs < pet.nextClipTick) return;

  if (pet.clip == nullptr) {
    // 实例丢失（正常切段失败会保留旧实例，这里仅作防御性兜底）：
    // 1 秒后重新挑一段，否则直接 return 会让宠物永久定格。
    pet.nextClipTick = nowMs + 1000;
    core::ActionSpec next = core::PickNextAction(pet.facing);
    if (next.name.empty()) next = core::IdleSpec();
    core::log::Log("pet{} has no clip, retry: {}", pet.index, next.name);
    PlayAction(pet, next);
    return;
  }
  // loop 由解码线程自行回绕，实例永远不会 ended，状态机也不该切段。
  if (pet.spec.loop) return;
  const bool stalled =
      pet.playDurationMs > 0 &&
      nowMs - pet.playStartTick >
          static_cast<DWORD>(pet.playDurationMs * 2 + 2000);
  if (pet.clip->ended || stalled) OnClipEnded(pet, nowMs, stalled);
}

// 找出 Z 序更高、且真正盖住该点的窗口；没有则返回 nullptr。
// 不用 WindowFromPoint：它依赖窗口的合成表面，DComp（WS_EX_NOREDIRECTIONBITMAP）
// 窗口在内容异常时会被直接跳过，进而「判定遮挡 → 不重绘 → 内容更不恢复」死锁。
HWND FindCoverWindow(HWND self, int x, int y) {
  for (HWND h = GetWindow(self, GW_HWNDPREV); h != nullptr;
       h = GetWindow(h, GW_HWNDPREV)) {
    if (!IsWindowVisible(h) || IsIconic(h)) continue;
    const LONG_PTR exStyle = GetWindowLongPtrW(h, GWL_EXSTYLE);
    // 点击穿透的覆盖层不构成视觉遮挡。
    if ((exStyle & WS_EX_TRANSPARENT) != 0) continue;
    // UWP / 开始菜单等 cloaked 窗口在 Z 序里可见但没有任何像素，不能算遮挡。
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked,
                                        sizeof(cloaked))) &&
        cloaked) {
      continue;
    }
    // 录屏/截图工具常用全透明 layered 覆盖层，同样不构成视觉遮挡。
    if ((exStyle & WS_EX_LAYERED) != 0) {
      BYTE alpha = 255;
      COLORREF key = 0;
      DWORD flags = 0;
      if (SUCCEEDED(GetLayeredWindowAttributes(h, &key, &alpha, &flags)) &&
          (flags & LWA_ALPHA) != 0 && alpha == 0) {
        continue;
      }
    }

    // 自己家的宠物：窗口矩形比角色大得多（动画在 X 轴上只占中间一小条），
    // 两侧透明区域不能算遮挡，必须用实际可见区域的缓存矩形。
    const winpet::Pet* coverPet = nullptr;
    for (const auto& p : winpet::g_pets) {
      if (p->hwnd == h) {
        coverPet = p.get();
        break;
      }
    }
    int l = 0;
    int t = 0;
    int r = 0;
    int b = 0;
    if (coverPet != nullptr) {
      l = coverPet->screenBody.l;
      t = coverPet->screenBody.t;
      r = coverPet->screenBody.r;
      b = coverPet->screenBody.b;
    } else {
      RECT rc{};
      if (!GetWindowRect(h, &rc)) continue;
      l = rc.left;
      t = rc.top;
      r = rc.right;
      b = rc.bottom;
    }
    if (x >= l && x < r && y >= t && y < b) return h;
  }
  return nullptr;
}

void UpdateOcclusion(winpet::Pet& pet) {
  if (pet.hwnd == nullptr) return;

  // 探测点优先取角色可见区域内部；首帧前或可见区域异常小时退回窗口中心。
  // body 有效时用它，才能反映「角色本身被盖住」而不是「窗口矩形相交」。
  int cx = 0;
  int py[3]{};
  const int bodyW = pet.bodyR - pet.bodyL;
  const int bodyH = pet.bodyB - pet.bodyT;
  if (pet.clip != nullptr && bodyW >= pet.windowW / 4 &&
      bodyH >= pet.windowH / 4) {
    cx = pet.winX + (pet.bodyL + pet.bodyR) / 2;
    py[0] = pet.winY + pet.bodyT + bodyH * 3 / 10;
    py[1] = pet.winY + pet.bodyT + bodyH / 2;
    py[2] = pet.winY + pet.bodyT + bodyH * 7 / 10;
  } else {
    cx = pet.winX + pet.windowW / 2;
    py[0] = pet.winY + pet.windowH * 3 / 10;
    py[1] = pet.winY + pet.windowH / 2;
    py[2] = pet.winY + pet.windowH * 7 / 10;
  }

  // 三个探测点全部被盖住才算遮挡；只要有一个露出来就算可见。
  HWND cover = nullptr;
  bool anyVisible = false;
  for (int i = 0; i < 3; ++i) {
    cover = FindCoverWindow(pet.hwnd, cx, py[i]);
    if (cover == nullptr) {
      anyVisible = true;
      break;
    }
  }
  const bool visible = anyVisible;
  if (visible == !pet.occluded) return;  // 状态没变
  pet.occluded = !visible;
  // 从遮挡恢复时强制重绘一次：遮挡期间没有 Present，若此时也没有新帧，
  // 画面会停在旧内容上（或因内容丢失而全透明）。
  if (visible) pet.needPresent = true;
  core::log::Log("pet{} occlusion {} cover=0x{:x} (clip={} visibleRefs={})",
                 pet.index, visible ? "visible" : "hidden",
                 static_cast<unsigned long long>(
                     visible ? 0 : reinterpret_cast<uintptr_t>(cover)),
                 pet.clip != nullptr ? 1 : 0,
                 pet.clip != nullptr ? pet.clip->visibleRefs : -1);
  if (pet.clip == nullptr) return;
  // 实例级引用计数：所有引用者都遮挡时才真正停解码线程。
  winclips::SetVisible(pet.clip, visible);
  pet.visible = visible;
}

}  // namespace

void Tick(winpet::Pet& pet) {
  // 遮挡检测 1.5s 一次；不可见时也要继续跑，否则上层的宠物走开后
  // 它永远醒不过来（画面与位置都会永久冻结）。
  const DWORD now = GetTickCount();
  if (now - pet.lastOcclusionTick >= 1500) {
    pet.lastOcclusionTick = now;
    UpdateOcclusion(pet);
  }
  // 被完全盖住：不解码、不推进物理、不重绘，窗口保留最后一帧。
  if (pet.occluded) return;

  // 一致性兜底：宠物认为自己可见，但实例的 visibleRefs 为 0，
  // 说明引用计数漏了一次 —— 此时 UpdateAll 会跳过该实例，frameStamp 不再
  // 增长，Present 永不触发，表现出来就是「画面卡在某段动作不动」。
  // 每帧一次整数比较的代价，换掉整类永久定格。
  if (pet.clip != nullptr && !pet.visible && pet.clip->visibleRefs == 0) {
    winclips::SetVisible(pet.clip, true);
    pet.visible = true;
    core::log::Log("pet{} revive clip (visibleRefs was 0)", pet.index);
  }

  // 单 16ms Tick（与 Linux 后端同构）：取帧 → 物理/切段 → Present。
  // 物理用固定 dt、移动按视频帧 dFrames 推进，都不依赖墙钟。
  const bool got_frame = ConsumeFrame(pet);

  // 连续 3 秒没有新帧时把完整状态打进日志（每 3 秒一条，频率很低）。
  // 「画面卡住」的三种可能在这里可以直接区分：
  //   ended=1        → 切段判定没触发
  //   running=0      → 解码线程被停了（遮挡计数问题）
  //   stamp 不涨     → 解码线程还在但拿不到帧（文件/解码卡住）
  if (got_frame) {
    pet.lastFrameTick = now;
  } else if (pet.clip != nullptr && pet.clip->frameStamp > 0 &&
             now - pet.lastFrameTick > 3000) {
    pet.lastFrameTick = now;
    core::log::Log(
        "pet{} NOFRAME 3s: clip={} ended={} visRefs={} running={} occluded={} "
        "played={} stamp={}",
        pet.index, pet.currentClip, pet.clip->ended ? 1 : 0,
        pet.clip->visibleRefs, pet.clip->running ? 1 : 0, pet.occluded ? 1 : 0,
        pet.playedFrames, pet.clip->frameStamp);
  }

  // 帧号快照必须先于移动与切段判定：二者都以「本 Tick 呈现的帧」为准。
  const int frames = pet.playedFrames;

  // 拖动时立刻停走。
  if (pet.dragging) pet.moveActive = false;
  const bool needFly = pet.flying;
  const bool needMove = pet.moveActive && pet.moveSpeedPxPerFrame > 0;
  // 飞行与行走同帧发生时，监视器和碰撞区域只收集一次。
  if (needFly || needMove) {
    const core::MonitorInfo mon = MonitorFor(pet.hwnd);
    // 收集一次、排序一次，飞行与行走共用；复用 Pet 里的 scratch 缓冲。
    CollectOthers(pet, &pet.othersScratch);
    if (needFly) StepFly(pet, mon);
    if (needMove) StepMove(pet, frames, mon, pet.othersScratch);
  }

#if PET_DEBUG_MASK_BMP
  // 逐帧掩码导出是编译期开关（默认关闭），避免正式包里藏一个持续写磁盘的分支。
  if (pet.clip != nullptr && pet.frameCount % 30 == 0) {
    const WinClipTexture& mask_tex = winclips::Tex(*pet.clip);
    winutil::SaveMaskBmp(mask_tex.maskW, mask_tex.maskH, pet.clip->mask.mask,
                         pet.playedFrames * 1000 + pet.index);
  }
#endif
  ++pet.frameCount;

  CheckClipEnd(pet);

  // Q 弹曲线是 60Hz 的时间函数，必须每帧推进；但只有画面真的会变时才 Present：
  // ① 本 Tick 换了新视频帧；② Q 弹动画进行中（含收尾那一帧，此时 squash
  // 取自推进前的状态）；③ 显式置位的 needPresent（缩放/换实例/首帧）。
  const bool squash = pet.squashActive;
  UpdateSquash(pet);
  if (got_frame || squash || pet.needPresent) Present(pet);
}

void TickAll() {
  // 先让所有共享实例换帧/上传/重算掩码（每实例一次），再逐只消费同一批结果：
  // 同 clip 的宠物天然同步，解码器与纹理上传次数只与 clip 数有关。
  winclips::UpdateAll();
  for (const auto& pet : winpet::g_pets) {
    if (pet->hwnd == nullptr || pet->closing) continue;
    Tick(*pet);
  }
}

}  // namespace winpetops
