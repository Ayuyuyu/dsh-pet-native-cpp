#include "util.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace core {
namespace {

std::mt19937& Rng() {
  thread_local std::mt19937 rng{std::random_device{}()};
  return rng;
}

}  // namespace

int RandomInt(int lo, int hi) {
  if (hi <= lo) return lo;
  std::uniform_int_distribution<int> dist(lo, hi);
  return dist(Rng());
}

std::string PickRandom(const std::vector<std::string>& items) {
  if (items.empty()) return {};
  return items[static_cast<size_t>(RandomInt(
      0, static_cast<int>(items.size()) - 1))];
}

float SquashCurve(float u) {
  constexpr float kSquash = 0.55f;
  if (u < 0.45f) {
    const float p = u / 0.45f;
    return 1.0f - (1.0f - kSquash) * p * p;
  }
  const float p = (u - 0.45f) / 0.55f;
  constexpr float c1 = 1.70158f;
  constexpr float c3 = c1 + 1.0f;
  const float f =
      1.0f + c3 * std::pow(p - 1.0f, 3) + c1 * std::pow(p - 1.0f, 2);
  return std::min(1.12f, kSquash + (1.0f - kSquash) * std::max(f, 0.0f));
}

int MaskSizeFor(int windowW) { return windowW >= 480 ? 512 : 384; }

}  // namespace core
