#pragma once

// 平台无关的资源路径解析：源码里不出现任何机器相关的绝对路径。
// 约定两种布局都能工作：
//   1) 分发：exe 同目录放 assets/；
//   2) 开发：exe 在 build/Release 下，向上若干级能找到 assets/。
#include <string>

namespace core {

// 从 baseDir 开始、最多向上 levels 级，查找 relPath（如 "assets/clips/"）。
// 命中返回该路径；都没命中则返回 baseDir 与 relPath 的拼接结果（调用方自行处理失败）。
std::string ResolveUpwards(const std::string& baseDir, const std::string& relPath,
                           int levels = 4);

// 宽字符版本供 Windows 使用：exe 路径可能含中文，窄字符版本在 ACP 非 UTF-8
// 的系统上会把 UTF-8 字节按 ANSI 解释，导致配置文件永远找不到。
std::wstring ResolveUpwards(const std::wstring& baseDir,
                            const std::wstring& relPath, int levels = 4);

}  // namespace core
