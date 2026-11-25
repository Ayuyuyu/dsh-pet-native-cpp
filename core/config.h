#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace core {

struct MoveParams {
  int dirX = 0;
  int dirY = 0;
  int minDist = 0;
  int maxDist = 0;
  double leadSec = 2.0;
  double tailSec = 2.0;
  int flip = 0;
};

// pet-config.json 顶层权重：idle / turn / move；分类权重另存 g_categoryMeta。
struct Weights {
  int idle = 10;
  int turn = 5;
  int move = 5;
};

struct CategoryMeta {
  int weight = 0;
  bool noMirror = false;
};

enum class ActionKind {
  Idle,
  Turn,
  Move,
  Category,
  Manual,
  Click,
  Drag,
  Sleep,  // 睡觉开始/循环/起来三段，不参与随机
};

struct ActionSpec {
  ActionKind kind = ActionKind::Idle;
  std::string name;
  bool loop = false;
};

extern std::map<std::string, std::string> g_byName;
extern std::vector<std::string> g_idleClips;
extern std::vector<std::string> g_turnClips;
extern std::vector<std::string> g_dragClips;
extern std::vector<std::string> g_clickClips;
extern std::vector<std::string> g_moveNames;
extern std::vector<std::string> g_randomClips;
extern std::map<std::string, MoveParams> g_moves;
extern std::vector<std::pair<std::string, std::vector<std::string>>> g_categories;
extern std::vector<CategoryMeta> g_categoryMeta;
// 可点播的事件组：余额档位 / 碎碎念 / 工作状态。
extern std::vector<std::pair<std::string, std::vector<std::string>>> g_eventGroups;

// 睡觉动作组（pet-config.json 的 pools.sleep[0]）：开始 → 循环 → 起来。
// 每段是候选池，播的时候随机取一个；循环段以 loop 方式循环播放。
struct SleepClips {
  std::vector<std::string> start;
  std::vector<std::string> loop;
  std::vector<std::string> end;
};
extern SleepClips g_sleepClips;
extern Weights g_weights;
extern std::map<std::string, int> g_clickWeights;
// 顶层初始显示比例（百分比），配置缺省为 15，合法范围 10~40。
extern int g_initialSizePercent;

// 动作播完后是否强制回待机（pet-config.json 的 idleAfterAction，缺省 false）。
// 开启后节奏是「动作 → 待机 → 随机下一段 → 待机…」；
// 关闭时维持原来的连续加权随机
extern bool g_idleAfterAction;

// 读取 pet-config.json，填充以上全局池。
// path 重载让 Windows 传宽字符路径，避免中文安装目录下 ANSI 解释失败。
void LoadConfig(const char* path);
void LoadConfig(const std::filesystem::path& path);

// 加权轮盘：按 idle/turn/move/分类权重抽下一段；facing=-1 时 noMirror 分类回退 idle。
ActionSpec PickNextAction(int facing);
// 点击回应按 clickWeights 加权（缺省 10）。
std::string PickClickAction();
// 池内等概率。
std::string PickFromPool(const std::vector<std::string>& pool);
// 待机动作（idle 池随机一个），供「动作后回待机」使用。
ActionSpec IdleSpec();
// 菜单点播：名字属于 moves 池则按 Move 处理，否则 Manual。
ActionSpec SpecForName(const std::string& name);
// 拖拽悬空反馈：drag 池 + loop。
ActionSpec MakeDragSpec();

}  // namespace core
