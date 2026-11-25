#pragma once

// 平台无关日志门面：C++20 std::format 类型安全格式化，输出目标由平台层注入一次。
// 用法：启动时 core::log::Init(enabled, &Sink)，之后任意平台/核心模块
// 直接 core::log::Log("pet{} done={}", index, done)。
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace core {
namespace log {

// 输出一行（不含时间戳）。平台层实现（命名规则统一为 pet-<平台>.log）：
//   Win   → exe 目录下 pet-win.log，前缀 GetTickCount；
//   Linux → exe 目录下 pet-linux.log，前缀 NowMs。
using Sink = void (*)(std::string_view line);

// 初始化：enabled=false 或 sink=nullptr 时所有 Log 立即返回，不产生 IO。
void Init(bool enabled, Sink sink);
bool Enabled();

// 直接写一行（不经格式化），供崩溃处理器等特殊情况使用。
void Write(std::string_view message);

// 格式串在编译期校验（std::format_string），写错会直接编译失败。
template <class... Args>
void Log(std::format_string<Args...> fmt, Args&&... args) {
  if (!Enabled()) return;
  try {
    Write(std::format(fmt, std::forward<Args>(args)...));
  } catch (const std::format_error& e) {
    Write(std::string("[log format error] ") + e.what());
  }
}

}  // namespace log
}  // namespace core
