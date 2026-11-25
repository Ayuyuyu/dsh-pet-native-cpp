#include "platform/linux/linux_ui.h"

#include <gdk/gdkx.h>
#include <gtk/gtk.h>

#include <X11/Xlib.h>

#include <string>

#include "core/config.h"
#include "core/log.h"
#include "platform/linux/linux_pet_ops.h"
#include "platform/linux/linux_util.h"

using namespace core;       // NOLINT
using namespace core::log;  // Log(...) 与 Win 端保持同形
using namespace linuxpetops;
using namespace linuxutil;

namespace linuxui {

namespace {

enum MenuCmd {
  kCmdClip,
  kCmdAdd,
  kCmdClose,
  kCmdBigger,
  kCmdSmaller,
  kCmdSleep,
  kCmdQuit,
  kCmdPageMain,
  kCmdPageActions,
  kCmdPageClips,
};

struct MenuAction {
  Pet* pet = nullptr;
  std::string clip;
  MenuCmd cmd = kCmdClip;
  GtkWidget* menuWin = nullptr;
  int category = -1;  // -1 待机 / -2 点击回应 / -3 移动 / -4 转向 / -5 拖拽
                      // <=-100 事件组（idx = -100-category） / >=0 分类索引
};

// 菜单 = 一个 POPUP 窗口 + 可重建的内容区，实现「主菜单 / 分类 / 动作」分页。
GtkWidget* g_menuWin = nullptr;
GtkWidget* g_menuBody = nullptr;
Pet* g_menuPet = nullptr;
int g_menuAnchorX = 0;  // 右键时的鼠标 root 坐标
int g_menuAnchorY = 0;
int g_menuX = 0;  // PlaceMenu 算出的菜单左上角
int g_menuY = 0;
int g_menuW = 0;
int g_menuH = 0;
Window g_menuXid = None;
Window g_menuFocusAtOpen = None;  // 打开菜单时的 X 输入焦点
int64_t g_menuNextCheck = 0;      // 失焦检测节流
int64_t g_menuOpenTick = 0;       // 菜单打开时刻（按键检测宽限用）

void FreeMenuAction(gpointer data, GClosure*) {
  // GTK destroy notifier 归还此前 release 出去的所有权，交给 RAII 释放。
  std::unique_ptr<MenuAction> act(static_cast<MenuAction*>(data));
  (void)act;
}

// 菜单放在鼠标右上方（左下角贴鼠标点）：在屏幕底部右键时向上展开，
// 不会溢出。分页后内容变高（动作列表带 360px 滚动区），每次重建都要
// 重新定位；对齐 Win32 TrackPopupMenu 自动翻到放得下的一侧。
void PlaceMenu() {
  if (g_menuWin == nullptr) return;
  GtkRequisition req{};
  gtk_widget_get_preferred_size(g_menuWin, nullptr, &req);
  const Rect work = WorkAreaRect();
  int x = g_menuAnchorX;
  int y = g_menuAnchorY - req.height;
  if (y < work.t) y = g_menuAnchorY;  // 上方放不下 → 退回鼠标下方
  if (req.width > 0 && x + req.width > work.r) x = work.r - req.width;
  if (req.height > 0 && y + req.height > work.b) y = work.b - req.height;
  if (x < work.l) x = work.l;
  if (y < work.t) y = work.t;
  g_menuX = x;
  g_menuY = y;
  g_menuW = req.width;
  g_menuH = req.height;
  gtk_window_move(GTK_WINDOW(g_menuWin), x, y);
  GdkWindow* gdkWin = gtk_widget_get_window(g_menuWin);
  if (gdkWin != nullptr) {
    const Window xid = gdk_x11_window_get_xid(gdkWin);
    if (xid != None) {
      XMoveWindow(g_dpy, xid, x, y);
      XRaiseWindow(g_dpy, xid);
      XFlush(g_dpy);
    }
  }
  Log("menu placed at {},{} size={}x{} anchor=({},{})", x, y, req.width,
      req.height, g_menuAnchorX, g_menuAnchorY);
}

void BuildMainPage();
void BuildActionPage();
void BuildClipPage(int category);

void ClearMenuBody() {
  if (g_menuBody == nullptr) return;
  GListPtr children(gtk_container_get_children(GTK_CONTAINER(g_menuBody)));
  for (GList* it = children.get(); it != nullptr; it = it->next) {
    gtk_widget_destroy(GTK_WIDGET(it->data));
  }
}

void OnMenuButtonClicked(GtkButton*, gpointer data) {
  auto* act = static_cast<MenuAction*>(data);
  if (act == nullptr || act->pet == nullptr) return;
  // gtk_widget_destroy 会同步销毁当前按钮，触发 destroy notifier 释放 act。
  // 先拷贝所需字段，销毁之后只能使用这些局部副本。
  const MenuCmd cmd = act->cmd;
  Pet* pet = act->pet;
  const std::string clip = act->clip;
  const int category = act->category;
  GtkWidget* menuWin = act->menuWin;

  // 分页命令只切页，不关闭菜单。
  switch (cmd) {
    case kCmdPageMain:
      BuildMainPage();
      return;
    case kCmdPageActions:
      BuildActionPage();
      return;
    case kCmdPageClips:
      BuildClipPage(category);
      return;
    default:
      break;
  }
  if (menuWin != nullptr) gtk_widget_destroy(menuWin);
  switch (cmd) {
    case kCmdClip:
      pet->dragAnimStarted = false;
      PlayAction(*pet, SpecForName(clip));
      break;
    case kCmdAdd:
      CreatePet(g_nextPetId++);
      break;
    case kCmdClose:
      pet->closing = true;
      break;
    case kCmdBigger:
      ApplySizeAll(5);
      break;
    case kCmdSmaller:
      ApplySizeAll(-5);
      break;
    case kCmdSleep:
      ToggleSleep(*pet);
      break;
    case kCmdQuit:
      g_running = false;
      break;
    default:
      break;
  }
}

GtkWidget* AddMenuButton(GtkWidget* parent, const char* label, MenuCmd cmd,
                         const std::string& clip = std::string(),
                         int category = -1) {
  GtkWidget* btn = gtk_button_new_with_label(label);
  gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
  gtk_widget_set_size_request(btn, 200, 28);
  if (GtkWidget* child = gtk_bin_get_child(GTK_BIN(btn))) {
    gtk_label_set_xalign(GTK_LABEL(child), 0.0f);
  }
  auto act = std::make_unique<MenuAction>(
      MenuAction{g_menuPet, clip, cmd, g_menuWin, category});
  // 所有权交给 GTK 的 destroy notifier（FreeMenuAction）。
  g_signal_connect_data(btn, "clicked", G_CALLBACK(OnMenuButtonClicked),
                        act.release(), FreeMenuAction,
                        static_cast<GConnectFlags>(0));
  gtk_box_pack_start(GTK_BOX(parent), btn, FALSE, FALSE, 0);
  return btn;
}

void BuildMainPage() {
  ClearMenuBody();
  AddMenuButton(g_menuBody, "再来一只", kCmdAdd);
  AddMenuButton(g_menuBody, "关闭这一只", kCmdClose);
  AddMenuButton(g_menuBody, "放大 5%", kCmdBigger);
  AddMenuButton(g_menuBody, "缩小 5%", kCmdSmaller);
  gtk_box_pack_start(GTK_BOX(g_menuBody),
                     gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE,
                     FALSE, 2);
  AddMenuButton(g_menuBody, "动作  >", kCmdPageActions);
  // 睡觉是常驻项：睡着时文案变成「起来」。
  const bool sleeping =
      g_menuPet != nullptr &&
      (g_menuPet->sleepPhase == SleepPhase::Start ||
       g_menuPet->sleepPhase == SleepPhase::Loop);
  AddMenuButton(g_menuBody, sleeping ? "起来" : "睡觉", kCmdSleep);
  gtk_box_pack_start(GTK_BOX(g_menuBody),
                     gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE,
                     FALSE, 2);
  AddMenuButton(g_menuBody, "退出", kCmdQuit);
  gtk_widget_show_all(g_menuBody);
  PlaceMenu();  // 分页后内容高度变了，重新定位，避免底部溢出
}

void BuildActionPage() {
  ClearMenuBody();
  AddMenuButton(g_menuBody, "<- 返回", kCmdPageMain);
  gtk_box_pack_start(GTK_BOX(g_menuBody),
                     gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE,
                     FALSE, 2);
  AddMenuButton(g_menuBody, "待机  >", kCmdPageClips, std::string(), -1);
  AddMenuButton(g_menuBody, "转向  >", kCmdPageClips, std::string(), -4);
  AddMenuButton(g_menuBody, "拖拽  >", kCmdPageClips, std::string(), -5);
  AddMenuButton(g_menuBody, "点击回应  >", kCmdPageClips, std::string(), -2);
  AddMenuButton(g_menuBody, "移动  >", kCmdPageClips, std::string(), -3);
  for (int i = 0; i < static_cast<int>(g_categories.size()); ++i) {
    const std::string label = g_categories[i].first + "  >";
    AddMenuButton(g_menuBody, label.c_str(), kCmdPageClips, std::string(), i);
  }
  for (int i = 0; i < static_cast<int>(g_eventGroups.size()); ++i) {
    const std::string label = g_eventGroups[i].first + "  >";
    AddMenuButton(g_menuBody, label.c_str(), kCmdPageClips, std::string(),
                  -100 - i);
  }
  gtk_widget_show_all(g_menuBody);
  PlaceMenu();  // 分页后内容高度变了，重新定位，避免底部溢出
}

void BuildClipPage(int category) {
  ClearMenuBody();
  AddMenuButton(g_menuBody, "<- 返回", kCmdPageActions);
  gtk_box_pack_start(GTK_BOX(g_menuBody),
                     gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE,
                     FALSE, 2);

  const std::vector<std::string>* clips = nullptr;
  if (category == -1) {
    clips = &g_idleClips;
  } else if (category == -2) {
    clips = &g_clickClips;
  } else if (category == -3) {
    clips = &g_moveNames;
  } else if (category == -4) {
    clips = &g_turnClips;
  } else if (category == -5) {
    clips = &g_dragClips;
  } else if (category <= -100) {
    const int idx = -100 - category;
    if (idx >= 0 && idx < static_cast<int>(g_eventGroups.size())) {
      clips = &g_eventGroups[static_cast<size_t>(idx)].second;
    }
  } else if (category >= 0 &&
             category < static_cast<int>(g_categories.size())) {
    clips = &g_categories[static_cast<size_t>(category)].second;
  }
  // 无效分类也走完整构建/布局流程，避免菜单沿用上一页的尺寸和位置。
  static const std::vector<std::string> kEmptyClips;
  if (clips == nullptr) clips = &kEmptyClips;

  GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_widget_set_size_request(scroll, 210, 360);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  GtkWidget* list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  for (const auto& name : *clips) {
    AddMenuButton(list, name.c_str(), kCmdClip, name);
  }
  gtk_container_add(GTK_CONTAINER(scroll), list);
  gtk_box_pack_start(GTK_BOX(g_menuBody), scroll, TRUE, TRUE, 0);
  gtk_widget_show_all(g_menuBody);
  PlaceMenu();  // 分页后内容高度变了，重新定位，避免底部溢出
}

// 菜单是 override_redirect 窗口：WM 不给它焦点通知，GTK 事件只能覆盖部分场景，
// 不能单靠 gtk_grab_add / button-press-event 收起菜单。
// 由主循环定期检测焦点转移和菜单外的按键，模拟 Win32 TrackPopupMenu 的收起。
void CheckMenuDismiss() {
  if (g_menuWin == nullptr) return;
  Window focus = None;
  int revert = 0;
  XGetInputFocus(g_dpy, &focus, &revert);
  if (focus != g_menuFocusAtOpen && focus != g_menuXid) {
    gtk_widget_destroy(g_menuWin);
    return;
  }
  // 右键 press→release 期间指针正好停在菜单左下角边界，先给 250ms 宽限，
  // 否则 Button3Mask 会被当成「菜单外点击」把刚弹出的菜单立刻关掉。
  if (NowMs() - g_menuOpenTick < 250) return;
  Window rootRet = None;
  Window child = None;
  int rx = 0, ry = 0, wx = 0, wy = 0;
  unsigned int mask = 0;
  if (!XQueryPointer(g_dpy, g_root, &rootRet, &child, &rx, &ry, &wx, &wy,
                     &mask)) {
    return;
  }
  constexpr int kMargin = 4;
  if ((mask & (Button1Mask | Button2Mask | Button3Mask)) != 0 &&
      (rx < g_menuX - kMargin || rx >= g_menuX + g_menuW + kMargin ||
       ry < g_menuY - kMargin || ry >= g_menuY + g_menuH + kMargin)) {
    gtk_widget_destroy(g_menuWin);
  }
}

}  // namespace

void ShowMenu(Pet& pet, int rootX, int rootY) {
  if (g_menuWin != nullptr) {
    gtk_widget_destroy(g_menuWin);
    g_menuWin = nullptr;
  }
  GtkWidget* win = gtk_window_new(GTK_WINDOW_POPUP);
  gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_POPUP_MENU);
  gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
  gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
  GtkWidget* body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(win), body);

  g_menuWin = win;
  g_menuBody = body;
  g_menuPet = &pet;
  g_signal_connect(win, "destroy",
                   G_CALLBACK(+[](GtkWidget* w, gpointer) {
                     gtk_grab_remove(w);  // 与 ShowMenu 的 gtk_grab_add 配对
                     g_menuWin = nullptr;
                     g_menuBody = nullptr;
                     g_menuPet = nullptr;
                     g_menuXid = None;
                   }),
                   nullptr);

  g_menuAnchorX = rootX;
  g_menuAnchorY = rootY;
  g_menuOpenTick = NowMs();
  BuildMainPage();

  // GTK_WINDOW_POPUP 没有父窗口，GDK 会拒绝定位、也没法作为 popup 映射，
  // KWin 就会把它当普通窗口加标题栏并居中摆放（菜单不跟随鼠标）。
  // 先 realize 拿到 XID，直接设 override_redirect；菜单位置交给 PlaceMenu。
  gtk_widget_realize(win);
  GdkWindow* menuGdk = gtk_widget_get_window(win);
  if (g_debug) {
    Log("menu gdk display={} window={}",
        G_OBJECT_TYPE_NAME(gdk_display_get_default()),
        menuGdk != nullptr ? G_OBJECT_TYPE_NAME(menuGdk) : "null");
  }
  const Window menuXid =
      menuGdk != nullptr ? gdk_x11_window_get_xid(menuGdk) : None;
  g_menuXid = menuXid;
  Window focus = None;
  int revert = 0;

  if (menuXid != None) {
    XSetWindowAttributes attrs{};
    attrs.override_redirect = True;
    XChangeWindowAttributes(g_dpy, menuXid, CWOverrideRedirect, &attrs);
  }
  gtk_widget_show_all(win);
  PlaceMenu();  // 用 Xlib 把菜单放到鼠标右上方
  // map 之后 WM 可能动过焦点，重新取一次快照。
  XGetInputFocus(g_dpy, &focus, &revert);
  g_menuFocusAtOpen = focus;

  gtk_grab_add(win);
  g_signal_connect(win, "button-press-event",
                   G_CALLBACK(+[](GtkWidget* w, GdkEventButton* ev, gpointer)
                                  -> gboolean {
                     GtkAllocation alloc{};
                     gtk_widget_get_allocation(w, &alloc);
                     if (ev->x < 0 || ev->y < 0 || ev->x >= alloc.width ||
                         ev->y >= alloc.height) {
                       gtk_widget_destroy(w);
                       return TRUE;
                     }
                     return FALSE;
                   }),
                   nullptr);
  Log("menu shown at {},{} xid=0x{:x}", g_menuX, g_menuY,
      static_cast<unsigned long>(menuXid));
}

void UpdateMenu(int64_t nowMs) {
  if (nowMs < g_menuNextCheck) return;
  g_menuNextCheck = nowMs + 50;
  CheckMenuDismiss();
}

void CloseMenu() {
  if (g_menuWin == nullptr) return;
  gtk_grab_remove(g_menuWin);
  gtk_widget_destroy(g_menuWin);
  // destroy 回调会把这些全局指针清零；这里兜底，避免菜单残留借用指针。
  g_menuWin = nullptr;
  g_menuBody = nullptr;
  g_menuPet = nullptr;
  g_menuXid = None;
}

}  // namespace linuxui
