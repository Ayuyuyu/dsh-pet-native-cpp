#include "platform/linux/linux_state.h"

// 全局状态的单一定义点（声明见 linux_state.h）。
Display* g_dpy = nullptr;
int g_screen = 0;
Window g_root = 0;
GLuint g_prog = 0;
XVisualInfo* g_visual = nullptr;
bool g_visualOwned = false;
Colormap g_cmap = 0;
GLXContext g_shareCtx = nullptr;
bool g_hasCompositor = false;
bool g_hasWm = false;
bool g_running = true;
bool g_gtkReady = false;
bool g_dragAny = false;
bool g_menuTest = false;
bool g_sleepTest = false;
bool g_exitTest = false;
bool g_debug = false;
std::string g_startClip;
std::string g_clipsRoot;
double g_scalePercent = 15.0;  // 启动时由 core::g_initialSizePercent 覆盖
int g_nextPetId = 0;

std::vector<std::unique_ptr<Pet>> g_pets;
int64_t g_startTick = 0;
