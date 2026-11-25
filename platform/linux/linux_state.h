#pragma once

// Linux 平台层的宠物数据与进程级状态声明。
// 定义集中在 linux_state.cpp，各模块共享此头文件（对应 Win 端的 win_pet.h）。
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#endif
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/action_machine.h"
#include "core/config.h"
#include "core/decode.h"
#include "core/mask.h"

// X11 / GLX 的进程级状态。
extern Display* g_dpy;
extern int g_screen;
extern Window g_root;
extern GLuint g_prog;
extern XVisualInfo* g_visual;
// g_visual 是否由 X 分配（glXChooseVisual）需要 XFree；
// 有合成器时它指向 InitVisual 里的 static XVisualInfo，绝不能释放。
extern bool g_visualOwned;
extern Colormap g_cmap;
extern GLXContext g_shareCtx;
extern bool g_hasCompositor;
extern bool g_hasWm;
extern bool g_running;
extern bool g_gtkReady;  // gtk_init_check 成功前不得调用任何 GTK/GDK API
extern bool g_dragAny;
extern bool g_menuTest;  // --menu-test：启动后自动弹一次菜单
extern bool g_sleepTest;  // --sleep-test：自动睡觉再起来（无鼠标环境验证状态机）
extern bool g_exitTest;   // --exit-test：启动几秒后自动退出（验证清理路径）
extern bool g_debug;
extern std::string g_startClip;  // --clip=动作名
extern std::string g_clipsRoot;  // main 里按 exe 目录解析后填入
extern double g_scalePercent;
// 单调宠物 id：关闭中间一只后不能复用 g_pets.size()，否则日志与菜单里
// 会出现两个同号宠物。网格槽位另按当前数量计算。
extern int g_nextPetId;

// 睡觉状态机（与 Win 端同构）：开始 → 循环（无限）→ 起来 → 回到正常随机。
// 定义收进 core，两端共用同一套决策；Awake 而非 None 避开 Xlib 的同名宏。
using SleepPhase = core::SleepPhase;

struct Pet {
  Window win = 0;
  GLXContext ctx = nullptr;
  int windowW = 384;
  int windowH = 216;
  GLuint tex = 0;
  int texW = 0;  // tex 的实际尺寸：尺寸不变时复用纹理，避免每次切段重建
  int texH = 0;

  core::DecodeState dec;
  core::MaskResult maskResult;
  std::vector<char> regionMask;
  int forceRegionFrames = 0;
  int frameCount = 0;  // 渲染帧序号（16ms Tick，仅用于启动位置校准）
  int playedFrames = 0;     // 已呈现的视频帧数：状态机的视频时钟
  int clipFrames = 0;       // 当前动作真实帧数（dec.clipFrames - 1）
  int64_t playStartTick = 0;  // 仅作解码卡死时的兜底计时（ms）
  int playDurationMs = 0;     // 当前动作真实时长（ms）
  int64_t nextClipTick = 0;   // 无有效片段时的退避重试时刻
  std::string currentClip;

  bool moveActive = false;
  int moveDirX = 0;
  int moveDirY = 0;
  int moveTotal = 0;
  int moveDone = 0;
  int moveLeadFrames = 0;  // 转身段帧数，此前原地
  int moveLastFrames = 0;  // 上次平移时的视频帧号
  double moveSpeedPxPerFrame = 0.0;

  bool flying = false;
  double vx = 0.0;
  double vy = 0.0;
  double vyPrev = 0.0;  // 上一帧 vy，用于检测弹跳最高点
  int64_t flyTick = 0;
  int64_t lastCollisionTick = 0;  // 飞行撞宠物冷却（老版 800ms）

  bool dragging = false;
  int dragOffX = 0;
  int dragOffY = 0;
  double dragLastX = 0.0;
  double dragLastY = 0.0;
  int64_t dragLastTick = 0;

  int bodyL = 0;
  int bodyT = 0;
  int bodyR = 640;
  int bodyB = 360;

  // 窗口在 root（屏幕）坐标系里的位置，自己维护。
  // 有 WM 时窗口会被 reparent 到 frame，XGetWindowAttributes 拿到的是相对
  // frame 的坐标；WM 又异步处理 ConfigureRequest，读回的位置还会滞后。
  int posX = 0;
  int posY = 0;
  bool posDirty = false;  // 程序自己移动过窗口后，不再按 WM 摆放校准
  // 窗口被 WM 约束在工作区内时，身体内容在窗口内的补偿平移。
  int drawOffX = 0;
  int drawOffY = 0;

  float scaleY = 1.0f;
  bool flip = false;  // 由 facing 派生，渲染与掩码共用
  int64_t squashStart = 0;
  bool squashActive = false;

  // 动作状态机（与 Win 端同构）。
  core::ActionSpec spec;
  int facing = 1;
  bool walkFlipped = false;
  bool dragMode = false;
  bool dragAnimStarted = false;
  double dragStartX = 0.0;
  double dragStartY = 0.0;

  // 睡觉：菜单点「睡觉」播开始段，播完自动接循环段（loop），
  // 点「起来」播结束段，播完回到正常随机。
  SleepPhase sleepPhase = SleepPhase::Awake;

  int index = 0;
  bool closing = false;
};

// 每只宠物一份所有权；函数参数里的 Pet* 仍是借用指针。
extern std::vector<std::unique_ptr<Pet>> g_pets;
extern int64_t g_startTick;
