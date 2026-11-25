#pragma once

#include <string>
#include <vector>

namespace core {

// 平台无关的数学与随机工具。随机引擎按线程独立，避免全局 rand() 竞争
// 与取模偏差；两端不再各自复制一份实现。

// [lo, hi] 闭区间；hi <= lo 时返回 lo。
int RandomInt(int lo, int hi);

// 池内等概率抽取；空池返回空串。
std::string PickRandom(const std::vector<std::string>& items);

// Q 弹曲线：u 为 0~1 的归一化时间，返回值是纵向缩放比例。
float SquashCurve(float u);

// 掩码档位按窗口宽度分两档，避免小窗口采样过密、大窗口采样过疏。
int MaskSizeFor(int windowW);

}  // namespace core
