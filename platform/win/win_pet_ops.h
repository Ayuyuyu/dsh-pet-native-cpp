#pragma once

// Pet 在 Win 平台上的全部行为：窗口几何、每帧管线、动作状态机、每帧 Tick 编排。
// 这些操作都直接读写 Pet 状态，放在同一模块内避免相互调用形成依赖环；
// 与 core 的分工：core 放平台无关算法（解码/掩码/物理），这里负责 Win32 落地。
// 依赖单向：win_main → win_pet_ops → win_clip_pool / render_d3d11 / core。
#include <windows.h>

#include <string>
#include <vector>

#include "core/config.h"
#include "core/physics.h"
#include "platform/win/win_pet.h"

namespace winpetops {

// ===== 窗口几何 =====

// 取窗口所在显示器信息（含工作区底边）。
core::MonitorInfo MonitorFor(HWND hwnd);

// 把其他宠物的可见区域（屏幕坐标）收集进 out。
// 读的是 Pet 里的碰撞缓存，不调用 GetWindowRect。
void CollectOthers(const winpet::Pet& self, std::vector<core::Rect>* out);

// SetWindowPos 之后调用：刷新窗口左上角与 body 的屏幕矩形缓存。
void SyncWindowPos(winpet::Pet& pet);

// 以窗口中心为锚点缩放：重建交换链与 RTV；动画纹理由 winclips::ResizeAll 同步重建。
// resizePool=false 供 ApplySizeAll 使用：整池只在循环外重建一次。
void ApplySize(winpet::Pet& pet, int percent, bool resizePool = true);

// 对所有宠物应用同一尺寸，并更新全局尺寸百分比。
void ApplySizeAll(int percent);

// ===== 每帧管线 =====

// 宠物侧从共享实例同步可见 body：掩码只有一份未镜像的，
// flip 时把 X 镜像到窗口坐标；同时刷新碰撞缓存与 body 缩放。
void SyncBodyFromClip(winpet::Pet& pet);

// 客户端坐标是否落在宠物可见像素上（WM_NCHITTEST 用）。
// 实例未就绪时返回 true，保证首帧前窗口仍可点。
bool HitTest(const winpet::Pet& pet, POINT client);

// 用当前帧掩码重建窗口区域：透明像素必须从窗口「消失」，
// 否则浏览器/记事本等跨进程窗口收不到该处的鼠标消息。
// 用 ExtCreateRegion 一次构建 + 垂直合并矩形，避免逐行 CreateRectRgn 的开销。
void UpdateWindowRegion(winpet::Pet& pet);

// 每帧推进 Q 弹曲线（纯时间函数，不绘制）。
void UpdateSquash(winpet::Pet& pet);

// 绘制并 Present；会清掉 needPresent。
void Present(winpet::Pet& pet);

// 本 Tick 是否拿到了新画面：读实例帧戳、同步 body 与视频时钟。
bool ConsumeFrame(winpet::Pet& pet);

// ===== 动作状态机 =====

// 打开并播放一段动作（底层入口，通常由 PlayAction 调用）。
bool PlayClip(winpet::Pet& pet, const std::string& name);

// 切段前恢复 flip 镜像，按 kind 设置新朝向。
bool PlayAction(winpet::Pet& pet, const core::ActionSpec& spec);

// loop 循环、dragMode 不切段、turn 播完转身。
void OnClipEnded(winpet::Pet& pet, DWORD nowMs, bool stalled);

// 右键菜单「睡觉/起来」：未睡时播开始段（播完自动进循环段），
// 已睡时播结束段（播完回到正常随机）。
void ToggleSleep(winpet::Pet& pet);

// ===== 每帧 Tick =====

// 单 16ms Tick 编排：取帧 → 物理 → 切段判定 → 条件 Present。
// 顺序有依赖：取帧必须先于帧号快照，Present 必须最后。
void Tick(winpet::Pet& pet);

// 全局唯一 16ms 节拍：先刷新所有共享实例的帧，再逐只跑 Tick。
// 只应由主程序的单一 WM_TIMER 调用。
void TickAll();

}  // namespace winpetops
