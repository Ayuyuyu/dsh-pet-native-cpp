#include "clip_pool.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "log.h"
#include "util.h"

namespace core {
namespace {

constexpr int kMaxInstancesPerClip = 4;

ClipPoolCallbacks g_callbacks{};

std::vector<std::unique_ptr<ClipInstance>> g_instances;

int MaskWidth(int outW) {
  const int w = g_callbacks.maskWidthFor != nullptr ? g_callbacks.maskWidthFor(outW)
                                                    : MaskSizeFor(outW);
  return std::min(w, outW);
}

void UploadInstance(ClipInstance& inst) {
  if (!g_callbacks.uploadVideo ||
      !g_callbacks.uploadVideo(inst, inst.dec.bgra.data(), inst.outW,
                               inst.outH)) {
    return;
  }
  // alpha 阈值 4：抗锯齿边缘也算角色的一部分；空区域依旧完全穿透。
  // 掩码档位由 outW/outH 等比推导，core 不依赖平台纹理结构。
  const int mw = MaskWidth(inst.outW);
  const int mh = mw * inst.outH / inst.outW;
  BuildMask(inst.dec.bgra.data(), inst.outW, inst.outH, inst.outW, inst.outH,
            mw, mh, false, 4, 2, &inst.mask);
  if (g_callbacks.uploadMask) {
    g_callbacks.uploadMask(inst, inst.mask.mask.data(), mw, mh);
  }
}

void EraseInstance(ClipInstance* inst) {
  core::Stop(inst->dec);
  if (g_callbacks.destroyTexture) g_callbacks.destroyTexture(*inst);
  g_instances.erase(
      std::remove_if(g_instances.begin(), g_instances.end(),
                     [inst](const std::unique_ptr<ClipInstance>& p) {
                       return p.get() == inst;
                     }),
      g_instances.end());
}

}  // namespace

void SetClipPoolCallbacks(const ClipPoolCallbacks& callbacks) {
  g_callbacks = callbacks;
}

ClipInstance* AcquireClip(const std::string& name, const std::string& path,
                          bool loop, bool exclusive, int outW, int outH) {
  if (outW <= 0 || outH <= 0) return nullptr;

  if (!exclusive) {
    ClipInstance* best = nullptr;
    int sameClip = 0;
    for (const auto& p : g_instances) {
      ClipInstance& inst = *p;
      if (inst.name != name || inst.loop != loop || inst.ended) continue;
      ++sameClip;
      if (best == nullptr || inst.refs < best->refs) best = &inst;
    }
    if (best != nullptr) {
      ++best->refs;
      return best;
    }
    // 已达上限时也不能复用 ended 实例：画面定格会让新成员动作只闪一帧。
    if (sameClip >= kMaxInstancesPerClip) {
      ClipInstance* fallback = nullptr;
      for (const auto& p : g_instances) {
        ClipInstance& inst = *p;
        if (inst.name != name || inst.loop != loop || inst.ended) continue;
        if (fallback == nullptr || inst.refs < fallback->refs) fallback = &inst;
      }
      if (fallback != nullptr) {
        ++fallback->refs;
        return fallback;
      }
    }
  }

  auto inst = std::make_unique<ClipInstance>();
  inst->name = name;
  inst->loop = loop;
  if (!core::Open(inst->dec, path, outW, outH)) return nullptr;
  inst->outW = inst->dec.videoW;
  inst->outH = inst->dec.videoH;
  inst->fps = inst->dec.fps > 0 ? inst->dec.fps : 30.0;
  inst->clipFrames = std::max(1, inst->dec.clipFrames - 1);
  const int maskW = MaskWidth(inst->outW);
  const int maskH = maskW * inst->outH / inst->outW;
  if (!g_callbacks.createTexture ||
      !g_callbacks.createTexture(*inst, inst->outW, inst->outH, maskW, maskH)) {
    return nullptr;
  }
  inst->dec.loop = loop;
  core::Start(inst->dec);
  inst->running = true;
  // 同步等首帧（最多 200ms），保证切段返回后纹理已有内容。
  for (int i = 0; i < 40; ++i) {
    if (core::TakeFrame(inst->dec)) {
      UploadInstance(*inst);
      ++inst->playedFrames;
      inst->frameStamp = 1;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  inst->refs = 1;
  g_instances.push_back(std::move(inst));
  return g_instances.back().get();
}

void ReleaseClip(ClipInstance* inst) {
  if (inst == nullptr || inst->refs <= 0) return;
  --inst->refs;
  if (inst->refs == 0) EraseInstance(inst);
}

void SetClipVisible(ClipInstance* inst, bool visible) {
  if (inst == nullptr) return;
  if (visible) {
    if (inst->visibleRefs++ == 0 && !inst->running && inst->refs > 0) {
      core::Start(inst->dec);
      inst->running = true;
    }
    return;
  }
  if (inst->visibleRefs > 0) --inst->visibleRefs;
  if (inst->visibleRefs == 0 && inst->running) {
    core::Stop(inst->dec);
    inst->running = false;
  }
}

void UpdateClips() {
  for (const auto& p : g_instances) {
    ClipInstance& inst = *p;
    if (inst.refs <= 0 || inst.visibleRefs == 0) continue;
    if (core::TakeFrame(inst.dec)) {
      UploadInstance(inst);
      ++inst.playedFrames;
      ++inst.frameStamp;
    }
    if (!inst.loop && !inst.ended &&
        (inst.dec.eof.load() || inst.playedFrames >= inst.clipFrames)) {
      inst.ended = true;
    }
  }
}

void ResizeClips(int outW, int outH) {
  if (outW <= 0 || outH <= 0) return;
  int changed = 0;
  for (const auto& p : g_instances) {
    ClipInstance& inst = *p;
    if (inst.refs <= 0 || (inst.outW == outW && inst.outH == outH)) continue;
    const bool wasRunning = inst.running;
    core::Stop(inst.dec);
    const int maskW = MaskWidth(outW);
    const int maskH = maskW * outH / outW;
    // 先造新纹理再改解码尺寸：任一步失败都保留旧纹理与旧尺寸。
    ClipInstance probe;
    probe.name = inst.name;
    const bool textureOk = g_callbacks.createTexture &&
                           g_callbacks.createTexture(probe, outW, outH, maskW, maskH);
    if (!textureOk || !core::Resize(inst.dec, outW, outH)) {
      if (g_callbacks.destroyTexture) g_callbacks.destroyTexture(probe);
      if (wasRunning) core::Start(inst.dec);
      continue;
    }
    if (g_callbacks.destroyTexture) g_callbacks.destroyTexture(inst);
    inst.texture = probe.texture;
    probe.texture = nullptr;
    inst.outW = outW;
    inst.outH = outH;
    inst.mask.mask.clear();
    inst.mask.rects.clear();
    if (wasRunning) core::Start(inst.dec);
    ++changed;
  }
  if (changed > 0) {
    log::Log("clip resize all: {}x{} changed={} total={}", outW, outH, changed,
             static_cast<int>(g_instances.size()));
  }
}

void StopAllClips() {
  for (const auto& p : g_instances) {
    if (g_callbacks.destroyTexture) g_callbacks.destroyTexture(*p);
    core::Stop(p->dec);
  }
  g_instances.clear();
}

int ActiveClipCount() { return static_cast<int>(g_instances.size()); }

}  // namespace core
