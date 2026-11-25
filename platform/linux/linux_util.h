#pragma once

// Linux 平台层通用工具：时间、路径、日志落盘与资源 RAII。
// 数学/随机工具已迁移到 core/util.h，不依赖 Pet / 窗口状态机。
#include <X11/Xlib.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct _GList;
using GList = _GList;  // glib 链表节点，避免整个 util 头依赖 gtk/glib

namespace linuxutil {

int64_t NowMs();

// exe 所在目录（带结尾 /）：进程内不变，静态缓存后返回 const 引用。
const std::string& ExeDir();

// 日志落盘：公共 core::log 只负责格式化，时间戳与目标由平台注入。
void LogSink(std::string_view line);

// X11 / GLib 返回的资源用 RAII 管理，避免手动 XFree / g_list_free 配对。
struct XFreeDeleter {
  void operator()(unsigned char* p) const;
};
using XPropertyPtr = std::unique_ptr<unsigned char, XFreeDeleter>;

struct GListFreer {
  void operator()(GList* p) const;
};
using GListPtr = std::unique_ptr<GList, GListFreer>;

}  // namespace linuxutil
