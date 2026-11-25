#include "physics.h"

#include <cmath>
#include <cstdlib>  // std::abs(int)

namespace core {
namespace {

bool Overlaps(const Rect& a, const Rect& b) {
  return a.l < b.r && a.r > b.l && a.t < b.b && a.b > b.t;
}

// 只要下一帧相交且在向对方靠近就算撞击。
// 不能用「从不相交进入相交」：宠物身体框会随动画逐帧变化，贴身后继续靠近
// 也会错过 Entered 边界，结果就是两只宠物互相穿过。
// 水平/竖直任一方向中心距缩短或发生穿越都算靠近；两个方向都在远离才放过。
bool HitPet(const Rect& next, const Rect& cur,
            const std::vector<Rect>& others) {
  for (const Rect& o : others) {
    if (!Overlaps(next, o)) continue;
    if (!Overlaps(cur, o)) return true;
    const int oCx = (o.l + o.r) / 2;
    const int oCy = (o.t + o.b) / 2;
    const int curDx = (cur.l + cur.r) / 2 - oCx;
    const int nextDx = (next.l + next.r) / 2 - oCx;
    const int curDy = (cur.t + cur.b) / 2 - oCy;
    const int nextDy = (next.t + next.b) / 2 - oCy;
    if (std::abs(nextDx) < std::abs(curDx) || curDx * nextDx < 0) return true;
    if (std::abs(nextDy) < std::abs(curDy) || curDy * nextDy < 0) return true;
  }
  return false;
}

}  // namespace

bool RectsOverlap(const Rect& a, const Rect& b) {
  return Overlaps(a, b);
}

namespace {

}  // namespace

bool StepWalk(int curX, int curY, int step, int dirX, int dirY,
              const Rect& body, const MonitorInfo& mon,
              const std::vector<Rect>& others, int* outX, int* outY,
              bool* outHitEdge, bool* outHitPet) {
  *outHitEdge = false;
  *outHitPet = false;
  const int nx = curX + dirX * step;
  const int ny = curY + dirY * step;
  const Rect cur{curX + body.l, curY + body.t, curX + body.r, curY + body.b};
  const Rect next{nx + body.l, ny + body.t, nx + body.r, ny + body.b};

  const bool hit_edge =
      (nx + body.l < mon.left || nx + body.r > mon.right ||
       ny + body.t < mon.top || ny + body.b > mon.workBottom);
  const bool hit_pet = HitPet(next, cur, others);
  if (!hit_edge && !hit_pet) {
    *outX = nx;
    *outY = ny;
    return false;
  }

  *outHitEdge = hit_edge;
  *outHitPet = hit_pet;
  // 回弹一步：沿原方向反向退让，并钳在四个工作区边界内。
  int bx = curX - dirX * step;
  int by = curY - dirY * step;
  if (bx + body.l < mon.left) bx = mon.left - body.l;
  if (bx + body.r > mon.right) bx = mon.right - body.r;
  if (by + body.t < mon.top) by = mon.top - body.t;
  if (by + body.b > mon.workBottom) by = mon.workBottom - body.b;
  *outX = bx;
  *outY = by;
  return true;
}

// 宠物间碰撞在平台层处理。
void StepFly(double* vx, double* vy, int* x, int* y, const WindowBounds& b,
             double dt, bool* outBounced, bool* outStopped) {
  *vy += 1400.0 * dt;
  int nx = *x + static_cast<int>(*vx * dt);
  int ny = *y + static_cast<int>(*vy * dt);
  bool bounced = false;

  if (nx < b.minX) {
    nx = b.minX;
    *vx = -*vx * 0.78;
    bounced = true;
  }
  if (nx > b.maxX) {
    nx = b.maxX;
    *vx = -*vx * 0.78;
    bounced = true;
  }
  if (ny < b.minY) {
    ny = b.minY;
    *vy = -*vy * 0.78;
    bounced = true;
  }
  if (ny > b.maxY) {
    ny = b.maxY;
    *vy = -*vy * 0.78;
    *vx *= 0.85;
    if (std::fabs(*vy) < 80.0) *vy = 0.0;
    bounced = true;
  }

  *x = nx;
  *y = ny;
  *outBounced = bounced;
  // 贴住下边界 + 速度都小。
  *outStopped = (ny >= b.maxY && std::fabs(*vy) < 30.0 && std::fabs(*vx) < 30.0);
}

}  // namespace core
