#include "platform/linux/linux_pet_ops.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <thread>
#include <vector>

#include "core/decode.h"
#include "core/log.h"
#include "core/mask.h"
#include "core/physics.h"
#include "core/util.h"
#include "platform/linux/linux_render.h"
#include "platform/linux/linux_util.h"

using namespace core;       // NOLINT
using namespace core::log;  // Log(...) 与 Win 端保持同形
using namespace linuxrender;
using namespace linuxutil;

namespace linuxpetops {

// ===== 窗口几何 =====

// KDE/GNOME 的 _NET_WORKAREA：去掉面板后的可用区域。
// KWin 会把受管理窗口完整约束在这个区域里，窗口不能像 Win32 那样部分移出屏幕。
Rect WorkAreaRect() {
  static Rect work{0, 0, 0, 0};
  static int64_t lastCheck = 0;
  // 面板/分辨率/WM 重启都会改变工作区；首次读取失败时也不能永久缓存错误值。
  const int64_t now = NowMs();
  if (lastCheck != 0 && now - lastCheck < 5000) return work;
  lastCheck = now;
  const Atom prop = XInternAtom(g_dpy, "_NET_WORKAREA", False);
  Atom type = None;
  int format = 0;
  unsigned long n = 0;
  unsigned long after = 0;
  unsigned char* raw = nullptr;
  const Status st = XGetWindowProperty(g_dpy, g_root, prop, 0, 4, False,
                                       XA_CARDINAL, &type, &format, &n, &after,
                                       &raw);
  XPropertyPtr data(raw);
  if (st == Success && data != nullptr && format == 32 && n >= 4) {
    const long* v = reinterpret_cast<const long*>(data.get());
    work = Rect{static_cast<int>(v[0]), static_cast<int>(v[1]),
                static_cast<int>(v[0] + v[2]),
                static_cast<int>(v[1] + v[3])};
  } else {
    work = Rect{0, 0, DisplayWidth(g_dpy, g_screen),
                DisplayHeight(g_dpy, g_screen)};
  }
  Log("work area: ({},{})-({},{})", work.l, work.t, work.r, work.b);
  return work;
}

MonitorInfo MonitorFor() {
  const Rect work = WorkAreaRect();
  MonitorInfo mon;
  mon.left = work.l;
  mon.top = work.t;
  mon.right = work.r;
  mon.bottom = DisplayHeight(g_dpy, g_screen);
  mon.workBottom = work.b;  // 底部贴工作区下沿（面板上方）
  return mon;
}

// 读取窗口在 root 上的真实左上角（窗口可能已被 WM reparent 到 frame）。
bool WindowAbsPos(Window win, int* x, int* y) {
  int rx = 0, ry = 0;
  Window child = None;
  if (!XTranslateCoordinates(g_dpy, win, g_root, 0, 0, &rx, &ry, &child)) {
    return false;
  }
  *x = rx;
  *y = ry;
  return true;
}

void ApplyInputShape(Pet& pet);

// 把窗口写到 root 绝对坐标，并同步逻辑位置。
// KWin 会把 ConfigureRequest 的坐标解释为 frame 的绝对位置；无 WM 时父窗口
// 就是 root，两者语义一致。
// 有 WM 时窗口被完整约束在工作区内（不能部分移出屏幕），这里把「期望位置」
// 与「WM 允许的位置」之差转成窗口内的内容平移，让身体仍能贴到屏幕边缘。
void MovePetTo(Pet& pet, int x, int y) {
  int winX = x;
  int winY = y;
  if (g_hasWm) {
    const Rect work = WorkAreaRect();
    // std::clamp 要求 lo <= hi：工作区比窗口窄/矮时先修正上界。
    winX = std::clamp(x, work.l, std::max(work.l, work.r - pet.windowW));
    winY = std::clamp(y, work.t, std::max(work.t, work.b - pet.windowH));
    // 补偿量不能超过身体框外的透明边距（正好等于物理边界），
    // 否则把宠物拖出屏幕时身体会被推出窗口、整只消失。
    // 缩放后 body 可能还是旧尺寸，使 lo > hi（std::clamp 前置条件被破坏）。
    // 用 max 保证区间合法，最坏情况只是暂时没有透明边距补偿。
    const int hiX = std::max(winX - pet.bodyL, winX + pet.windowW - pet.bodyR);
    const int hiY = std::max(winY - pet.bodyT, winY + pet.windowH - pet.bodyB);
    x = std::clamp(x, winX - pet.bodyL, hiX);
    y = std::clamp(y, winY - pet.bodyT, hiY);
  }
  pet.posX = x;
  pet.posY = y;
  pet.posDirty = true;
  const int offX = x - winX;
  const int offY = y - winY;
  const bool offChanged = (offX != pet.drawOffX || offY != pet.drawOffY);
  pet.drawOffX = offX;
  pet.drawOffY = offY;
  XMoveWindow(g_dpy, pet.win, winX, winY);
  if (offChanged) ApplyInputShape(pet);
}

std::vector<Rect> CollectOthers(Pet& self) {
  std::vector<Rect> out;
  for (const auto& owner : g_pets) {
    Pet* p = owner.get();
    if (p == &self || p->win == 0) continue;
    out.push_back(Rect{p->posX + p->bodyL, p->posY + p->bodyT,
                       p->posX + p->bodyR, p->posY + p->bodyB});
  }
  return out;
}

// 按掩码矩形设置 XShape 输入区域（区域外鼠标穿透）。
void ApplyInputShape(Pet& pet) {
  if (pet.win == 0) return;
  if (pet.maskResult.rects.empty()) return;
  std::vector<XRectangle> rects;
  rects.reserve(pet.maskResult.rects.size());
  for (const Rect& r : pet.maskResult.rects) {
    // 输入形状跟着内容一起平移，并按窗口边界裁剪。
    const int l = std::max(0, r.l + pet.drawOffX);
    const int t = std::max(0, r.t + pet.drawOffY);
    const int rr = std::min(pet.windowW, r.r + pet.drawOffX);
    const int bb = std::min(pet.windowH, r.b + pet.drawOffY);
    if (rr <= l || bb <= t) continue;
    XRectangle xr{};
    xr.x = static_cast<short>(l);
    xr.y = static_cast<short>(t);
    xr.width = static_cast<unsigned short>(rr - l);
    xr.height = static_cast<unsigned short>(bb - t);
    rects.push_back(xr);
  }
  if (rects.empty()) {
    // 全透明帧必须清空输入/可见形状，否则沿用上一帧区域，
    // 出现「看不见却点得到」。None 是指 0 号 Pixmap，表示空形状。
    XShapeCombineMask(g_dpy, pet.win, ShapeInput, 0, 0, None, ShapeSet,
                      Unsorted);
    if (!g_hasCompositor) {
      XShapeCombineMask(g_dpy, pet.win, ShapeBounding, 0, 0, None, ShapeSet,
                        Unsorted);
    }
    XFlush(g_dpy);
    return;
  }
  XShapeCombineRectangles(g_dpy, pet.win, ShapeInput, 0, 0, rects.data(),
                          static_cast<int>(rects.size()), ShapeSet, Unsorted);
  // 没有合成器时 ARGB 不被混合，必须用 Bounding 形状裁出可见区域。
  if (!g_hasCompositor) {
    XShapeCombineRectangles(g_dpy, pet.win, ShapeBounding, 0, 0, rects.data(),
                            static_cast<int>(rects.size()), ShapeSet,
                            Unsorted);
  }
  XFlush(g_dpy);
}

// 整体缩放：只改窗口尺寸，GL 全屏 quad 与纹理尺寸无关。
void ApplySizeAll(int delta) {
  int percent = static_cast<int>(g_scalePercent) + delta;
  if (percent < 10) percent = 10;
  if (percent > 40) percent = 40;
  g_scalePercent = percent;
  const int screenW = DisplayWidth(g_dpy, g_screen);
  int targetW = screenW * percent / 100;
  if (targetW < 220) targetW = 220;
  if (targetW > 640) targetW = 640;
  const int targetH = targetW * 9 / 16;
  for (const auto& p : g_pets) {
    p->windowW = targetW;
    p->windowH = targetH;
    // body 是上一帧按旧窗口尺寸算出的窗口内偏移，立即重置为整窗：
    // 否则 MovePetTo 的补偿 clamp 会出现 lo > hi，把宠物推到屏幕外。
    p->bodyL = 0;
    p->bodyT = 0;
    p->bodyR = targetW;
    p->bodyB = targetH;
    p->texW = p->texH = 0;  // 尺寸变了，纹理必须重建
    XResizeWindow(g_dpy, p->win, targetW, targetH);
    // 尺寸变了，工作区约束和补偿量要重算。
    MovePetTo(*p, p->posX, p->posY);
    p->regionMask.clear();
    p->forceRegionFrames = 4;  // 固定几帧即可，整段帧数会让节流永远失效
    Log("pet{} resize {}% {}x{}", p->index, percent, targetW, targetH);
  }
  XFlush(g_dpy);
}

// ===== 每帧管线 =====

namespace {

// 掩码 / 可见区域 / 区域矩形：平台无关计算放在 core；
// 切段/缩放后强制几帧，之后按 2% 阈值节流，形状完全相同时跳过 XShape。
void UpdateMaskAndShape(Pet& pet) {
  if (pet.dec.videoW <= 0 || pet.dec.videoH <= 0 || pet.dec.bgra.empty()) return;
  BuildMask(pet.dec.bgra.data(), pet.dec.videoW, pet.dec.videoH, pet.windowW,
            pet.windowH, pet.windowW >= 480 ? 512 : 384,
            pet.windowW >= 480 ? 288 : 216, pet.flip, 16, 2,
            &pet.maskResult);
  pet.bodyL = pet.maskResult.bodyL;
  pet.bodyT = pet.maskResult.bodyT;
  pet.bodyR = pet.maskResult.bodyR;
  pet.bodyB = pet.maskResult.bodyB;

  const bool force = pet.forceRegionFrames > 0;
  if (pet.forceRegionFrames > 0) --pet.forceRegionFrames;
  const size_t count = pet.maskResult.mask.size();
  bool apply = force || pet.regionMask.size() != count;
  if (!apply) {
    // 一次遍历同时判断「完全相同」与「变化 >=2%」，避免再全量比较一遍。
    size_t diff = 0;
    for (size_t i = 0; i < count; ++i) {
      if (pet.maskResult.mask[i] != pet.regionMask[i]) ++diff;
    }
    apply = diff * 50 >= count;
  }
  if (apply) {
    pet.regionMask = pet.maskResult.mask;
    ApplyInputShape(pet);
  }
}

}  // namespace

void UploadFrame(Pet& pet) {
  UploadVideoFrame(pet);
  UpdateMaskAndShape(pet);
}

// ===== 动作状态机 =====

namespace {

core::ActionState StateOf(const Pet& pet) {
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

void ApplyStateToPet(Pet& pet, const core::ActionState& st) {
  pet.spec = st.spec;
  pet.sleepPhase = st.sleepPhase;
  pet.facing = st.facing;
  pet.walkFlipped = st.walkFlipped;
  pet.dragMode = st.dragMode;
  pet.nextClipTick = st.nextClipTick;
}

}  // namespace

bool PlayClip(Pet& pet, const std::string& name) {
  const auto it = g_byName.find(name);
  if (it == g_byName.end()) return false;
  Stop(pet.dec);
  if (!Open(pet.dec, g_clipsRoot + it->second)) {
    Log("pet{} open clip failed: {}", pet.index, name.c_str());
    return false;
  }
  glXMakeCurrent(g_dpy, pet.win, pet.ctx);
  InitTexture(pet);
  // loop 类动作（drag 反馈、睡觉循环段）在 EOF 后 seek 循环；
  // 其余播完即停在最后一帧，由状态机按 eof 切段。
  pet.dec.loop = pet.spec.loop;
  Start(pet.dec);
  bool firstFrame = false;
  for (int i = 0; i < 40; ++i) {
    if (TakeFrame(pet.dec)) {
      firstFrame = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  pet.currentClip = name;
  pet.frameCount = 0;
  // 解码线程按平均 fps 调度，Tick 是固定 16ms；两者不同步，
  // 所以用「实际呈现的视频帧数」当状态机时钟（与 Win32 端一致）。
  const double fps = pet.dec.fps > 0 ? pet.dec.fps : 30.0;
  pet.clipFrames = std::max(1, pet.dec.clipFrames - 1);
  pet.playDurationMs = static_cast<int>(pet.clipFrames * 1000.0 / fps);
  pet.playStartTick = NowMs();

  // 朝向和移动参数要在首帧上传前定好。
  const auto mv = g_moves.find(name);
  // 非移动动作按全局持久 facing 显示（Turn 播完会真正转身），
  // 移动动作的视觉方向必须取自自身 flip 配置，不能继承上次 Turn 残留的 facing，
  // 否则「原地左转奔跑」会被镜像成向右跑，与 dirX 的平移方向相反。
  pet.flip = (mv != g_moves.end()) ? (mv->second.flip != 0)
                                   : (pet.facing == -1);
  pet.moveActive = false;
  pet.moveSpeedPxPerFrame = 0.0;
  if (mv != g_moves.end()) {
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
    pet.moveTotal = core::RandomInt(lo, hi);
    pet.moveDone = 0;
    // lead/tail/速度由 core 统一计算，两端系数不再分歧。
    const core::MoveTiming timing =
        core::ComputeMoveTiming(mv->second, fps, pet.clipFrames, pet.moveTotal);
    if (timing.moveFrames > 0 && pet.moveTotal > 0) {
      pet.moveActive = true;
      pet.moveLeadFrames = timing.leadFrames;
      pet.moveLastFrames = 0;
      pet.moveSpeedPxPerFrame = timing.speedPxPerFrame;
      Log("pet{} walk start dist={} dir={},{} lead={} move={} clip={} frames",
          pet.index, pet.moveTotal, pet.moveDirX, pet.moveDirY,
          timing.leadFrames, timing.moveFrames, pet.clipFrames);
    }
  }
  // 换动作后必须重算区域，否则旧掩码会裁掉新动作。
  pet.regionMask.clear();
  pet.forceRegionFrames = pet.dec.clipFrames;
  UploadFrame(pet);
  // 首帧也算一帧，之后由 Tick 的 TakeFrame 累加。
  pet.playedFrames = firstFrame ? 1 : 0;
  Log("pet{} play clip: {} tick={} dur={}ms frames={}", pet.index,
      name.c_str(), static_cast<long long>(pet.playStartTick),
      pet.playDurationMs, pet.clipFrames);
  return true;
}

// 切段前恢复 flip 镜像，按 kind 设置朝向。
bool PlayAction(Pet& pet, const ActionSpec& spec) {
  if (spec.name.empty()) return false;
  // 派生状态的提交与失败回滚统一交给 core::ActionMachine。
  core::ActionState st = StateOf(pet);
  const core::ActionState snapshot = core::SnapshotAction(st);
  core::ApplyActionSpec(st, spec);
  ApplyStateToPet(pet, st);
  if (!PlayClip(pet, spec.name)) {
    core::RestoreAction(st, snapshot);
    ApplyStateToPet(pet, st);
    pet.nextClipTick = NowMs() + 1000;
    return false;
  }
  return true;
}

// 片段结束：loop 每圈重置时钟；其余由 core 统一决策切段。
void OnClipEnded(Pet& pet, int64_t nowMs, bool stalled) {
  if (pet.spec.loop && !stalled) {
    pet.playedFrames = 0;
    pet.moveLastFrames = 0;
    pet.moveDone = 0;
    pet.playStartTick = nowMs;
    return;
  }
  core::ActionState st = StateOf(pet);
  const ActionSpec next = core::NextActionAfterEnd(st, nowMs, stalled);
  ApplyStateToPet(pet, st);
  if (next.name.empty()) return;
  if (!PlayAction(pet, next)) pet.playStartTick = nowMs;
}

void ToggleSleep(Pet& pet) {
  const bool sleeping =
      pet.sleepPhase == SleepPhase::Start || pet.sleepPhase == SleepPhase::Loop;
  ActionSpec spec;
  spec.kind = ActionKind::Sleep;
  if (sleeping) {
    spec.name = PickFromPool(g_sleepClips.end);
    pet.sleepPhase = SleepPhase::End;
  } else {
    spec.name = PickFromPool(g_sleepClips.start);
    pet.sleepPhase = SleepPhase::Start;
  }
  if (spec.name.empty() || !PlayAction(pet, spec)) {
    pet.sleepPhase = SleepPhase::Awake;
    Log("pet{} sleep toggle failed (clips={})", pet.index,
        static_cast<int>(g_sleepClips.start.size()));
    return;
  }
  Log("pet{} sleep {}", pet.index, sleeping ? "wake" : "sleep");
}

// ===== 每帧 Tick =====

void Tick(Pet& pet) {
  const int64_t tickNow = NowMs();
  // 首段打开失败时没有 eof、没有视频时钟，stalled 兜底永远不会触发。
  // 用独立的退避计时每秒重试一次，避免宠物永久定格。
  if (pet.currentClip.empty() && tickNow >= pet.nextClipTick) {
    pet.nextClipTick = tickNow + 1000;
    const ActionSpec next = PickNextAction(pet.facing);
    if (!next.name.empty()) PlayAction(pet, next);
  }
  // WM 可能在 map 之后才完成摆放；仅启动后前 1.5 秒校准，
  // 不能每次切段（frameCount 归零）都重新读回位置，否则切动作时会跳位。
  if (!pet.posDirty && pet.frameCount > 0 &&
      tickNow - g_startTick < 1500 && pet.frameCount % 15 == 0) {
    WindowAbsPos(pet.win, &pet.posX, &pet.posY);
  }
  if (TakeFrame(pet.dec)) {
    UploadVideoFrame(pet);
    ++pet.playedFrames;  // 只在真正呈现一帧时推进视频时钟
    UpdateMaskAndShape(pet);
  }

  // Q 弹。
  if (pet.squashActive) {
    const float u = static_cast<float>(NowMs() - pet.squashStart) / 220.0f;
    if (u >= 1.0f) {
      pet.squashActive = false;
      pet.scaleY = 1.0f;
    } else {
      pet.scaleY = core::SquashCurve(u);
    }
  }

  // 拖动时立刻停走。
  if (pet.dragging) pet.moveActive = false;
  // 飞行与行走可能同帧发生，监视器与碰撞区域只收集一次。
  const bool needFly = pet.flying;
  const bool needMove = pet.moveActive && pet.moveSpeedPxPerFrame > 0;
  const MonitorInfo mon =
      (needFly || needMove) ? MonitorFor() : MonitorInfo{};
  const std::vector<Rect> others =
      (needFly || needMove) ? CollectOthers(pet) : std::vector<Rect>{};

  // 飞行。
  if (needFly) {
    const int64_t now = NowMs();
    const double dt = 0.016;  // 固定步长
    int x = pet.posX;
    int y = pet.posY;
    bool bounced = false;
    bool stopped = false;
    // 窗口范围。
    core::WindowBounds bounds;
    bounds.minX = mon.left - pet.bodyL;
    bounds.maxX = mon.right - pet.bodyR;
    bounds.minY = mon.top - pet.bodyT;
    bounds.maxY = mon.workBottom - pet.bodyB;
    StepFly(&pet.vx, &pet.vy, &x, &y, bounds, dt, &bounced, &stopped);
    MovePetTo(pet, x, y);

    // 老版飞行撞宠物：位置提交后按当前身体框相交判定；800ms 冷却 +
    // 速度阈值；自己减速，对方获得固定冲量并进入 flying。
    if (now - pet.lastCollisionTick > 800 &&
        (std::fabs(pet.vx) + std::fabs(pet.vy)) > 150.0) {
      const Rect self{x + pet.bodyL, y + pet.bodyT, x + pet.bodyR,
                      y + pet.bodyB};
      const int selfCx = x + pet.windowW / 2;
      for (const auto& candidate : g_pets) {
        Pet* other = candidate.get();
        if (other == &pet || other->win == 0) continue;
        const Rect ob{other->posX + other->bodyL, other->posY + other->bodyT,
                      other->posX + other->bodyR, other->posY + other->bodyB};
        if (!RectsOverlap(self, ob)) continue;
        const int otherCx = other->posX + other->windowW / 2;
        const int dir = selfCx < otherCx ? 1 : -1;
        pet.vx = -pet.vx * 0.6;
        pet.vy = -pet.vy * 0.4;
        other->vx = static_cast<double>(dir) * 450.0;
        other->vy = -220.0;
        pet.lastCollisionTick = other->lastCollisionTick = now;
        if (!other->flying) {
          other->flying = true;
          other->flyTick = now;
        }
        Log("pet{} fly hit pet{} dir={}", pet.index, other->index, dir);
        break;
      }
    }
    // 贴地低速提前收尾：core 的 |v|<30 停止条件配合整数位移截断，会让 vy
    // 在贴地时一帧帧累加成大反弹，看起来像原地弹跳不停。
    if (!stopped && y >= bounds.maxY && std::fabs(pet.vy) < 80.0 &&
        std::fabs(pet.vx) < 80.0) {
      stopped = true;
    }
    // 兜底：任何异常（WM 丢配置请求等）都不允许无限飞。
    if (now - pet.flyTick > 12000) stopped = true;
    if (stopped) {
      pet.flying = false;
      pet.vx = pet.vy = 0.0;
      Log("pet{} fly stopped pos=({},{})", pet.index, x, y);
    }
    if (g_debug) {
      int rx = 0, ry = 0;
      Window child = None;
      XTranslateCoordinates(g_dpy, pet.win, g_root, 0, 0, &rx, &ry, &child);
      Log(
          "fly pet{} t={} pos=({},{}) off=({},{}) real=({},{}) v=({:.0f},{:.0f})",
          pet.index, static_cast<long long>(NowMs() - g_startTick), x, y,
          pet.drawOffX, pet.drawOffY, rx, ry, pet.vx, pet.vy);
    }
    // 最高点：vy 从负变正（上一帧向上、本帧向下），记录 pos。
    if (pet.vyPrev < 0.0 && pet.vy >= 0.0) {
      Log("fly apex pet{} pos=({},{})", pet.index, x, y);
    }
    pet.vyPrev = pet.vy;
    if (bounced) {
      Log("pet{} fly bounce vy={:.0f} vx={:.0f} pos=({},{})", pet.index, pet.vy,
          pet.vx, x, y);
    }
  }

  // 视频帧时钟：转身段（leadFrames）原地，之后每呈现一帧走固定像素，
  // 走完 dist 立刻停（tail 段静止）。平移严格跟着画面里的「跑」。
  const int frames = pet.playedFrames;
  if (needMove) {
    if (frames < pet.moveLeadFrames) {
      pet.moveLastFrames = frames;
    } else {
      const int dFrames = frames - pet.moveLastFrames;
      pet.moveLastFrames = frames;
      if (dFrames > 0) {
        int step = static_cast<int>(pet.moveSpeedPxPerFrame * dFrames);
        if (step < 1) step = 1;
        const int remain = pet.moveTotal - pet.moveDone;
        if (step > remain) step = remain;
        if (step > 0) {
          // body 四边都是窗口内偏移；StepWalk 自己加上 posX/posY 得到屏幕矩形。
          const Rect body{pet.bodyL, pet.bodyT, pet.bodyR, pet.bodyB};
          int nx = pet.posX;
          int ny = pet.posY;
          bool hitEdge = false;
          bool hitPet = false;
          StepWalk(pet.posX, pet.posY, step, pet.moveDirX, pet.moveDirY, body,
                   mon, others, &nx, &ny, &hitEdge, &hitPet);
          const bool collided = hitEdge || hitPet;
          if (collided) {
            // 撞边/撞宠物：位移方向反向一次，视频镜像/朝向不变。
            // 剩余距离 ×0.6，太小就停，所以只会回弹一小段。
            const int left = pet.moveTotal - pet.moveDone;
            const int dx = nx - pet.posX;
            const int dy = ny - pet.posY;
            pet.moveDirX = -pet.moveDirX;
            pet.moveDirY = -pet.moveDirY;
            pet.moveDone = 0;
            pet.moveTotal = static_cast<int>(left * 0.6);
            if (pet.moveTotal < 20) {
              pet.moveActive = false;
              pet.moveSpeedPxPerFrame = 0.0;
            }
            Log("pet{} walk bounce {} dir={},{} flip={} d={},{} pos={},{} total={}",
                pet.index, hitEdge ? "wall" : "pet", pet.moveDirX,
                pet.moveDirY, pet.flip ? 1 : 0, dx, dy, nx, ny,
                pet.moveTotal);
          }
          MovePetTo(pet, nx, ny);
          if (!collided) {
            pet.moveDone += step;
            if (pet.moveDone >= pet.moveTotal) {
              pet.moveActive = false;
              Log("pet{} walk stop done={} at={}ms", pet.index, pet.moveDone,
                  static_cast<long long>(NowMs() - pet.playStartTick));
            }
          }
        }
      }
    }
  }

  ++pet.frameCount;
  // 视频播完
  // 解码卡死时用墙钟兜底，避免窗口一直停在同一帧。
  const int64_t nowMs = NowMs();
  const bool ended = pet.dec.eof.load() ||
                     (pet.clipFrames > 0 && frames >= pet.clipFrames);
  const bool stalled =
      pet.playDurationMs > 0 &&
      nowMs - pet.playStartTick >
          static_cast<int64_t>(pet.playDurationMs) * 2 + 2000;
  if (ended || stalled) OnClipEnded(pet, nowMs, stalled);
  Render(pet);
}

// ===== 创建与查找 =====

void CreatePet(int index) {
  const int screenW = DisplayWidth(g_dpy, g_screen);
  const int screenH = DisplayHeight(g_dpy, g_screen);

  int targetW = static_cast<int>(screenW * g_scalePercent / 100.0);
  if (targetW < 220) targetW = 220;
  if (targetW > 640) targetW = 640;
  const int targetH = targetW * 360 / 640;

  auto pet = std::make_unique<Pet>();
  pet->index = index;
  pet->windowW = targetW;
  pet->windowH = targetH;
  pet->bodyR = targetW;
  pet->bodyB = targetH;

  XSetWindowAttributes swa{};
  swa.colormap = g_cmap;
  swa.background_pixel = 0;
  swa.border_pixel = 0;
  swa.override_redirect = g_hasWm ? False : True;
  swa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                   PointerMotionMask | StructureNotifyMask;

  // 以屏幕中心为基准、按 8×8 网格铺开：即使 50 只也全部落在屏幕内。
  // 旧实现每 3 只向下挪一行，800 高的屏幕到第 4 行就完全跑出可视区，
  // 用户看不见却又实实在在地创建、解码。
  constexpr int kGridCols = 8;
  constexpr int kGridRows = 8;  // 与 --pets 上限 64 对应
  const int gapX = targetW * 55 / 100;
  const int gapY = targetH * 55 / 100;
  const int gridW = (kGridCols - 1) * gapX + targetW;
  const int gridH = (kGridRows - 1) * gapY + targetH;
  int homeX = (screenW - gridW) / 2 + (index % kGridCols) * gapX;
  int homeY = (screenH - gridH) / 2 + (index / kGridCols) * gapY;
  // 兜底钳制：放大比例很大时网格会超出屏幕，也不能让窗口跑出去。
  if (homeX < 0) homeX = 0;
  if (homeY < 0) homeY = 0;
  if (homeX + targetW > screenW) homeX = screenW - targetW;
  if (homeY + targetH > screenH) homeY = screenH - targetH;
  pet->posX = homeX;
  pet->posY = homeY;
  unsigned long wmask = CWColormap | CWBackPixel | CWBorderPixel | CWEventMask;
  if (!g_hasWm) wmask |= CWOverrideRedirect;
  pet->win = XCreateWindow(g_dpy, g_root, homeX, homeY, targetW, targetH, 0,
                           g_visual->depth, InputOutput, g_visual->visual,
                           wmask, &swa);
  if (pet->win == 0) {
    Log("pet{} create window failed", index);
    return;
  }
  XStoreName(g_dpy, pet->win, "pet-linux");

  if (g_hasWm) {
    // 无边框 + 置顶 + 不占任务栏：交给 WM 管理（KWin 才会合成 ARGB 内容）。
    const Atom typeAtom = XInternAtom(g_dpy, "_NET_WM_WINDOW_TYPE", False);
    const Atom utility =
        XInternAtom(g_dpy, "_NET_WM_WINDOW_TYPE_UTILITY", False);
    XChangeProperty(g_dpy, pet->win, typeAtom, XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&utility), 1);

    const Atom stateAtom = XInternAtom(g_dpy, "_NET_WM_STATE", False);
    const Atom above = XInternAtom(g_dpy, "_NET_WM_STATE_ABOVE", False);
    const Atom skipTask =
        XInternAtom(g_dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
    const Atom skipPager =
        XInternAtom(g_dpy, "_NET_WM_STATE_SKIP_PAGER", False);
    const Atom states[3] = {above, skipTask, skipPager};
    XChangeProperty(g_dpy, pet->win, stateAtom, XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(states), 3);

    // Motif hints：decorations=0 去掉标题栏和边框。
    const Atom motif = XInternAtom(g_dpy, "_MOTIF_WM_HINTS", False);
    const unsigned long mwm[5] = {2 /* MWM_HINTS_DECORATIONS */, 0, 0, 0, 0};
    XChangeProperty(g_dpy, pet->win, motif, motif, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(mwm), 5);

    // USPosition：告诉 WM「这是用户指定的位置」，请照办。
    // 没有它时 KWin 的智能放置会把多只宠物全部居中叠在屏幕中央，
    // 表现为「启动 50 只却只看到一只」。
    XSizeHints hints{};
    hints.flags = USPosition | USSize;
    hints.x = homeX;
    hints.y = homeY;
    hints.width = targetW;
    hints.height = targetH;
    XSetWMNormalHints(g_dpy, pet->win, &hints);
  }

  XMapWindow(g_dpy, pet->win);
  XFlush(g_dpy);
  XSync(g_dpy, False);
  // 兜底：个别 WM 仍会改写位置，Map 之后按真实位置对齐逻辑坐标，
  // 保证第一次走路/碰撞用的是屏幕上的实际位置。
  WindowAbsPos(pet->win, &pet->posX, &pet->posY);
  if (g_hasWm && (pet->posX != homeX || pet->posY != homeY)) {
    XMoveWindow(g_dpy, pet->win, homeX, homeY);
    XSync(g_dpy, False);
    WindowAbsPos(pet->win, &pet->posX, &pet->posY);
  }

  pet->ctx = glXCreateContext(g_dpy, g_visual, g_shareCtx, True);
  if (pet->ctx == nullptr) {
    Log("pet{} create gl context failed", index);
    XDestroyWindow(g_dpy, pet->win);
    return;
  }
  if (g_shareCtx == nullptr) g_shareCtx = pet->ctx;
  Log("pet{} window+ctx ok", index);
  if (glXMakeCurrent(g_dpy, pet->win, pet->ctx) == False) {
    // make-current 失败后建 shader/贴图都作用在错误 context 上，
    // 不如尽早放弃这只宠物，避免后续 BadMatch 或资源泄漏。
    Log("pet{} make current failed, drop this pet", index);
    glXDestroyContext(g_dpy, pet->ctx);
    pet->ctx = nullptr;
    XDestroyWindow(g_dpy, pet->win);
    pet->win = 0;
    return;
  }
  if (g_prog == 0) {
    g_prog = BuildProgram();
    Log("pet{} program={}", index, g_prog);
  }

  ActionSpec firstSpec = g_startClip.empty() ? PickNextAction(1)
                                             : SpecForName(g_startClip);
  if (firstSpec.name.empty()) {
    firstSpec.kind = ActionKind::Idle;
    firstSpec.name = g_idleClips.empty() ? std::string("待机呼吸休闲")
                                         : g_idleClips[0];
  }
  if (!PlayAction(*pet, firstSpec)) {
    // 首段失败时 immediate 回退待机；仍失败则交给 Tick 的退避重试，
    // 不能让宠物停在 tex=0、无 eof、无时钟的永久定格状态。
    Log("pet{} play first clip failed, fallback to idle", index);
    if (!PlayAction(*pet, IdleSpec())) {
      pet->nextClipTick = NowMs() + 1000;
    }
  }
  g_pets.push_back(std::move(pet));
  Log("pet{} created {}x{} at {},{}", index, targetW, targetH, homeX, homeY);
}

// 找到事件对应的宠物。
Pet* PetForWindow(Window win) {
  for (const auto& p : g_pets) {
    if (p->win == win) return p.get();
  }
  return nullptr;
}

}  // namespace linuxpetops
