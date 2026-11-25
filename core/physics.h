#pragma once

#include <vector>

#include "mask.h"

namespace core {

struct MonitorInfo {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;      // 物理范围（rcMonitor）
  int workBottom = 0;  // 工作区底部（任务栏上沿）
};

// 窗口 left/top 的允许范围（由 body 贴住工作区边界反推）。
struct WindowBounds {
  int minX = 0;
  int maxX = 0;
  int minY = 0;
  int maxY = 0;
};

// 走路：沿 dirX/dirY 走 step 像素；撞边/撞宠物时输出反向退让位置并返回 true。
// body 的 l/r/t/b 都是窗口内偏移；curX/curY 是窗口左上角的屏幕坐标。
// others 是屏幕绝对矩形；撞边与撞宠物分开输出供平台层记录日志。
bool StepWalk(int curX, int curY, int step, int dirX, int dirY,
              const Rect& body, const MonitorInfo& mon,
              const std::vector<Rect>& others, int* outX, int* outY,
              bool* outHitEdge, bool* outHitPet);

// 矩形相交（body 必须同为屏幕绝对坐标）。
bool RectsOverlap(const Rect& a, const Rect& b);

// 飞行：重力 + 边界反弹，更新 x/y/vx/vy。
// 宠物间飞行碰撞由平台层处理：需要访问另一只宠物的速度/状态与碰撞冷却。
// 实现常量沿用既有手感：重力 1400、反弹 0.78、落地摩擦 0.85、
// 落地归零 80、停止条件 y >= maxY && |vy|<30 && |vx|<30；dt 由调用方传入。
void StepFly(double* vx, double* vy, int* x, int* y, const WindowBounds& bounds,
             double dt, bool* outBounced, bool* outStopped);

}  // namespace core
