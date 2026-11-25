#pragma once

// Win 平台层的宠物数据与全局状态声明。
// 只放类型与 extern 声明，定义在 win_main.cpp 的 winpet namespace 里；
// 各 Win 模块共享此头文件，避免 Pet 结构被绑死在主文件中。
#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "core/action_machine.h"
#include "core/config.h"
#include "core/mask.h"
#include "platform/win/render_d3d11.h"
#include "platform/win/win_clip_pool.h"

namespace winpet {

// 睡觉状态机：开始 → 循环（无限）→ 起来 → 结束回到正常随机。
// 定义收进 core，两端共用同一套决策；Awake 而非 None 也避开 Xlib 的同名宏。
using SleepPhase = core::SleepPhase;

struct Pet {
  HWND hwnd = nullptr;
  int windowW = 640;
  int windowH = 360;
  int index = 0;

  std::string currentClip;
  // 共享动画实例：解码器与纹理归 winclips 所有，这里只借用，切段时换指针。
  winclips::ClipInstance* clip = nullptr;
  unsigned lastStamp = 0;  // 上次看到的实例帧戳，用于判断本帧是否有新画面

  int frameCount = 0;       // 渲染帧序号（调试导出用）
  // 视频帧号，直接取 clip->playedFrames：moves 是独占实例，从 1 起算，
  // 正好与 lead/move/tail 的时间轴对齐。
  int playedFrames = 0;
  int clipFrames = 0;       // 当前动作总帧数（clip->clipFrames）
  DWORD playStartTick = 0;  // 仅作解码卡死时的兜底计时（ms）
  int playDurationMs = 0;   // 当前动作真实时长（ms）
  // 下次允许切段的时刻：切段失败（打开素材失败）后退避 1 秒再重试，
  // 避免每帧重试把 CPU 打满，同时也避免一次失败就永久定格。
  DWORD nextClipTick = 0;
  // 上次拿到新视频帧的时刻：连续 3 秒没新帧就把完整状态写进日志，
  // 用于定位「画面卡住」到底卡在解码、实例可见性还是切段判定上。
  DWORD lastFrameTick = 0;

  WinPetSurface rt;  // 每只宠物独占的交换链 / DComp visual / 常量缓冲

  // 当前帧可见区域（窗口坐标），用于碰撞判断。
  int bodyL = 0;
  int bodyT = 0;
  int bodyR = 640;
  int bodyB = 360;

  // 碰撞缓存：窗口左上角 + body 的屏幕绝对矩形。
  // 只在 SetWindowPos 之后与掩码更新时刷新，物理阶段不再逐只 GetWindowRect。
  int winX = 0;
  int winY = 0;
  core::Rect screenBody;
  std::vector<core::Rect> othersScratch;  // 收集其他宠物时的复用缓冲

  // 窗口区域缓存：与画面同源生成，但只在掩码变化超过阈值时重建，
  // 避免 50 只 × 24fps 每秒 1200 次同步 SetWindowRgn 把 UI 线程堵住。
  std::vector<char> regionMask;
  int regionW = 0;
  int regionH = 0;
  int forceRegionFrames = 0;  // 切段/缩放后强制重建几帧，防止旧区域裁掉新动作

  bool moveActive = false;
  int moveDirX = 0;
  int moveDirY = 0;
  int moveTotal = 0;
  int moveDone = 0;
  int moveLeadFrames = 0;          // 转身段帧数，此前原地
  int moveLastFrames = 0;          // 上次平移时的视频帧号
  double moveSpeedPxPerFrame = 0.0;

  float scaleY = 1.0f;
  bool flip = false;  // 由 facing 派生，渲染与命中测试共用
  DWORD squashStart = 0;
  bool squashActive = false;

  // 本帧必须重绘：新视频帧 / Q 弹动画期间由 Tick 自行判定，
  // 这个标志留给「内容没变但外观变了」的一次性场景（缩放、换实例、首帧）。
  bool needPresent = false;

  // 动作状态机。
  core::ActionSpec spec;
  int facing = 1;            // 1 原样 / -1 镜像，跨动画保持
  bool walkFlipped = false;  // 镜像是否由 move.flip 引起，切走时恢复
  bool dragMode = false;
  bool dragAnimStarted = false;
  POINT dragStartPos{};

  // 睡觉：菜单点「睡觉」播开始段，播完自动接循环段（loop），
  // 点「起来」播结束段，播完回到正常随机。
  SleepPhase sleepPhase = SleepPhase::Awake;

  bool dragging = false;
  POINT dragOffset{};
  POINT dragLastPos{};
  DWORD dragLastTick = 0;

  // 遮挡暂停：被其它窗口/宠物完全盖住（或跑到屏幕外）时停止解码与绘制，
  bool occluded = false;
  bool visible = false;          // 是否已向当前实例登记为「可见」
  DWORD lastOcclusionTick = 0;   // 上次遮挡探测的时刻（低频，1.5s 一次）

  bool closing = false;

  // 甩出去的惯性飞行。
  bool flying = false;
  double vx = 0.0;
  double vy = 0.0;
  DWORD lastCollisionTick = 0;  // 飞行撞宠物冷却（老版 800ms）
};

// 全局状态集中声明，定义见 win_main.cpp。
extern std::vector<std::unique_ptr<Pet>> g_pets;
extern int g_sizePercent;
extern std::string g_startClip;

}  // namespace winpet
