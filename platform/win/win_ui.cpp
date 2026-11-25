#include "platform/win/win_ui.h"

#include <shellapi.h>

#include <algorithm>
#include <iterator>
#include <string_view>

#include "core/config.h"
#include "platform/win/win_util.h"

namespace winui {

// ===== 右键菜单 =====

std::vector<std::string> g_menuNames;

void BuildPetMenu(HWND hwnd, int sizePercent, bool sleeping) {
  g_menuNames.clear();
  winutil::ScopedMenu root(CreatePopupMenu());
  winutil::ScopedMenu actions(CreatePopupMenu());
  if (root.get() == nullptr || actions.get() == nullptr) return;

  auto addGroup = [&](const wchar_t* label,
                      const std::vector<std::string>& items) {
    if (items.empty()) return;
    winutil::ScopedMenu sub(CreatePopupMenu());
    for (const auto& name : items) {
      const int id = kIdActionBase + static_cast<int>(g_menuNames.size());
      g_menuNames.push_back(name);
      AppendMenuW(sub.get(), MF_STRING, id, winutil::ToWide(name).c_str());
    }
    // MF_POPUP 挂到父菜单后由父菜单的 DestroyMenu 级联销毁。
    if (AppendMenuW(actions.get(), MF_POPUP,
                    reinterpret_cast<UINT_PTR>(sub.get()), label)) {
      sub.release();
    }
  };

  addGroup(L"待机", core::g_idleClips);
  addGroup(L"转向", core::g_turnClips);
  addGroup(L"拖拽", core::g_dragClips);
  addGroup(L"点击回应", core::g_clickClips);
  addGroup(L"移动", core::g_moveNames);
  for (const auto& cat : core::g_categories) {
    addGroup(winutil::ToWide(cat.first).c_str(), cat.second);
  }
  for (const auto& ev : core::g_eventGroups) {
    addGroup(winutil::ToWide(ev.first).c_str(), ev.second);
  }

  if (AppendMenuW(root.get(), MF_POPUP,
                  reinterpret_cast<UINT_PTR>(actions.get()), L"动作")) {
    actions.release();
  }
  // 睡觉是常驻项：睡着时文案变成「起来」。
  AppendMenuW(root.get(), MF_STRING, kIdSleepToggle,
              sleeping ? L"起来" : L"睡觉");
  AppendMenuW(root.get(), MF_SEPARATOR, 0, nullptr);
  AppendMenuW(root.get(), MF_STRING, kIdAddPet, L"再来一只");
  AppendMenuW(root.get(), MF_STRING, kIdClosePet, L"关闭这一只");
  winutil::ScopedMenu size_menu(CreatePopupMenu());
  AppendMenuW(size_menu.get(), MF_STRING, kIdBigger, L"放大 5%");
  AppendMenuW(size_menu.get(), MF_STRING, kIdSmaller, L"缩小 5%");
  {
    const std::wstring title =
        L"大小（" + std::to_wstring(sizePercent) + L"%）";
    if (AppendMenuW(root.get(), MF_POPUP,
                    reinterpret_cast<UINT_PTR>(size_menu.get()),
                    title.c_str())) {
      size_menu.release();
    }
  }
  AppendMenuW(root.get(), MF_SEPARATOR, 0, nullptr);
  AppendMenuW(root.get(), MF_STRING, kIdExit, L"退出");

  POINT pt{};
  GetCursorPos(&pt);
  if (SetForegroundWindow(hwnd)) {
    TrackPopupMenu(root.get(), TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    // TrackPopupMenu 需要 WM_NULL 收尾，否则部分系统上菜单不会随失焦关闭。
    PostMessageW(hwnd, WM_NULL, 0, 0);
  }
  // root 析构自动 DestroyMenu，并级联销毁已挂载的子菜单。
}

// ===== 托盘 =====

namespace {
bool g_petsVisible = true;
HICON g_trayOwnedIcon = nullptr;  // LoadImageW 得到的图标，需要 DestroyIcon
HWND g_trayOwner = nullptr;       // 托盘项按 (hWnd, uID) 寻址，记住真正宿主
}  // namespace

void CreateTray(HWND owner) {
  if (owner == nullptr || g_trayOwner == owner) return;
  if (g_trayOwner != nullptr) RemoveTray(g_trayOwner);  // 换宿主时先清理旧项
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = owner;
  nid.uID = kTrayId;
  nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  nid.uCallbackMessage = kTrayMsg;
  HICON icon = static_cast<HICON>(
      LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON,
                 0, 0, LR_DEFAULTSIZE));
  if (icon != nullptr) g_trayOwnedIcon = icon;
  // 系统共享图标不归本进程所有，不能 DestroyIcon。
  nid.hIcon = icon != nullptr ? icon : LoadIconW(nullptr, IDI_APPLICATION);
  constexpr std::wstring_view kTip = L"原生桌宠";
  std::copy_n(kTip.begin(), std::min(kTip.size(), std::size(nid.szTip) - 1),
              nid.szTip);
  if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
    if (icon != nullptr) DestroyIcon(icon);
    g_trayOwnedIcon = nullptr;
    return;
  }
  g_trayOwner = owner;
}

HWND TrayOwner() { return g_trayOwner; }

void RemoveTray(HWND owner) {
  // 非宿主宠物销毁时不得删托盘项，更不能销毁任务栏仍在使用的图标。
  if (g_trayOwner == nullptr || g_trayOwner != owner) return;
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = owner;
  nid.uID = kTrayId;
  Shell_NotifyIconW(NIM_DELETE, &nid);
  // 必须等托盘项删除后再销毁图标，否则任务栏会拿到失效句柄。
  if (g_trayOwnedIcon != nullptr) {
    DestroyIcon(g_trayOwnedIcon);
    g_trayOwnedIcon = nullptr;
  }
  g_trayOwner = nullptr;
}

void TogglePetsVisible(const std::vector<HWND>& windows) {
  g_petsVisible = !g_petsVisible;
  for (HWND hwnd : windows) {
    ShowWindow(hwnd, g_petsVisible ? SW_SHOWNOACTIVATE : SW_HIDE);
  }
}

}  // namespace winui
