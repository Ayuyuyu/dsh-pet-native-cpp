#include "config.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>

#include <nlohmann/json.hpp>

#include "log.h"

namespace core {

std::map<std::string, std::string> g_byName;
std::vector<std::string> g_idleClips;
std::vector<std::string> g_turnClips;
std::vector<std::string> g_dragClips;
std::vector<std::string> g_clickClips;
std::vector<std::string> g_moveNames;
std::vector<std::string> g_randomClips;
std::map<std::string, MoveParams> g_moves;
std::vector<std::pair<std::string, std::vector<std::string>>> g_categories;
std::vector<CategoryMeta> g_categoryMeta;
std::vector<std::pair<std::string, std::vector<std::string>>> g_eventGroups;
SleepClips g_sleepClips;
Weights g_weights;
std::map<std::string, int> g_clickWeights;
int g_initialSizePercent = 15;
bool g_idleAfterAction = false;

namespace {

// 每线程独立引擎，避免 rand() 的全局状态竞争、取模偏差和 RAND_MAX 上限。
int UniformInt(int lo, int hi) {
  thread_local std::mt19937 rng{std::random_device{}()};
  std::uniform_int_distribution<int> dist(lo, hi);
  return dist(rng);
}

// 负权重会让轮盘区间错乱、极端值会让累加溢出，统一夹到 [0, 10000]。
int SanitizeWeight(int w) {
  if (w < 0) return 0;
  if (w > 10000) return 10000;
  return w;
}

// 按名字加权抽下标；累加用 int64 防止多项权重溢出 int。
int PickWeightedIndex(const std::vector<std::string>& items,
                      const std::map<std::string, int>& weights,
                      int defWeight) {
  if (items.empty()) return -1;
  const int def = SanitizeWeight(defWeight);
  int64_t total = 0;
  for (const auto& n : items) {
    const auto it = weights.find(n);
    total += SanitizeWeight(it != weights.end() ? it->second : def);
  }
  if (total <= 0) return 0;
  // 项数有限且每项 ≤10000，total 远小于 INT_MAX，可安全缩回 int。
  int r = UniformInt(0, static_cast<int>(total) - 1);
  for (size_t i = 0; i < items.size(); ++i) {
    const auto it = weights.find(items[i]);
    r -= SanitizeWeight(it != weights.end() ? it->second : def);
    if (r < 0) return static_cast<int>(i);
  }
  return static_cast<int>(items.size()) - 1;
}

}  // namespace

std::string PickFromPool(const std::vector<std::string>& pool) {
  if (pool.empty()) return {};
  return pool[static_cast<size_t>(UniformInt(0, static_cast<int>(pool.size()) - 1))];
}

ActionSpec IdleSpec() {
  ActionSpec s;
  s.kind = ActionKind::Idle;
  s.name = PickFromPool(g_idleClips);
  return s;
}

std::string PickClickAction() {
  const int i = PickWeightedIndex(g_clickClips, g_clickWeights, 10);
  return i < 0 ? std::string() : g_clickClips[static_cast<size_t>(i)];
}

ActionSpec SpecForName(const std::string& name) {
  ActionSpec s;
  s.name = name;
  s.kind = g_moves.count(name) != 0 ? ActionKind::Move : ActionKind::Manual;
  return s;
}

ActionSpec MakeDragSpec() {
  ActionSpec s;
  s.kind = ActionKind::Drag;
  s.name = PickFromPool(g_dragClips);
  s.loop = true;
  return s;
}

ActionSpec PickNextAction(int facing) {
  ActionSpec s;
  int64_t catSum = 0;
  for (const auto& c : g_categoryMeta) catSum += SanitizeWeight(c.weight);
  const int64_t total = static_cast<int64_t>(SanitizeWeight(g_weights.idle)) +
                        SanitizeWeight(g_weights.turn) +
                        SanitizeWeight(g_weights.move) + catSum;
  if (total <= 0) {
    // 配置缺权重时退回旧的等概率随机池，保证向后兼容。
    s.kind = ActionKind::Manual;
    s.name = PickFromPool(g_randomClips);
    if (s.name.empty()) s.name = PickFromPool(g_idleClips);
    return s;
  }

  // 各项上限 10000、项数有限，total 可安全缩回 int。
  int r = UniformInt(0, static_cast<int>(total) - 1);
  if ((r -= SanitizeWeight(g_weights.idle)) < 0) {
    s.kind = ActionKind::Idle;
    s.name = PickFromPool(g_idleClips);
  } else if ((r -= SanitizeWeight(g_weights.turn)) < 0) {
    s.kind = ActionKind::Turn;
    s.name = PickFromPool(g_turnClips);
  } else if ((r -= SanitizeWeight(g_weights.move)) < 0) {
    s.kind = ActionKind::Move;
    s.name = PickFromPool(g_moveNames);
  } else {
    int cat = static_cast<int>(g_categories.size()) - 1;
    for (size_t i = 0; i < g_categories.size(); ++i) {
      const int w = i < g_categoryMeta.size()
                        ? SanitizeWeight(g_categoryMeta[i].weight)
                        : 0;
      if ((r -= w) < 0) {
        cat = static_cast<int>(i);
        break;
      }
    }
    s.kind = ActionKind::Category;
    const bool noMirror =
        cat >= 0 && cat < static_cast<int>(g_categoryMeta.size())
            ? g_categoryMeta[static_cast<size_t>(cat)].noMirror
            : false;
    if (facing == -1 && noMirror) {
      // 文字类动作在镜像状态不播，否则字会反。
      s.kind = ActionKind::Idle;
      s.name = PickFromPool(g_idleClips);
    } else if (cat >= 0 && cat < static_cast<int>(g_categories.size())) {
      s.name = PickFromPool(g_categories[static_cast<size_t>(cat)].second);
    }
  }
  if (s.name.empty()) {
    s.kind = ActionKind::Idle;
    s.name = PickFromPool(g_idleClips);
  }
  return s;
}

void LoadConfig(const char* path) { LoadConfig(std::filesystem::path(path)); }

void LoadConfig(const std::filesystem::path& path) {
  // filesystem::path 重载在 Windows 上调用 CRT 的宽字符打开路径，
  // 中文目录下的 pet-config.json 才能被正确读取。
  std::ifstream in(path);
  if (!in) return;

  nlohmann::json j;
  try {
    in >> j;
  } catch (...) {
    return;
  }

  auto clear = [&]() {
    g_byName.clear();
    g_idleClips.clear();
    g_turnClips.clear();
    g_dragClips.clear();
    g_clickClips.clear();
    g_moveNames.clear();
    g_randomClips.clear();
    g_moves.clear();
    g_categories.clear();
    g_categoryMeta.clear();
    g_eventGroups.clear();
    g_clickWeights.clear();
    g_sleepClips = SleepClips{};
    g_weights = Weights{};
    g_initialSizePercent = 15;
    g_idleAfterAction = false;
  };
  clear();

  // 字段类型错误也属配置错误：捕获后回到空配置，不让异常穿过 main。
  try {
  if (j.contains("sizePercent") && j["sizePercent"].is_number_integer()) {
    g_initialSizePercent = j["sizePercent"].get<int>();
    if (g_initialSizePercent < 10) g_initialSizePercent = 10;
    if (g_initialSizePercent > 40) g_initialSizePercent = 40;
  }
  if (j.contains("idleAfterAction") && j["idleAfterAction"].is_boolean()) {
    g_idleAfterAction = j["idleAfterAction"].get<bool>();
  }

  const auto& by_name = j["byName"];
  if (by_name.is_object()) {
    for (auto it = by_name.begin(); it != by_name.end(); ++it) {
      g_byName[it.key()] = it.value().get<std::string>();
    }
  }

  auto read_pool = [&j](const char* name) {
    std::vector<std::string> out;
    const auto& arr = j["pools"][name];
    if (arr.is_array()) {
      for (const auto& n : arr) out.push_back(n.get<std::string>());
    }
    return out;
  };

  g_idleClips = read_pool("idle");
  g_turnClips = read_pool("turn");
  g_dragClips = read_pool("drag");
  g_clickClips = read_pool("clicks");

  // pools.sleep 是「对象数组」，每项含 start / loop / end 三个候选池；
  // 多组时合并到一起（播的时候随机取）。
  const auto& sleep = j["pools"]["sleep"];
  if (sleep.is_array()) {
    for (const auto& group : sleep) {
      if (!group.is_object()) continue;
      auto read_names = [&group](const char* key,
                                 std::vector<std::string>* out) {
        if (!group.contains(key) || !group[key].is_array()) return;
        for (const auto& n : group[key]) {
          if (n.is_string()) out->push_back(n.get<std::string>());
        }
      };
      read_names("start", &g_sleepClips.start);
      read_names("loop", &g_sleepClips.loop);
      read_names("end", &g_sleepClips.end);
    }
  }

  const auto& w = j["weights"];
  if (w.is_object()) {
    g_weights.idle = SanitizeWeight(w.value("idle", 10));
    g_weights.turn = SanitizeWeight(w.value("turn", 5));
    g_weights.move = SanitizeWeight(w.value("move", 5));
  }
  const auto& cw = j["clickWeights"];
  if (cw.is_object()) {
    for (auto it = cw.begin(); it != cw.end(); ++it) {
      g_clickWeights[it.key()] = SanitizeWeight(it.value().get<int>());
    }
  }

  if (j["pools"]["moves"].is_array()) {
    for (const auto& m : j["pools"]["moves"]) {
      if (!m.contains("name")) continue;
      MoveParams p;
      p.dirX = m.value("dirX", 0);
      p.dirY = m.value("dirY", 0);
      p.minDist = m.value("minDist", 0);
      p.maxDist = m.value("maxDist", 0);
      p.leadSec = m.value("leadSec", 2.0);
      p.tailSec = m.value("tailSec", 2.0);
      p.flip = m.value("flip", false) ? 1 : 0;
      const std::string name = m["name"].get<std::string>();
      if (std::find(g_moveNames.begin(), g_moveNames.end(), name) ==
          g_moveNames.end()) {
        g_moveNames.push_back(name);  // 同名 move 只抽一次，避免权重被重复项放大
      }
      g_moves[name] = p;
    }
  }

  if (j["pools"]["categories"].is_array()) {
    for (const auto& c : j["pools"]["categories"]) {
      if (!c.contains("id") || !c.contains("actions")) continue;
      std::vector<std::string> acts;
      for (const auto& a : c["actions"]) acts.push_back(a.get<std::string>());
      CategoryMeta meta;
      meta.weight = SanitizeWeight(c.value("weight", 0));
      meta.noMirror = c.value("noMirror", false);
      g_categoryMeta.push_back(meta);
      g_categories.emplace_back(c["id"].get<std::string>(), acts);
    }
  }

  // 事件池：仅用于菜单点播，不进自动链。
  const auto& events = j["pools"]["events"];
  if (events.is_object()) {
    const std::map<std::string, std::string> labels = {
        {"balance", "余额档位"},
        {"whisper", "碎碎念"},
        {"workStatus", "工作状态"},
    };
    for (auto it = events.begin(); it != events.end(); ++it) {
      std::vector<std::string> names;
      if (it.value().is_array()) {
        for (const auto& slot : it.value()) {
          if (slot.is_array()) {
            for (const auto& n : slot) names.push_back(n.get<std::string>());
          } else if (slot.is_string()) {
            names.push_back(slot.get<std::string>());
          }
        }
      }
      if (names.empty()) continue;
      const auto label = labels.find(it.key());
      g_eventGroups.emplace_back(
          label != labels.end() ? label->second : it.key(), names);
    }
  }

  // 兼容兜底池：权重配置缺失时仍可等概率抽（不含 events / sleep）。
  std::set<std::string> seen;
  auto add = [&seen](const std::string& n) {
    if (!n.empty() && seen.insert(n).second) g_randomClips.push_back(n);
  };
  for (const auto& n : g_idleClips) add(n);
  for (const auto& n : g_turnClips) add(n);
  for (const auto& n : g_dragClips) add(n);
  for (const auto& n : g_clickClips) add(n);
  for (const auto& n : g_moveNames) add(n);
  for (const auto& cat : g_categories) {
    for (const auto& a : cat.second) add(a);
  }
  } catch (const nlohmann::json::exception& e) {
    // 不再因为一个字段类型错误就清空全部配置：已成功解析的池继续可用，
    // 否则用户会看到一个完全没有任何动画的宠物。记录原因便于修配置。
    log::Log("config field error, keep parsed pools: {}", e.what());
  }
}

}  // namespace core
