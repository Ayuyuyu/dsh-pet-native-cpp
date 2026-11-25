#include "action_machine.h"

#include <algorithm>

namespace core {

ActionState SnapshotAction(const ActionState& st) { return st; }

void RestoreAction(ActionState& st, const ActionState& snapshot) { st = snapshot; }

void ApplyActionSpec(ActionState& st, const ActionSpec& spec) {
  // 任何非睡觉动作都视为已醒：点击/拖拽/菜单点播会清掉残留的睡意。
  if (spec.kind != ActionKind::Sleep) st.sleepPhase = SleepPhase::Awake;
  // 移动镜像是临时状态；切走任何动作都要先恢复全局朝向。
  if (st.walkFlipped) {
    st.walkFlipped = false;
    st.facing = 1;
  }
  st.spec = spec;
  st.dragMode = (spec.kind == ActionKind::Drag);
  if (spec.kind == ActionKind::Move) {
    const auto it = g_moves.find(spec.name);
    if (it != g_moves.end() && it->second.flip != 0) {
      st.walkFlipped = true;
      st.facing = -1;
    }
  }
}

ActionSpec NextActionAfterEnd(ActionState& st, int64_t nowMs, bool stalled) {
  (void)nowMs;
  // loop 由解码线程自行回绕，不切段；drag 反馈也必须等外部松手。
  if ((st.spec.loop || st.dragMode) && !stalled) return {};
  // turn 播完真正翻转全局朝向。
  if (st.spec.kind == ActionKind::Turn) st.facing = -st.facing;

  // 睡觉：开始段 → 循环段（loop 由解码线程回绕）→ 起来段 → 正常链。
  if (st.sleepPhase == SleepPhase::Start) {
    st.sleepPhase = SleepPhase::Loop;
    ActionSpec loop_spec;
    loop_spec.kind = ActionKind::Sleep;
    loop_spec.name = PickFromPool(g_sleepClips.loop);
    loop_spec.loop = true;
    if (!loop_spec.name.empty()) return loop_spec;
    st.sleepPhase = SleepPhase::Awake;  // 循环段缺失，退回正常流程
  }
  if (st.sleepPhase == SleepPhase::End) st.sleepPhase = SleepPhase::Awake;

  ActionSpec next = (g_idleAfterAction && st.spec.kind != ActionKind::Idle)
                        ? IdleSpec()
                        : PickNextAction(st.facing);
  if (next.name.empty()) next = IdleSpec();
  return next;
}

MoveTiming ComputeMoveTiming(const MoveParams& mv, double fps, int clipFrames,
                             int distance) {
  MoveTiming out;
  const double safeFps = fps > 0 ? fps : 30.0;
  out.leadFrames = static_cast<int>(mv.leadSec * safeFps);
  const int tailFrames = static_cast<int>(mv.tailSec * safeFps);
  out.moveFrames = clipFrames - out.leadFrames - tailFrames;
  out.speedPxPerFrame = out.moveFrames > 0
                            ? static_cast<double>(distance) * 1.8 / out.moveFrames
                            : 0.0;
  return out;
}

bool IsStalled(const ActionState& st, int64_t nowMs) {
  return st.playDurationMs > 0 &&
         nowMs - st.playStartTick >
             static_cast<int64_t>(st.playDurationMs) * 2 + 2000;
}

}  // namespace core
