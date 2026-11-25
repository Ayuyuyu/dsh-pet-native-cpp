#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace core {

namespace detail {

// FFmpeg C API 资源统一交给 unique_ptr + 自定义 deleter，
// 让 Open() 的任意提前 return 都能自动回收已分配的部分资源。
struct FormatCloser {
  void operator()(AVFormatContext* p) const {
    if (p != nullptr) avformat_close_input(&p);
  }
};
struct CodecFreer {
  void operator()(AVCodecContext* p) const {
    if (p != nullptr) avcodec_free_context(&p);
  }
};
struct SwsFreer {
  void operator()(SwsContext* p) const {
    if (p != nullptr) sws_freeContext(p);
  }
};
struct PacketFreer {
  void operator()(AVPacket* p) const {
    if (p != nullptr) av_packet_free(&p);
  }
};
struct FrameFreer {
  void operator()(AVFrame* p) const {
    if (p != nullptr) av_frame_free(&p);
  }
};

}  // namespace detail

using FormatPtr = std::unique_ptr<AVFormatContext, detail::FormatCloser>;
using CodecPtr = std::unique_ptr<AVCodecContext, detail::CodecFreer>;
using SwsPtr = std::unique_ptr<SwsContext, detail::SwsFreer>;
using PacketPtr = std::unique_ptr<AVPacket, detail::PacketFreer>;
using FramePtr = std::unique_ptr<AVFrame, detail::FrameFreer>;

// 动画实例持有的解码状态：FFmpeg 资源 + 双缓冲 BGRA + 解码线程。
struct DecodeState {
  FormatPtr fmt;
  CodecPtr codec;
  SwsPtr sws;
  PacketPtr pkt;
  FramePtr frame;

  std::vector<uint8_t> bgra;      // 主线程消费
  std::vector<uint8_t> bgraBack;  // 解码线程写入
  // videoW/videoH 是**输出**尺寸（= bgra 尺寸），默认等于源视频尺寸。
  // srcW/srcH 始终是编码尺寸，sws 与 alpha 平面按它取值。
  int videoW = 0;
  int videoH = 0;
  int srcW = 0;
  int srcH = 0;
  int streamIndex = -1;
  double fps = 30.0;
  int clipFrames = 300;

  std::thread thread;
  std::atomic<bool> stop{false};
  std::atomic<bool> eof{false};  // 已解码到文件尾（不循环时置位）
  bool loop = false;             // EOF 是否 seek 回开头循环（drag 反馈用）
  std::mutex mutex;
  std::condition_variable cv;
  bool frameReady = false;  // 仅在 mutex 内读写

  // 析构时先 join 解码线程，再释放 FFmpeg 资源：宠物销毁不会再留下
  // 未回收的解码上下文，也不会让线程访问已释放的对象。
  ~DecodeState();
  DecodeState() = default;
  DecodeState(const DecodeState&) = delete;
  DecodeState& operator=(const DecodeState&) = delete;
};

// 打开一段 webm（libvpx-vp9，保留 alpha），读取帧率并按容器时长估算帧数。
// outW/outH 为 0 时按源视频尺寸输出；桌面宠物按窗口尺寸传入，
// 省掉「按 640×360 解码上传、再由 GPU 缩到 288×162」的 4 倍内存带宽。
bool Open(DecodeState& d, const std::string& path, int outW = 0, int outH = 0);

// 释放所有 FFmpeg 资源。
void Close(DecodeState& d);

// 只改输出尺寸：重建 swscale 与 bgra 缓冲，**不动解码器与文件位置**，
// 所以画面不会跳回片头，下一帧就按新尺寸输出。
// 调用前必须先 Stop（解码线程正在使用 sws 与 bgra）。
bool Resize(DecodeState& d, int outW, int outH);

// 启动/停止解码线程（线程里做解码 + 色彩转换 + alpha 覆盖）。
void Start(DecodeState& d);
void Stop(DecodeState& d);

// 主线程取最新帧：有就交换到 d.bgra 并返回 true。
bool TakeFrame(DecodeState& d);

// 解一帧（线程内用，也用于首帧同步）。
bool DecodeNext(DecodeState& d);

}  // namespace core
