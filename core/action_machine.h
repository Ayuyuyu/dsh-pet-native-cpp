#pragma once

#include <cstdint>

#include "config.h"

namespace core {

// 两端共用的睡觉阶段。名字用 Awake 而不是 None：
// Linux 的 X11/Xlib.h 定义了 #define None 0L，枚举值 None 会无法编译。
enum class SleepPhase { Awake, Start, Loop, End };

// 动作状态机的全部派生状态。平台 Pet 通过组合或逐字段镜像持有它，
// 决策逻辑集中在这里，避免两端各写一份并逐渐产生分歧。
struct ActionState {
  ActionSpec spec;
  SleepPhase sleepPhase = SleepPhase::Awake;
  int facing = 1;            // 1 原样 / -1 镜像
  bool walkFlipped = false;  // 镜像是否来自 move.flip，切走时恢复
  bool dragMode = false;
  int64_t nextClipTick = 0;  // 打开失败后的退避重试时刻

  int clipFrames = 0;
  int playedFrames = 0;
  int64_t playStartTick = 0;
  int playDurationMs = 0;

  bool moveActive = false;
  int moveDirX = 0;
  int moveDirY = 0;
  int moveTotal = 0;
  int moveDone = 0;
  int moveLeadFrames = 0;
  int moveLastFrames = 0;
  double moveSpeedPxPerFrame = 0.0;
};

// 提交动作前的快照与失败后的整体恢复。
ActionState SnapshotAction(const ActionState& st);
void RestoreAction(ActionState& st, const ActionState& snapshot);

// 按 spec 设置派生状态（转身、睡觉、拖拽、移动镜像）；不触碰任何平台资源。
void ApplyActionSpec(ActionState& st, const ActionSpec& spec);

// 片段自然结束后的统一决策。返回空 name 表示本段不切（loop / drag / stalled）。
// 调用方拿到返回 spec 后自行 PlayAction；失败时把 nextClipTick 设为 +1s。
ActionSpec NextActionAfterEnd(ActionState& st, int64_t nowMs, bool stalled);

// 移动时间轴：dist*1.8 系数是两端统一的提前量，剩余距离由物理层钳制。
struct MoveTiming {
  int leadFrames = 0;
  int moveFrames = 0;
  double speedPxPerFrame = 0.0;
};
MoveTiming ComputeMoveTiming(const MoveParams& mv, double fps, int clipFrames,
                             int distance);

// 解码卡死的墙钟兜底。
bool IsStalled(const ActionState& st, int64_t nowMs);

}  // namespace core
