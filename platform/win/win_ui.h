#pragma once

// Win 平台层的用户交互入口：右键动作菜单与托盘图标。
// 两者都只依赖 core 配置与 win_util，不依赖 Pet / 窗口状态机。
#ifndef NOMINMAX
#define NOMINMAX  // 避免 windows.h 的 min/max 宏破坏 std::min/std::max
#endif
#include <windows.h>

#include <string>
#include <vector>

namespace winui {

// ---- 右键菜单 ----
// 动作项 ID = kIdActionBase + g_menuNames 下标，WM_COMMAND 由主程序查表点播。
constexpr UINT kIdActionBase = 1000;
constexpr UINT kIdExit = 2002;
constexpr UINT kIdAddPet = 3003;
constexpr UINT kIdBigger = 3004;
constexpr UINT kIdSmaller = 3005;
constexpr UINT kIdClosePet = 3006;
constexpr UINT kIdSleepToggle = 3007;

// 每次构建菜单时重建；主程序启动时也预填一份供外部点播使用。
extern std::vector<std::string> g_menuNames;

// sleeping 为真时「睡觉」项显示为「起来」。
void BuildPetMenu(HWND hwnd, int sizePercent, bool sleeping);

// ---- 托盘 ----
constexpr UINT kTrayMsg = WM_APP + 100;
constexpr UINT kTrayId = 1;
constexpr UINT kIdTrayToggle = 3001;
constexpr UINT kIdTrayExit = 3002;

void CreateTray(HWND owner);
void RemoveTray(HWND owner);
HWND TrayOwner();

// windows 为当前所有宠物窗口；可见状态保存在本模块内。
void TogglePetsVisible(const std::vector<HWND>& windows);

}  // namespace winui
