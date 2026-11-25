// 动画实例池的 Win 适配层：core::ClipPool 负责解码/引用计数/遮挡暂停，
// 这里只把 D3D11 纹理的生命周期与上传接到回调上。
#include "win_clip_pool.h"

#include <memory>

#include "core/log.h"
#include "core/util.h"
#include "platform/win/win_util.h"

namespace winclips {
namespace {

bool CreateTexture(core::ClipInstance& inst, int w, int h, int maskW,
                   int maskH) {
  auto tex = std::make_unique<WinClipTexture>();
  if (!RenderCreateClipTextures(*tex, w, h, maskW, maskH)) return false;
  inst.texture = tex.release();
  return true;
}

bool UploadVideo(core::ClipInstance& inst, const uint8_t* bgra, int w, int h) {
  if (inst.texture == nullptr) return false;
  return RenderUploadVideo(Tex(inst), bgra, w, h);
}

void UploadMask(core::ClipInstance& inst, const char* mask, int mw, int mh) {
  if (inst.texture == nullptr) return;
  RenderUploadMask(Tex(inst), mask, mw, mh);
}

void DestroyTexture(core::ClipInstance& inst) {
  delete static_cast<WinClipTexture*>(inst.texture);
  inst.texture = nullptr;
}

int MaskWidthFor(int windowW) { return core::MaskSizeFor(windowW); }

void RegisterCallbacks() {
  static const bool registered = [] {
    core::SetClipPoolCallbacks({&CreateTexture, &UploadVideo, &UploadMask,
                                &DestroyTexture, &MaskWidthFor});
    return true;
  }();
  (void)registered;
}

}  // namespace

ClipInstance* Acquire(const std::string& name, const std::string& path,
                      bool loop, bool exclusive, int outW, int outH) {
  RegisterCallbacks();
  return core::AcquireClip(name, path, loop, exclusive, outW, outH);
}

void Release(ClipInstance* inst) { core::ReleaseClip(inst); }

void SetVisible(ClipInstance* inst, bool visible) {
  core::SetClipVisible(inst, visible);
}

void UpdateAll() { core::UpdateClips(); }

void ResizeAll(int outW, int outH) { core::ResizeClips(outW, outH); }

void StopAll() { core::StopAllClips(); }

int ActiveCount() { return core::ActiveClipCount(); }

}  // namespace winclips
