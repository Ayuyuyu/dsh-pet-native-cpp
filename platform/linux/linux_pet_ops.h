#pragma once

// Pet 的 Linux 平台行为：窗口几何、每帧管线（GL + 掩码 + XShape）、
// 动作状态机与每帧 Tick（对应 Win 端的 win_pet_ops.*）。
#include <X11/Xlib.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/physics.h"
#include "platform/linux/linux_state.h"

namespace linuxpetops {

// ---- 窗口几何 ----
// 仅列跨文件使用的入口；MonitorFor / WindowAbsPos / CollectOthers /
// ApplyInputShape / UploadFrame / PlayClip / OnClipEnded 都是本模块内部实现。
core::Rect WorkAreaRect();
void MovePetTo(Pet& pet, int x, int y);
void ApplySizeAll(int delta);  // delta 为 ±5 的增量百分比

// ---- 动作状态机 ----
// 决策逻辑在 core::ActionMachine；这里只负责 PlayClip 的 IO 与失败回滚。
bool PlayAction(Pet& pet, const core::ActionSpec& spec);

// 右键菜单「睡觉/起来」：未睡时播开始段（播完自动进循环段），
// 已睡时播结束段（播完回到正常随机）。
void ToggleSleep(Pet& pet);

// ---- 每帧 Tick ----
void Tick(Pet& pet);

// 创建窗口 + GL 上下文并播放首段。
void CreatePet(int index);

// 事件对应的宠物（借用指针）。
Pet* PetForWindow(Window win);

}  // namespace linuxpetops
