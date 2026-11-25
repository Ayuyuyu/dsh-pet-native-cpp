#pragma once

#include <cstdint>
#include <vector>

namespace core {

struct Rect {
  int l = 0;
  int t = 0;
  int r = 0;
  int b = 0;
};

struct MaskResult {
  std::vector<char> mask;   // 0/1 掩码：命中测试、窗口/输入区域与调试纹理共用
  std::vector<Rect> rects;  // 窗口区域矩形（已外扩）
  int maskW = 0;
  int maskH = 0;
  int bodyL = 0;
  int bodyT = 0;
  int bodyR = 0;
  int bodyB = 0;
};

// 从 BGRA 帧计算掩码、可见区域包围盒、窗口区域矩形。
// 纯 CPU，不依赖任何平台 API；flip=true 时掩码水平翻转。
void BuildMask(const uint8_t* bgra, int videoW, int videoH, int windowW,
               int windowH, int maskW, int maskH, bool flip, int threshold,
               int expand, MaskResult* out);

}  // namespace core
