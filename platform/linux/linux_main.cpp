// 原生桌宠 Linux 后端：X11 (ARGB visual) + OpenGL (预乘 alpha) + XShape 输入穿透。
// 复用 core/ 的配置、解码、掩码与物理逻辑。
#define GL_GLEXT_PROTOTYPES
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>

#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>

#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>  // setenv
#include <cmath>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/config.h"
#include "core/decode.h"
#include "core/log.h"
#include "core/mask.h"
#include "core/paths.h"
#include "core/physics.h"
#include "core/util.h"
#include "platform/linux/linux_pet_ops.h"
#include "platform/linux/linux_render.h"
#include "platform/linux/linux_state.h"
#include "platform/linux/linux_ui.h"
#include "platform/linux/linux_util.h"

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif

using namespace core;        // NOLINT
using namespace core::log;   // Log(...) 与 Win 端保持同形
using namespace linuxpetops;
using namespace linuxrender;
using namespace linuxui;
using namespace linuxutil;

namespace {




// 在 context 仍 current 时释放该宠物的 GL 资源，并维护共享对象组：
// g_prog 属于 g_shareCtx 所在的共享组，只有组内最后一个 context 销毁后它才失效。
// 宿主宠物关闭时若组内还有其他存活 context，就把 g_shareCtx 迁移过去，
// 保证之后新建的宠物仍能与存活宠物共享同一个 program（否则新 context 不共享，
// 却因 g_prog != 0 而不重建，glUseProgram 会拿到无效 ID）。
void ReleasePetGL(Pet& p) {
  if (p.ctx == nullptr) return;
  glXMakeCurrent(g_dpy, p.win, p.ctx);
  if (p.tex != 0) {
    glDeleteTextures(1, &p.tex);
    p.tex = 0;
  }

  // 必须在销毁 p.ctx 之前定好新宿主与 program 的去留。
  GLXContext newShare = nullptr;
  if (p.ctx == g_shareCtx) {
    for (const auto& other : g_pets) {
      Pet* o = other.get();
      if (o != &p && o->ctx != nullptr) {
        newShare = o->ctx;
        break;
      }
    }
    if (newShare == nullptr && g_prog != 0) {
      // 组内最后一个 context：趁 p.ctx 仍 current 删除共享 program。
      glDeleteProgram(g_prog);
      g_prog = 0;
    }
  }

  glXMakeCurrent(g_dpy, None, nullptr);
  if (p.ctx == g_shareCtx) g_shareCtx = newShare;
  glXDestroyContext(g_dpy, p.ctx);
  p.ctx = nullptr;
}

void ReleaseAllPets() {
  for (size_t i = 0; i < g_pets.size(); ++i) {
    Pet& p = *g_pets[i];
    Stop(p.dec);
    Close(p.dec);
    ReleasePetGL(p);
    if (p.win != 0) {
      XDestroyWindow(g_dpy, p.win);
      p.win = 0;
    }
  }
}

void CloseDisplayResources() {
  if (g_cmap != 0) {
    XFreeColormap(g_dpy, g_cmap);
    g_cmap = 0;
  }
  if (g_visual != nullptr) {
    // 有合成器时 g_visual 指向 InitVisual 里的 static XVisualInfo，
    // 释放它会触发 free(): invalid size（表现为退出即 SIGABRT）。
    if (g_visualOwned) XFree(g_visual);
    g_visual = nullptr;
    g_visualOwned = false;
  }
  if (g_dpy != nullptr) {
    XCloseDisplay(g_dpy);
    g_dpy = nullptr;
  }
}

// 拖拽期间唯一的所有者：grab 之后所有鼠标事件都归它，不能按 ev.window
// 找宠物——指针移到另一只宠物窗口上时事件可能投递给那只窗口，导致
// dragging 与 XUngrabPointer 永远不被清理。
Pet* g_dragPet = nullptr;

void HandleButtonPress(XButtonEvent& ev) {
  Log("press button={} win=0x{:x}", ev.button, ev.window);
  if (ev.button == Button3) {
    Pet* pet = PetForWindow(ev.window);
    if (pet != nullptr && g_gtkReady) ShowMenu(*pet, ev.x_root, ev.y_root);
    return;
  }
  if (ev.button != Button1) return;
  Pet* pet = PetForWindow(ev.window);
  if (pet == nullptr) return;

  // 睡觉中点击宠物 = 「起来」：不播点击回应，也不进入拖动。
  if (pet->sleepPhase == SleepPhase::Start ||
      pet->sleepPhase == SleepPhase::Loop) {
    ToggleSleep(*pet);
    return;
  }

  pet->dragOffX = ev.x_root - pet->posX;
  pet->dragOffY = ev.y_root - pet->posY;
  pet->dragLastX = ev.x_root;
  pet->dragLastY = ev.y_root;
  pet->dragStartX = ev.x_root;
  pet->dragStartY = ev.y_root;
  pet->dragAnimStarted = false;
  pet->dragLastTick = NowMs();
  pet->dragging = true;
  g_dragAny = true;
  g_dragPet = pet;
  pet->flying = false;
  pet->vx = pet->vy = 0.0;
  // owner_events=False：所有事件都以 grab window 报告，不会按常规投递到
  // 指针下面的另一只宠物窗口；否则松手事件丢失，grab 与 dragging 都残留。
  const int grab = XGrabPointer(g_dpy, pet->win, False,
                                ButtonReleaseMask | PointerMotionMask,
                                GrabModeAsync, GrabModeAsync, None, None,
                                CurrentTime);
  if (grab != GrabSuccess) {
    Log("pet{} grab pointer failed: {}", pet->index, grab);
    pet->dragging = false;
    g_dragAny = false;
    g_dragPet = nullptr;
    return;
  }
  // 按下即播点击回应（加权）+ Q 弹，与 Win 端一致。
  ActionSpec clickSpec;
  clickSpec.kind = ActionKind::Click;
  clickSpec.name = PickClickAction();
  if (clickSpec.name.empty()) clickSpec.name = core::PickRandom(g_clickClips);
  if (!clickSpec.name.empty()) PlayAction(*pet, clickSpec);
  pet->squashStart = NowMs();
  pet->squashActive = true;
  Log("pet{} press at {},{}", pet->index, ev.x_root, ev.y_root);
}

void HandleMotion(XMotionEvent& ev) {
  if (!g_dragAny) return;
  Pet* pet = g_dragPet;
  if (pet == nullptr || !pet->dragging) return;
  MovePetTo(*pet, ev.x_root - pet->dragOffX, ev.y_root - pet->dragOffY);
  // 拖动超过阈值才切到「被鼠标拖拽悬空反馈」并循环播放。
  if (!pet->dragAnimStarted &&
      std::abs(ev.x_root - pet->dragStartX) +
              std::abs(ev.y_root - pet->dragStartY) > 6) {
    pet->dragAnimStarted = true;
    const ActionSpec dragSpec = MakeDragSpec();
    if (!dragSpec.name.empty()) PlayAction(*pet, dragSpec);
  }
  // 拖动过程中低通平滑速度：只用松手那一帧采样经常取到 0（甩不出去）。
  const int64_t now = NowMs();
  const int64_t dt = now - pet->dragLastTick;
  if (dt > 0 && dt < 200) {
    const double nvx = (ev.x_root - pet->dragLastX) * 1000.0 / dt;
    const double nvy = (ev.y_root - pet->dragLastY) * 1000.0 / dt;
    pet->vx = pet->vx * 0.7 + nvx * 0.3;
    pet->vy = pet->vy * 0.7 + nvy * 0.3;
  }
  pet->dragLastX = ev.x_root;
  pet->dragLastY = ev.y_root;
  pet->dragLastTick = now;
}

void HandleButtonRelease(XButtonEvent& ev) {
  if (ev.button != Button1) return;
  Pet* pet = g_dragPet;
  if (pet == nullptr || !pet->dragging) return;
  pet->dragging = false;
  g_dragAny = false;
  g_dragPet = nullptr;
  XUngrabPointer(g_dpy, CurrentTime);

  const int64_t now = NowMs();
  // 松手前停住超过 120ms 视为没甩（用拖动时平滑出来的速度）。
  if (now - pet->dragLastTick > 120) {
    pet->vx = 0.0;
    pet->vy = 0.0;
  }
  if (std::fabs(pet->vx) > 80.0 || std::fabs(pet->vy) > 80.0) {
    pet->flying = true;
    pet->flyTick = now;
    Log("pet{} throw v={:.0f},{:.0f}", pet->index, pet->vx, pet->vy);
  }

  // 松手后从拖拽反馈回到加权随机链。
  if (pet->dragAnimStarted) {
    pet->dragAnimStarted = false;
    const ActionSpec next = PickNextAction(pet->facing);
    if (!next.name.empty()) PlayAction(*pet, next);
  }
}



}  // namespace

