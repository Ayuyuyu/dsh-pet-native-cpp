#include "decode.h"

#include <algorithm>
#include <chrono>

extern "C" {
#include <libavutil/log.h>
}

namespace core {

void Close(DecodeState& d) {
  d.frame.reset();
  d.pkt.reset();
  d.sws.reset();
  d.codec.reset();
  d.fmt.reset();
  d.streamIndex = -1;
  d.videoW = d.videoH = 0;
  d.srcW = d.srcH = 0;
}

bool Open(DecodeState& d, const std::string& path, int outW, int outH) {
  // 允许对已用过的 DecodeState 直接 Open：先 join 线程再释放资源，
  // 否则调用方漏掉 Stop 时解码线程会在 Close 后继续访问 FFmpeg 上下文。
  Stop(d);
  // 屏蔽 FFmpeg/libvpx 的 info 输出（默认会往 stderr 打 libvpx 版本横幅），
  // 只保留 error，避免正常运行时终端被刷。多只宠物并发 Open 也只设置一次。
  static std::once_flag log_once;
  std::call_once(log_once, [] { av_log_set_level(AV_LOG_ERROR); });
  Close(d);
  d.eof.store(false);
  d.loop = false;

  AVFormatContext* fmt = nullptr;
  if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
    if (fmt != nullptr) avformat_close_input(&fmt);
    return false;
  }
  d.fmt.reset(fmt);
  if (avformat_find_stream_info(d.fmt.get(), nullptr) < 0) return false;

  d.streamIndex = av_find_best_stream(d.fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1,
                                      nullptr, 0);
  if (d.streamIndex < 0) return false;

  AVCodecParameters* par = d.fmt->streams[d.streamIndex]->codecpar;
  // 内置 vp9 解码器会丢弃 WebM 的 alpha 侧数据，必须用 libvpx-vp9。
  const AVCodec* codec = avcodec_find_decoder_by_name("libvpx-vp9");
  if (codec == nullptr) codec = avcodec_find_decoder(par->codec_id);
  if (codec == nullptr) return false;

  d.codec.reset(avcodec_alloc_context3(codec));
  if (!d.codec) return false;
  if (avcodec_parameters_to_context(d.codec.get(), par) < 0) return false;
  if (avcodec_open2(d.codec.get(), codec, nullptr) < 0) return false;

  // 输出尺寸：0 表示按源尺寸（Linux 端行为不变）；桌面宠物传窗口尺寸，
  // 让 sws 直接把 640×360 缩到 288×162，省掉 4 倍的 memcpy 与纹理上传。
  d.srcW = d.codec->width;
  d.srcH = d.codec->height;
  d.videoW = outW > 0 ? outW : d.srcW;
  d.videoH = outH > 0 ? outH : d.srcH;
  if (d.videoW <= 0 || d.videoH <= 0) return false;
  const size_t bytes = static_cast<size_t>(d.videoW) * d.videoH * 4;
  d.bgra.assign(bytes, 0);
  d.bgraBack.assign(bytes, 0);

  // YUVA420P → BGRA 由 sws 一次完成，RGB 与 alpha 共用 BILINEAR 插值，
  // 边缘天然平滑；不要再单独覆盖 alpha 平面。回退到内置 vp9 时源是
  // yuv420p，按解码器实际 pix_fmt 建缩放器，避免读不存在的第 4 个平面。
  const AVPixelFormat src_fmt = d.codec->pix_fmt != AV_PIX_FMT_NONE
                                    ? d.codec->pix_fmt
                                    : AV_PIX_FMT_YUVA420P;
  d.sws.reset(sws_getContext(d.srcW, d.srcH, src_fmt, d.videoW, d.videoH,
                             AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr,
                             nullptr));
  if (!d.sws) return false;

  d.pkt.reset(av_packet_alloc());
  d.frame.reset(av_frame_alloc());
  if (!d.pkt || !d.frame) return false;

