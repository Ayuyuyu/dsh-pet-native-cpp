#pragma once

// Win 平台层通用工具：编码转换、路径、数学、随机、日志、音效、调试导出与资源 RAII。
// 不依赖 Pet / 窗口状态机，便于被主程序、UI 等模块共用。
#ifndef NOMINMAX
#define NOMINMAX  // 避免 windows.h 的 min/max 宏破坏 std::min/std::max
#endif
#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace winutil {

// ---- Win32 资源 RAII ----
class ScopedMenu {
 public:
  explicit ScopedMenu(HMENU h) : h_(h) {}
  ~ScopedMenu() {
    if (h_ != nullptr) DestroyMenu(h_);
  }
  ScopedMenu(const ScopedMenu&) = delete;
  ScopedMenu& operator=(const ScopedMenu&) = delete;
  HMENU get() const { return h_; }
  HMENU release() {
    HMENU h = h_;
    h_ = nullptr;
    return h;
  }

 private:
  HMENU h_;
};

class ScopedWindow {
 public:
  explicit ScopedWindow(HWND h) : h_(h) {}
  ~ScopedWindow() {
    if (h_ != nullptr) DestroyWindow(h_);
  }
  ScopedWindow(const ScopedWindow&) = delete;
  ScopedWindow& operator=(const ScopedWindow&) = delete;
  HWND get() const { return h_; }
  HWND release() {
    HWND h = h_;
    h_ = nullptr;
    return h;
  }

 private:
  HWND h_;
};

// ---- 字符串与路径 ----
std::wstring ToWide(const std::string& s);
std::string ToUtf8(const std::wstring& w);
// 进程内不变，静态缓存后返回 const 引用，避免重复系统调用与拷贝。
const std::wstring& ExeDir();
// 路径全程保留宽字符；只有需要交给 FFmpeg 时才转 UTF-8 窄串。
const std::wstring& ConfigPath();
const std::wstring& ClipsRoot();
std::string ConfigPathUtf8();
std::string ClipsRootUtf8();

// ---- 调试辅助 ----
// 数学与随机工具已迁移到 core/util.h，两端共用同一份实现。
void PlayClickSound();

// 每 30 帧采样导出一次掩码 BMP（白=可点击区域），用于排查掩码/命中区域问题。
// 编译期开关，默认关闭：正式构建里整段代码不参与编译，完全不产生磁盘 IO。
// 打开方式：cmake -B build -DPET_DEBUG_MASK_BMP=ON
#ifndef PET_DEBUG_MASK_BMP
#define PET_DEBUG_MASK_BMP 0
#endif

#if PET_DEBUG_MASK_BMP
// 把当前帧掩码导出成 BMP，frame 用于文件名编号以便逐帧对齐。
void SaveMaskBmp(int w, int h, const std::vector<char>& mask, int frame);
#endif

}  // namespace winutil