int main(int argc, char** argv) {
  int pets = 1;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--debug") g_debug = true;
    if (arg == "--menu-test") g_menuTest = true;
    if (arg == "--sleep-test") g_sleepTest = true;
    if (arg == "--exit-test") g_exitTest = true;
    if (arg.starts_with("--clip=")) g_startClip = std::string(arg.substr(7));
    if (arg.starts_with("--pets=")) {
      const std::string_view num = arg.substr(7);
      int value = 0;
      const auto [end, ec] =
          std::from_chars(num.data(), num.data() + num.size(), value);
      if (ec == std::errc{} && end == num.data() + num.size() && value > 0) {
        // 与 Win 端一致的上限：网格按 64 只排布，再多就只能互相重叠。
        if (value > 64) value = 64;
        pets = value;
      }
    }
  }
  // 日志初始化：公共模块只负责格式化，落盘由 linuxutil::LogSink 完成。
  core::log::Init(g_debug, &LogSink);

  // 资源路径按 exe 目录解析（exe 同目录或向上若干级的 assets/），不依赖 CWD。
  const std::string exeDir = ExeDir();
  g_clipsRoot = core::ResolveUpwards(exeDir, "assets/clips/");
  const std::string configPath =
      core::ResolveUpwards(exeDir, "assets/pet-config.json");
  LoadConfig(configPath);
  g_scalePercent = static_cast<double>(core::g_initialSizePercent);
  if (g_byName.empty()) {
    std::cerr << "config load failed: " << configPath << '\n';
    return 1;
  }
  // 正常模式不往终端打字，配置摘要只在 --debug 时记录。
  Log("config: byName={} idle={} clicks={} moves={} random={}",
      static_cast<int>(g_byName.size()), static_cast<int>(g_idleClips.size()),
      static_cast<int>(g_clickClips.size()), static_cast<int>(g_moves.size()),
      static_cast<int>(g_randomClips.size()));

  g_dpy = XOpenDisplay(nullptr);
  if (g_dpy == nullptr) {
    std::cerr << "XOpenDisplay failed\n";
    return 2;
  }
  // GTK 只用于右键菜单；初始化失败不影响宠物本体。
  // 宠物窗口是 X11，菜单也必须走 X11 后端：在 Wayland 会话里 GTK 默认选
  // wayland，popup 没有父 surface 会被 KWin 当普通窗口加标题栏并居中摆放，
  // 既无法定位到鼠标、也拿不到 XID。
  setenv("GDK_BACKEND", "x11", 1);
  {
    int gtkArgc = 0;
    char** gtkArgv = nullptr;
    g_gtkReady = gtk_init_check(&gtkArgc, &gtkArgv);
    if (!g_gtkReady) Log("gtk init failed, right-click menu disabled");
  }
  // 我们的窗口是原生 Xlib 创建的，GTK 不知道它的窗口树，所以 popup 菜单
  // 会报 "no parent"/"no trigger event" 之类的 warning（功能不受影响）。
  // 接管 GLib 的日志输出，把这些提示收进调试日志，不污染 stderr。
  if (g_gtkReady) {
    g_log_set_writer_func(
        [](GLogLevelFlags level, const GLogField* fields, gsize n_fields,
           gpointer) -> GLogWriterOutput {
          for (gsize i = 0; i < n_fields; ++i) {
            if (fields[i].key != nullptr &&
                std::string_view(fields[i].key) == "MESSAGE" &&
                fields[i].value != nullptr) {
              Log("glib: {}", static_cast<const char*>(fields[i].value));
            }
          }
          // ERROR 默认会 abort；吞掉它会把致命错误降级成一行日志。
          return (level & G_LOG_LEVEL_ERROR) != 0
                     ? G_LOG_WRITER_UNHANDLED
                     : G_LOG_WRITER_HANDLED;
        },
        nullptr, nullptr);
  }

  // 菜单关闭由主循环调用 UpdateMenu 轮询（50ms 节流），不依赖 focus-out-event。
  g_screen = DefaultScreen(g_dpy);
  g_root = RootWindow(g_dpy, g_screen);
  Log("display {}x{}", DisplayWidth(g_dpy, g_screen),
      DisplayHeight(g_dpy, g_screen));

  if (!InitVisual()) {
    std::cerr << "gl visual init failed\n";
    CloseDisplayResources();
    return 3;
  }

  for (int i = 0; i < pets; ++i) CreatePet(g_nextPetId++);
  if (g_pets.empty()) {
    CloseDisplayResources();
    return 4;
  }
  if (g_prog == 0) {
    std::cerr << "shader build failed\n";
    ReleaseAllPets();
    CloseDisplayResources();
    return 5;
  }

  g_startTick = NowMs();
  int64_t nextTick = g_startTick;
  while (g_running) {
    // --exit-test：4 秒后走正常退出流程，用于验证清理路径不再崩溃。
    if (g_exitTest && NowMs() - g_startTick > 4000) g_running = false;
    if (!g_running) break;
    while (XPending(g_dpy) > 0) {
      XEvent ev{};
      XNextEvent(g_dpy, &ev);
      switch (ev.type) {
        case ButtonPress:
          HandleButtonPress(ev.xbutton);
          break;
        case MotionNotify:
          HandleMotion(ev.xmotion);
          break;
        case ButtonRelease:
          HandleButtonRelease(ev.xbutton);
          break;
        case Expose:
          break;
        case DestroyNotify: {
          // WM 或会话退出会销毁窗口；不标记 closing 的话后续 XMoveWindow /
          // glXSwapBuffers 会作用在失效 Window 上，Xlib 默认错误处理器直接退出。
          Pet* p = PetForWindow(ev.xdestroywindow.window);
          if (p != nullptr) p->closing = true;
          break;
        }
        default:
          break;
      }
    }
    // GTK 菜单需要自己的迭代器派发事件（非阻塞）。
    if (g_gtkReady) {
      while (gtk_events_pending()) gtk_main_iteration_do(false);
    }

    // 回收已关闭的宠物（关闭时只置标记，避免在 GTK 回调里 join 线程）。
    for (size_t i = 0; i < g_pets.size();) {
      Pet* p = g_pets[i].get();
      if (!p->closing) {
        ++i;
        continue;
      }
      Stop(p->dec);
      Close(p->dec);
      ReleasePetGL(*p);
      if (p->win) XDestroyWindow(g_dpy, p->win);
      Log("pet{} closed", p->index);
      // erase 后 unique_ptr 析构会自动 Stop/Close，无需手动 delete。
      g_pets.erase(g_pets.begin() + static_cast<long>(i));
    }
    if (g_pets.empty()) break;

    // --menu-test：2 秒后自动弹一次菜单，用于无鼠标环境验证外观。
    static bool menuShown = false;
    if (g_menuTest && !menuShown) {
      const int64_t t = NowMs() - g_startTick;
      if (t > 2000) {
        menuShown = true;
        if (!g_pets.empty() && g_gtkReady) ShowMenu(*g_pets[0], 120, 140);
      }
    }

    // --sleep-test：2 秒后睡觉、14 秒后起来，用于无鼠标环境验证状态机。
    static bool sleepStarted = false;
    static bool sleepWoke = false;
    if (g_sleepTest && !g_pets.empty()) {
      const int64_t t = NowMs() - g_startTick;
      if (!sleepStarted && t > 2000) {
        sleepStarted = true;
        ToggleSleep(*g_pets[0]);
      } else if (sleepStarted && !sleepWoke && t > 14000) {
        sleepWoke = true;
        ToggleSleep(*g_pets[0]);
      }
    }

    const int64_t now = NowMs();
    // 菜单失焦 / 菜单外点击检测（内部 50ms 节流，避免每帧两次 X round-trip）。
    UpdateMenu(now);
    if (now >= nextTick) {
      for (const auto& p : g_pets) Tick(*p);
      // 按累计时间推进：负载高时 nextTick 落后较多也不把物理放慢，
      // 但落后超过一帧就丢弃积压，避免恢复时突然连跑几十个 Tick。
      nextTick += 16;  // 16ms 物理步长
      if (nextTick <= now) nextTick = now + 16;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  // 先销毁菜单：MenuAction 借用 Pet*，必须早于 g_pets 释放。
  linuxui::CloseMenu();
  // 先停线程、释放 GL 资源，再销毁 context 与窗口；
  // 之后 g_pets 析构时 DecodeState 会幂等地再 Stop/Close 一次。
  ReleaseAllPets();
  CloseDisplayResources();
  return 0;
}
