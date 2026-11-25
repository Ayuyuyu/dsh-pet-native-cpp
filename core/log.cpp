#include "core/log.h"

#include <atomic>

namespace core {
namespace log {

namespace {

std::atomic<bool> g_enabled{false};
// 函数指针是 trivially copyable，std::atomic<Sink> 可直接使用；
// 转成 void* 再转回来属于 conditionally-supported，没必要冒险。
std::atomic<Sink> g_sink{nullptr};

}  // namespace

void Init(bool enabled, Sink sink) {
  g_sink.store(sink);
  g_enabled.store(enabled && sink != nullptr);
}

bool Enabled() {
  return g_enabled.load();
}

void Write(std::string_view message) {
  if (!g_enabled.load()) return;
  const Sink sink = g_sink.load();
  if (sink != nullptr) sink(message);
}

}  // namespace log
}  // namespace core
