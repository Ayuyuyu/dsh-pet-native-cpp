#pragma once

// Linux 平台层的用户交互入口：GTK 右键菜单（对应 Win 端的 win_ui.*）。
#include <cstdint>

#include "platform/linux/linux_state.h"

namespace linuxui {

// 在鼠标位置弹出菜单（右键）。重复调用会先销毁旧菜单。
void ShowMenu(Pet& pet, int rootX, int rootY);

// 菜单失焦 / 菜单外点击检测：内部做 50ms 节流，主循环每帧调用即可。
void UpdateMenu(int64_t nowMs);

// 退出前主动销毁菜单：MenuAction 借用 Pet*，不能让它们在 g_pets 析构后
// 还留在 GTK 对象树里等待事件派发。
void CloseMenu();

}  // namespace linuxui
