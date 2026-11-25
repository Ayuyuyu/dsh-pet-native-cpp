#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

#include "core/clip_pool.h"
#include "platform/win/render_d3d11.h"

// Win 适配层：实例生命周期、引用计数、遮挡暂停与事务化缩放都在 core::ClipPool，
// 这里只注入 D3D11 纹理的创建/上传/销毁回调，并保留原有的调用名称。
namespace winclips {

using ClipInstance = core::ClipInstance;

// 平台纹理句柄存在 inst.texture 中，用 helper 取出，避免各处裸 void* 转换。
inline WinClipTexture& Tex(ClipInstance& inst) {
  return *static_cast<WinClipTexture*>(inst.texture);
}
inline const WinClipTexture& Tex(const ClipInstance& inst) {
  return *static_cast<const WinClipTexture*>(inst.texture);
}

ClipInstance* Acquire(const std::string& name, const std::string& path,
                      bool loop, bool exclusive, int outW, int outH);
void Release(ClipInstance* inst);
void SetVisible(ClipInstance* inst, bool visible);
void UpdateAll();
void ResizeAll(int outW, int outH);
void StopAll();
int ActiveCount();

}  // namespace winclips
