#include "platform/linux/linux_util.h"

#include <glib.h>
#include <unistd.h>  // readlink：定位 exe 目录

#include <chrono>
#include <fstream>
#include <string>

namespace linuxutil {

void XFreeDeleter::operator()(unsigned char* p) const {
  if (p != nullptr) XFree(p);
}

void GListFreer::operator()(GList* p) const {
  if (p != nullptr) g_list_free(p);
}

int64_t NowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
      .count();
}

const std::string& ExeDir() {
  static const std::string dir = [] {
    std::vector<char> buf(4096, '\0');
    for (;;) {
      const ssize_t n = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
      if (n <= 0) return std::string("./");
      // n == size-1 表示可能被截断，扩容后重试，否则路径末尾会被截掉。
      if (static_cast<size_t>(n) < buf.size() - 1) {
        const std::string path(buf.data(), static_cast<size_t>(n));
        const size_t pos = path.find_last_of('/');
        return pos == std::string::npos ? std::string("./")
                                        : path.substr(0, pos + 1);
      }
      buf.resize(buf.size() * 2);
    }
  }();
  return dir;
}

void LogSink(std::string_view line) {
  // 静态复用 ofstream，避免每条日志都重算路径并开关文件。
  static std::ofstream out(ExeDir() + "pet-linux.log", std::ios::app);
  if (!out) return;
  out << '[' << NowMs() << "] " << line << '\n' << std::flush;
}

}  // namespace linuxutil