  const AVRational fr = d.fmt->streams[d.streamIndex]->avg_frame_rate;
  const double raw_fps = (fr.num > 0 && fr.den > 0) ? av_q2d(fr) : 30.0;
  // 异常容器的 fps 可能是 0、负数或极大值：前者会让解码间隔为 0（忙循环），
  // 后者会让 dur*fps 溢出 int。夹到 [1, 240]，正常素材（~30fps）不受影响。
  d.fps = raw_fps < 1.0 ? 1.0 : (raw_fps > 240.0 ? 240.0 : raw_fps);
  const double dur =
      d.fmt->duration > 0 ? d.fmt->duration / static_cast<double>(AV_TIME_BASE)
                          : 10.0;
  const double safe_dur = dur < 3600.0 ? dur : 3600.0;
  const int64_t nb = d.fmt->streams[d.streamIndex]->nb_frames;
  if (nb > 0 && nb <= 864000) {
    d.clipFrames = static_cast<int>(nb);
  } else {
    // fps ≤240、时长 ≤1h，乘积 ≤864000，远小于 INT_MAX。
    d.clipFrames = static_cast<int>(safe_dur * d.fps) + 1;
  }
  return true;
}

bool Resize(DecodeState& d, int outW, int outH) {
  if (d.codec == nullptr || d.srcW <= 0 || d.srcH <= 0) return false;
  if (outW <= 0 || outH <= 0) return false;
  if (outW == d.videoW && outH == d.videoH) return true;  // 尺寸没变

  // 只重建缩放器与缓冲：codec/fmt/frame/pkt 全部保留，
  // 因此解码位置不丢，画面不会跳回片头。
  // 事务化：任一步失败都保持旧的 sws / videoW / bgra 不变，
  // 否则调用方「失败就按原尺寸继续播」会拿到空 sws 与新尺寸的 bgra。
  const AVPixelFormat src_fmt = d.codec->pix_fmt != AV_PIX_FMT_NONE
                                    ? d.codec->pix_fmt
                                    : AV_PIX_FMT_YUVA420P;
  SwsPtr next(sws_getContext(d.srcW, d.srcH, src_fmt, outW, outH,
                             AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr,
                             nullptr));
  if (!next) return false;
  const size_t bytes = static_cast<size_t>(outW) * outH * 4;
  std::vector<uint8_t> front;
  std::vector<uint8_t> back;
  try {
    front.assign(bytes, 0);
    back.assign(bytes, 0);
  } catch (...) {
    return false;
  }
  d.sws = std::move(next);
  d.videoW = outW;
  d.videoH = outH;
  d.bgra.swap(front);
  d.bgraBack.swap(back);
  return true;
}

bool DecodeNext(DecodeState& d) {
  while (!d.stop.load()) {
    if (av_read_frame(d.fmt.get(), d.pkt.get()) < 0) {
      // 文件尾：drag 类 loop 动画 seek 回开头循环；其余停在最后一帧，
      // 由状态机按 eof 切段（避免切段前闪回开头帧，表现为尾部画面缺失）。
      if (!d.loop) {
        // drain：取出解码器内部缓冲的尾帧，否则片段末尾会少几帧。
        avcodec_send_packet(d.codec.get(), nullptr);
        if (avcodec_receive_frame(d.codec.get(), d.frame.get()) == 0) {
          return true;
        }
        d.eof.store(true);
        return false;
      }
      // seek 失败时不能再盲目重试 av_read_frame，否则会在 EOF 上忙循环。
      if (av_seek_frame(d.fmt.get(), d.streamIndex, 0, AVSEEK_FLAG_BACKWARD) <
          0) {
        d.eof.store(true);
        return false;
      }
      avcodec_flush_buffers(d.codec.get());
      continue;
    }
    if (d.pkt->stream_index != d.streamIndex) {
      av_packet_unref(d.pkt.get());
      continue;
    }
    const int sent = avcodec_send_packet(d.codec.get(), d.pkt.get());
    av_packet_unref(d.pkt.get());
    if (sent < 0) continue;
    if (avcodec_receive_frame(d.codec.get(), d.frame.get()) == 0) return true;
  }
  return false;
}

