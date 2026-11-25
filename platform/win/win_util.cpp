#include "platform/win/win_util.h"

#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <random>
#include <string>

#include "core/paths.h"

namespace winutil {

namespace {

// 小端序列化：避免对 vector<uint8_t> 做 reinterpret_cast（对齐与严格别名都是 UB）。
void WriteLe32(std::vector<uint8_t>& buf, size_t off, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    buf[off + static_cast<size_t>(i)] =
        static_cast<uint8_t>(value >> (8 * i));
  }
}

void WriteLe16(std::vector<uint8_t>& buf, size_t off, uint16_t value) {
  for (int i = 0; i < 2; ++i) {
    buf[off + static_cast<size_t>(i)] =
        static_cast<uint8_t>(value >> (8 * i));
  }
}

}  // namespace

std::wstring ToWide(const std::string& s) {
  if (s.empty()) return {};
  const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (len <= 1) return {};
  // 按含结尾 NUL 的长度分配并写入，再 resize 去掉 NUL：
  // 直接写 len 个 wchar_t 到 size=len-1 的 string.data() 是越界写（UB）。
  std::wstring w(static_cast<size_t>(len), L'\0');
  const int written =
      MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
  if (written <= 0) return {};
  w.resize(static_cast<size_t>(written - 1));
  return w;
}

std::string ToUtf8(const std::wstring& w) {
  if (w.empty()) return {};
  const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0,
                                      nullptr, nullptr);
  if (len <= 1) return {};
  std::string s(static_cast<size_t>(len), '\0');
  const int written = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(),
                                          len, nullptr, nullptr);
  if (written <= 0) return {};
  s.resize(static_cast<size_t>(written - 1));
  return s;
}

const std::wstring& ExeDir() {
  static const std::wstring dir = [] {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
      const DWORD len = GetModuleFileNameW(nullptr, path.data(),
                                           static_cast<DWORD>(path.size()));
      if (len == 0) return std::wstring();
      if (len < path.size()) {
        path.resize(len);
        break;
      }
      path.resize(path.size() * 2);  // 缓冲区不足被截断，扩容重试
    }
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring()
                                     : path.substr(0, pos + 1);
  }();
  return dir;
}

// 分发时配置/素材在 exe 同目录；开发时从 exe 目录向上若干级找 assets/。
// 探测同目录文件必须用宽字符 API：exe 路径可能含中文，UTF-8 字节串交给
// GetFileAttributesA 会按 ANSI 代码页解释而误判为不存在。
const std::wstring& ConfigPath() {
  static const std::wstring path = [] {
    const std::wstring flat = ExeDir() + L"pet-config.json";
    if (GetFileAttributesW(flat.c_str()) != INVALID_FILE_ATTRIBUTES) {
      return flat;
    }
    return core::ResolveUpwards(ExeDir(), L"assets/pet-config.json");
  }();
  return path;
}

std::string ConfigPathUtf8() { return ToUtf8(ConfigPath()); }

const std::wstring& ClipsRoot() {
  static const std::wstring root = [] {
    const std::wstring flat = ExeDir() + L"assets/clips/";
    if (GetFileAttributesW(flat.c_str()) != INVALID_FILE_ATTRIBUTES) {
      return flat;
    }
    return core::ResolveUpwards(ExeDir(), L"assets/clips/");
  }();
  return root;
}

std::string ClipsRootUtf8() { return ToUtf8(ClipsRoot()); }

// 点击回应音效（wav），走 PlaySound 异步播放，不阻塞渲染。
void PlayClickSound() {
  // 100ms 节流，避免连点时音效叠在一起。
  static DWORD lastTick = 0;
  const DWORD now = GetTickCount();
  if (now - lastTick < 100) return;
  lastTick = now;
  // 首次点击解析一次并缓存：exe 同目录或向上若干级的 assets/sound。
  static const std::wstring path =
      core::ResolveUpwards(ExeDir(), L"assets/sound/click-duck.wav");
  PlaySoundW(path.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
}

// 把当前帧掩码导出成 BMP（白=可点击），用于人工核对区域比画面大多少。
#if PET_DEBUG_MASK_BMP
void SaveMaskBmp(int w, int h, const std::vector<char>& mask, int frame) {
  // w/h 来自掩码档位（当前 ≤512×288），row_size*h 远小于 INT_MAX；
  // 这里仍校验入参与掩码长度，避免将来换更大尺寸时越界读。
  if (w <= 0 || h <= 0) return;
  if (static_cast<size_t>(w) * static_cast<size_t>(h) > mask.size()) return;
  const int row_size = (w * 3 + 3) & ~3;
  const int data_size = row_size * h;
  const int file_size = 54 + data_size;
  std::vector<uint8_t> bmp(static_cast<size_t>(file_size), 0);
  bmp[0] = 'B';
  bmp[1] = 'M';
  WriteLe32(bmp, 2, static_cast<uint32_t>(file_size));
  WriteLe32(bmp, 10, 54);
  WriteLe32(bmp, 14, 40);
  WriteLe32(bmp, 18, static_cast<uint32_t>(w));
  WriteLe32(bmp, 22, static_cast<uint32_t>(-h));
  WriteLe16(bmp, 26, 1);
  WriteLe16(bmp, 28, 24);
  WriteLe32(bmp, 34, static_cast<uint32_t>(data_size));
  for (int y = 0; y < h; ++y) {
    uint8_t* row = bmp.data() + 54 + y * row_size;
    for (int x = 0; x < w; ++x) {
      const uint8_t v = mask[static_cast<size_t>(y) * w + x] ? 255 : 0;
      row[x * 3 + 0] = v;
      row[x * 3 + 1] = v;
      row[x * 3 + 2] = v;
    }
  }

  // 文件名带视频帧号，便于把掩码序列与画面逐帧对齐排查。
  std::string number = std::to_string(frame);
  if (number.size() < 4) number.insert(0, 4 - number.size(), '0');
  const std::string path = ToUtf8(ExeDir()) + "dbg_f" + number + ".bmp";

  std::ofstream out(path, std::ios::binary);
  if (!out) return;
  out.write(reinterpret_cast<const char*>(bmp.data()),
            static_cast<std::streamsize>(bmp.size()));
}
#endif  // PET_DEBUG_MASK_BMP

}  // namespace winutil
