#pragma once

#include <string>

#include "decode.h"
#include "mask.h"

namespace core {

// 平台无关的动画实例。同一动作的多只宠物共享一路解码、一份帧缓冲与掩码；
// texture 是平台资源句柄（Win 为 WinClipTexture*，Linux 为 GL 纹理封装*），
// 由回调创建和销毁，core 只保存指针，不解释其结构。
struct ClipInstance {
  std::string name;
  DecodeState dec;
  MaskResult mask;
  void* texture = nullptr;

  int outW = 0;
  int outH = 0;
  int fps = 30;
  int clipFrames = 1;
  bool loop = false;
  int refs = 0;
  int visibleRefs = 0;
  bool running = false;
  bool ended = false;
  int playedFrames = 0;
  unsigned frameStamp = 0;
};

// 平台注入的资源回调。所有回调只在主线程调用；create 失败时不得修改
// inst.texture，以便 core 保留旧纹理与旧尺寸。
struct ClipPoolCallbacks {
  bool (*createTexture)(ClipInstance& inst, int w, int h, int maskW, int maskH);
  bool (*uploadVideo)(ClipInstance& inst, const uint8_t* bgra, int w, int h);
  void (*uploadMask)(ClipInstance& inst, const char* mask, int mw, int mh);
  void (*destroyTexture)(ClipInstance& inst);
  int (*maskWidthFor)(int windowW);  // 可为 nullptr，使用默认档位
};

void SetClipPoolCallbacks(const ClipPoolCallbacks& callbacks);

// 取一个可用实例；exclusive（move/turn）每次新建，保证时间轴完整。
ClipInstance* AcquireClip(const std::string& name, const std::string& path,
                          bool loop, bool exclusive, int outW, int outH);
void ReleaseClip(ClipInstance* inst);
void SetClipVisible(ClipInstance* inst, bool visible);

// 每帧取帧 → 平台上传 → 更新掩码/结束标记。每实例只处理一次。
void UpdateClips();
// 输出尺寸变化时事务化重建所有活跃实例；失败者保留旧纹理与旧尺寸。
void ResizeClips(int outW, int outH);
void StopAllClips();
int ActiveClipCount();

}  // namespace core
