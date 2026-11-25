// core::ClipPool 的 Linux 适配层：GL 纹理的创建/上传/销毁通过回调注入。
// 注意：当前 Linux 帧管线仍按 Pet 各自的 DecodeState 运行；本适配层为
// 共享池接入提供完整实现，切换帧管线时只需把 PlayClip 改为 Acquire/Release。
#include <GL/gl.h>

#include <cstdint>
#include <memory>
#include <string>

#include "core/clip_pool.h"
#include "core/log.h"

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif

namespace linuxclips {
namespace {

struct GlClipTexture {
  GLuint tex = 0;
  int w = 0;
  int h = 0;
};

GlClipTexture& GlTex(core::ClipInstance& inst) {
  return *static_cast<GlClipTexture*>(inst.texture);
}

bool CreateTexture(core::ClipInstance& inst, int w, int h, int, int) {
  auto tex = std::make_unique<GlClipTexture>();
  tex->w = w;
  tex->h = h;
  glGenTextures(1, &tex->tex);
  if (tex->tex == 0) return false;
  glBindTexture(GL_TEXTURE_2D, tex->tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_BGRA,
               GL_UNSIGNED_BYTE, nullptr);
  inst.texture = tex.release();
  return true;
}

bool UploadVideo(core::ClipInstance& inst, const uint8_t* bgra, int w, int h) {
  if (inst.texture == nullptr) return false;
  GlClipTexture& tex = GlTex(inst);
  if (w != tex.w || h != tex.h) return false;
  glBindTexture(GL_TEXTURE_2D, tex.tex);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE,
                  bgra);
  return true;
}

void UploadMask(core::ClipInstance&, const char*, int, int) {
  // Linux 端没有 --debug 的红色掩码叠加纹理；掩码由 BuildMask 供 XShape 使用。
}

void DestroyTexture(core::ClipInstance& inst) {
  if (inst.texture == nullptr) return;
  GlClipTexture& tex = GlTex(inst);
  if (tex.tex != 0) glDeleteTextures(1, &tex.tex);
  delete &tex;
  inst.texture = nullptr;
}

void RegisterCallbacks() {
  static const bool registered = [] {
    core::SetClipPoolCallbacks({&CreateTexture, &UploadVideo, &UploadMask,
                                &DestroyTexture, nullptr});
    return true;
  }();
  (void)registered;
}

}  // namespace

core::ClipInstance* Acquire(const std::string& name, const std::string& path,
                            bool loop, bool exclusive, int outW, int outH) {
  RegisterCallbacks();
  return core::AcquireClip(name, path, loop, exclusive, outW, outH);
}

void Release(core::ClipInstance* inst) { core::ReleaseClip(inst); }
void SetVisible(core::ClipInstance* inst, bool visible) {
  core::SetClipVisible(inst, visible);
}
void UpdateAll() { core::UpdateClips(); }
void ResizeAll(int w, int h) { core::ResizeClips(w, h); }
void StopAll() { core::StopAllClips(); }

}  // namespace linuxclips