namespace {

// 解码线程：解码 + 色彩转换（含 alpha，由 sws 一次完成），写到 bgraBack。
// 按绝对时间调度：解码/转换耗时被「提前解码」吸收，帧间隔稳定在 1000/fps，
// 不再叠加解码抖动（此前 sleep 在解码之后，实际间隔 ≈ 解码耗时 + 41.7ms，
// 帧率掉到 ~18fps 且抖动，观感不如浏览器按 PTS 播放丝滑）。
void DecodeThread(DecodeState* d) {
  const double fps = d->fps > 0 ? d->fps : 30.0;
  // 下限 1000μs：即便 Open 之外的路径写入异常 fps，也不会退化成无 sleep 忙循环。
  const auto interval_us =
      std::max<long long>(1000, static_cast<long long>(1000000.0 / fps));
  const auto interval = std::chrono::microseconds(interval_us);
  auto next = std::chrono::steady_clock::now();
  while (!d->stop.load()) {
    if (!DecodeNext(*d)) break;

    {
      std::unique_lock<std::mutex> lock(d->mutex);
      // bgraBack 只有在 frameReady=false（上一帧已被主线程换走）时才可覆写。
      // 写入、置标志与主线程 swap 必须在同一把锁内，否则追帧路径会并发写同一缓冲。
      d->cv.wait(lock, [d] { return !d->frameReady || d->stop.load(); });
      if (d->stop.load()) break;

      uint8_t* dst[4] = {d->bgraBack.data(), nullptr, nullptr, nullptr};
      const int dst_stride[4] = {d->videoW * 4, 0, 0, 0};
      // 第 5 个参数是**源**高度（srcSliceH），不是输出高度：缩小时传 videoH
      // 只会转换源图的前 videoH 行，剩下的输出行保持初始值，画面下半截会变黑。
      sws_scale(d->sws.get(), d->frame->data, d->frame->linesize, 0, d->srcH,
                dst, dst_stride);
      // sws 从 YUVA420P 直出 BGRA 时已自带 alpha 且与 RGB 用同一套插值，
      // 边缘平滑（实测 384×216 帧有 7 万个半透明像素）。早先"alpha 不可靠"
      // 的判断在现在这套 FFmpeg/libvpx 组合上不成立；二次缩放反而会因
      // 最近的 BILINEAR 插值让 alpha 与 RGB 相位错开，产生毛刺和 2 核开销。
      d->frameReady = true;
    }
    next += interval;
    const auto now = std::chrono::steady_clock::now();
    if (next > now) {
      std::this_thread::sleep_until(next);
    } else {
      next = now;  // 解码跟不上时立即追帧，不累积延迟
    }
  }
}

}  // namespace

void Start(DecodeState& d) {
  Stop(d);
  d.stop.store(false);
  {
    std::lock_guard<std::mutex> lock(d.mutex);
    d.frameReady = false;
  }
  d.thread = std::thread(DecodeThread, &d);
}

void Stop(DecodeState& d) {
  d.stop.store(true);
  {
    std::lock_guard<std::mutex> lock(d.mutex);
    d.frameReady = false;
  }
  d.cv.notify_all();  // 唤醒正等待空闲缓冲的解码线程
  if (d.thread.joinable()) d.thread.join();
}

bool TakeFrame(DecodeState& d) {
  {
    std::lock_guard<std::mutex> lock(d.mutex);
    if (!d.frameReady) return false;
    d.bgra.swap(d.bgraBack);
    d.frameReady = false;
  }
  d.cv.notify_one();  // back 缓冲已空闲，解码线程可写下一帧
  return true;
}

DecodeState::~DecodeState() {
  Stop(*this);
  Close(*this);
}

}  // namespace core
