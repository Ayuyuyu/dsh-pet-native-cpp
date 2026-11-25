#include "core/paths.h"

#include <filesystem>
#include <system_error>

namespace core {

std::string ResolveUpwards(const std::string& baseDir, const std::string& relPath,
                           int levels) {
  std::error_code ec;
  std::filesystem::path dir(baseDir);
  for (int i = 0; i <= levels; ++i) {
    const std::filesystem::path candidate = dir / relPath;
    if (std::filesystem::exists(candidate, ec)) return candidate.string();
    const std::filesystem::path parent = dir.parent_path();
    if (parent == dir || parent.empty()) break;
    dir = parent;
  }
  return (std::filesystem::path(baseDir) / relPath).string();
}

std::wstring ResolveUpwards(const std::wstring& baseDir,
                            const std::wstring& relPath, int levels) {
  std::error_code ec;
  std::filesystem::path dir(baseDir);
  for (int i = 0; i <= levels; ++i) {
    const std::filesystem::path candidate = dir / relPath;
    if (std::filesystem::exists(candidate, ec)) return candidate.wstring();
    const std::filesystem::path parent = dir.parent_path();
    if (parent == dir || parent.empty()) break;
    dir = parent;
  }
  return (std::filesystem::path(baseDir) / relPath).wstring();
}

}  // namespace core
