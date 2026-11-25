#include "mask.h"

#include <algorithm>

namespace core {

void BuildMask(const uint8_t* bgra, int videoW, int videoH, int windowW,
               int windowH, int maskW, int maskH, bool flip, int threshold,
               int expand, MaskResult* out) {
  if (out == nullptr) return;
  // 打开失败或素材尺寸异常时，调用方可能还没有有效帧；此处统一兜底，
  // 避免空指针解引用和 maskW/maskH 为 0 时的整数除零。
  if (bgra == nullptr || videoW <= 0 || videoH <= 0 || maskW <= 0 ||
      maskH <= 0) {
    out->mask.clear();
    out->rects.clear();
    out->maskW = 0;
    out->maskH = 0;
    out->bodyL = 0;
    out->bodyT = 0;
    out->bodyR = 0;
    out->bodyB = 0;
    return;
  }
  const size_t stride = static_cast<size_t>(videoW) * 4;
  const size_t count = static_cast<size_t>(maskW) * maskH;
  // 用 resize 而非 assign：容量复用，每帧不再重新分配几十 KB；
  // 下面逐像素写满整个缓冲，无需清零。rects 同理只 clear 保留容量。
  out->mask.resize(count);
  out->rects.clear();
  out->maskW = maskW;
  out->maskH = maskH;

  int bl = maskW;
  int bt = maskH;
  int br = 0;
  int bb = 0;

  // 单趟完成三件事：生成掩码行、更新包围盒、产出该行的区域矩形。
  for (int y = 0; y < maskH; ++y) {
    const int sy = y * videoH / maskH;
    const uint8_t* row = bgra + static_cast<size_t>(sy) * stride;
    char* dst = out->mask.data() + static_cast<size_t>(y) * maskW;
    for (int x = 0; x < maskW; ++x) {
      const int sx = x * videoW / maskW;
      dst[x] = row[static_cast<size_t>(sx) * 4 + 3] > threshold ? 1 : 0;
    }
    // 镜像动作：掩码和画面一起翻转，否则命中区域会错边。
    if (flip) std::reverse(dst, dst + maskW);

    int x = 0;
    while (x < maskW) {
      if (!dst[x]) {
        ++x;
        continue;
      }
      const int start = x;
      while (x < maskW && dst[x]) ++x;
      if (start < bl) bl = start;
      if (y < bt) bt = y;
      if (x > br) br = x;
      if (y + 1 > bb) bb = y + 1;

      int l = start * windowW / maskW - expand;
      int r = x * windowW / maskW + expand + 1;
      int t = y * windowH / maskH - expand;
      int b = (y + 1) * windowH / maskH + expand + 1;
      if (l < 0) l = 0;
      if (t < 0) t = 0;
      if (r > windowW) r = windowW;
      if (b > windowH) b = windowH;
      out->rects.push_back(Rect{l, t, r, b});
    }
  }

  if (br > bl && bb > bt) {
    out->bodyL = bl * windowW / maskW;
    out->bodyT = bt * windowH / maskH;
    out->bodyR = br * windowW / maskW;
    out->bodyB = bb * windowH / maskH;
  } else {
    out->bodyL = 0;
    out->bodyT = 0;
    out->bodyR = windowW;
    out->bodyB = windowH;
  }
}

}  // namespace core
