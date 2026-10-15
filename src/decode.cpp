#include "decode.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>

namespace ears {

struct AudioDecoder::Impl {
  AVFormatContext* fmt_ctx = nullptr;
  AVCodecContext* codec_ctx = nullptr;
  SwrContext* swr_ctx = nullptr;
  AVPacket* packet = nullptr;
  AVFrame* frame = nullptr;
  int audio_stream_idx = -1;
  double duration_sec = 0;
  bool opened = false;
  bool input_eof = false;
  bool decoder_flushed = false;
  bool swr_flushed = false;
  std::vector<float> pending;
  std::vector<float> convert_buf;
  std::string err;

  ~Impl() { reset(); }

  void reset() {
    av_frame_free(&frame);
    av_packet_free(&packet);
    swr_free(&swr_ctx);
    avcodec_free_context(&codec_ctx);
    if (fmt_ctx) {
      avformat_close_input(&fmt_ctx);
      fmt_ctx = nullptr;
    }
    audio_stream_idx = -1;
    duration_sec = 0;
    opened = false;
    input_eof = false;
    decoder_flushed = false;
    swr_flushed = false;
    pending.clear();
    convert_buf.clear();
    err.clear();
  }

  bool append_converted(const uint8_t** in_data, int in_samples) {
    const int out_rate = AudioDecoder::kSampleRate;
    int out_samples = static_cast<int>(
        av_rescale_rnd(swr_get_delay(swr_ctx, codec_ctx->sample_rate) + in_samples, out_rate,
                       codec_ctx->sample_rate, AV_ROUND_UP));
    if (out_samples < 1) out_samples = 1;
    if (out_samples > static_cast<int>(convert_buf.size())) {
      convert_buf.resize(static_cast<size_t>(out_samples));
    }
    uint8_t* out_data[1] = {reinterpret_cast<uint8_t*>(convert_buf.data())};
    int got = swr_convert(swr_ctx, out_data, out_samples, in_data, in_samples);
    if (got < 0) {
      err = "audio resample failed";
      return false;
    }
    if (got > 0) {
      pending.insert(pending.end(), convert_buf.begin(), convert_buf.begin() + got);
    }
    return true;
  }

  bool resample_frame(AVFrame* f) {
    const uint8_t* in_data[16] = {nullptr};
    for (unsigned p = 0; p < codec_ctx->ch_layout.nb_channels && p < 16; ++p) {
      in_data[p] = f->data[p];
    }
    return append_converted(in_data, f->nb_samples);
  }

  bool decode_available_frames() {
    while (true) {
      int ret = avcodec_receive_frame(codec_ctx, frame);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return true;
      if (ret < 0) {
        err = "failed to decode audio frame";
        return false;
      }
      if (!resample_frame(frame)) return false;
    }
  }

  bool pump_more() {
    if (!pending.empty()) return true;

    while (!input_eof) {
      int ret = av_read_frame(fmt_ctx, packet);
      if (ret < 0) {
        input_eof = true;
        break;
      }
      if (packet->stream_index != audio_stream_idx) {
        av_packet_unref(packet);
        continue;
      }
      ret = avcodec_send_packet(codec_ctx, packet);
      av_packet_unref(packet);
      if (ret < 0) {
        err = "failed to send audio packet";
        return false;
      }
      if (!decode_available_frames()) return false;
      if (!pending.empty()) return true;
    }

    if (!decoder_flushed) {
      decoder_flushed = true;
      if (avcodec_send_packet(codec_ctx, nullptr) < 0) {
        err = "failed to flush audio decoder";
        return false;
      }
      if (!decode_available_frames()) return false;
      if (!pending.empty()) return true;
    }

    if (!swr_flushed) {
      while (true) {
        int delay = static_cast<int>(swr_get_delay(swr_ctx, AudioDecoder::kSampleRate));
        if (delay <= 0) {
          swr_flushed = true;
          break;
        }
        int out_samples = delay + 256;
        if (out_samples > static_cast<int>(convert_buf.size())) {
          convert_buf.resize(static_cast<size_t>(out_samples));
        }
        uint8_t* out_data[1] = {reinterpret_cast<uint8_t*>(convert_buf.data())};
        int got = swr_convert(swr_ctx, out_data, out_samples, nullptr, 0);
        if (got < 0) {
          err = "audio resample flush failed";
          return false;
        }
        if (got == 0) {
          swr_flushed = true;
          break;
        }
        pending.insert(pending.end(), convert_buf.begin(), convert_buf.begin() + got);
        if (!pending.empty()) return true;
      }
    }

    return true;
  }
};

AudioDecoder::AudioDecoder() : impl_(std::make_unique<Impl>()) {}
AudioDecoder::~AudioDecoder() = default;

bool AudioDecoder::open(const std::string& path, std::string& err) {
  impl_->reset();

  if (avformat_open_input(&impl_->fmt_ctx, path.c_str(), nullptr, nullptr) != 0) {
    err = "failed to open input file";
    return false;
  }

  if (avformat_find_stream_info(impl_->fmt_ctx, nullptr) < 0) {
    err = "failed to read stream information";
    impl_->reset();
    return false;
  }

  for (unsigned int i = 0; i < impl_->fmt_ctx->nb_streams; ++i) {
    if (impl_->fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      impl_->audio_stream_idx = static_cast<int>(i);
      break;
    }
  }
  if (impl_->audio_stream_idx < 0) {
    err = "no audio stream found";
    impl_->reset();
    return false;
  }

  AVStream* audio_stream = impl_->fmt_ctx->streams[impl_->audio_stream_idx];
  if (audio_stream->duration > 0 && audio_stream->time_base.den > 0) {
    impl_->duration_sec = static_cast<double>(audio_stream->duration) * av_q2d(audio_stream->time_base);
  } else if (impl_->fmt_ctx->duration > 0) {
    impl_->duration_sec = static_cast<double>(impl_->fmt_ctx->duration) / static_cast<double>(AV_TIME_BASE);
  }

  const AVCodec* codec = avcodec_find_decoder(audio_stream->codecpar->codec_id);
  if (!codec) {
    err = "unsupported audio codec";
    impl_->reset();
    return false;
  }

  impl_->codec_ctx = avcodec_alloc_context3(codec);
  if (!impl_->codec_ctx) {
    err = "failed to allocate codec context";
    impl_->reset();
    return false;
  }

  if (avcodec_parameters_to_context(impl_->codec_ctx, audio_stream->codecpar) < 0) {
    err = "failed to copy codec parameters";
    impl_->reset();
    return false;
  }

  if (avcodec_open2(impl_->codec_ctx, codec, nullptr) < 0) {
    err = "failed to open codec";
    impl_->reset();
    return false;
  }

  if (impl_->codec_ctx->sample_rate <= 0 || impl_->codec_ctx->ch_layout.nb_channels <= 0) {
    err = "invalid audio stream parameters";
    impl_->reset();
    return false;
  }

  AVChannelLayout out_ch_layout = AV_CHANNEL_LAYOUT_MONO;
  if (swr_alloc_set_opts2(&impl_->swr_ctx, &out_ch_layout, AV_SAMPLE_FMT_FLT, kSampleRate,
                          &impl_->codec_ctx->ch_layout, impl_->codec_ctx->sample_fmt,
                          impl_->codec_ctx->sample_rate, 0, nullptr) < 0) {
    err = "failed to allocate resampler";
    impl_->reset();
    return false;
  }

  if (swr_init(impl_->swr_ctx) < 0) {
    err = "failed to initialize resampler";
    impl_->reset();
    return false;
  }

  impl_->packet = av_packet_alloc();
  impl_->frame = av_frame_alloc();
  if (!impl_->packet || !impl_->frame) {
    err = "failed to allocate decode buffers";
    impl_->reset();
    return false;
  }

  impl_->opened = true;
  return true;
}

void AudioDecoder::close() {
  if (impl_) impl_->reset();
}

double AudioDecoder::duration_sec() const { return impl_ ? impl_->duration_sec : 0; }

bool AudioDecoder::eof() const {
  if (!impl_ || !impl_->opened) return true;
  return impl_->input_eof && impl_->decoder_flushed && impl_->swr_flushed && impl_->pending.empty();
}

bool AudioDecoder::read(size_t max_samples, std::vector<float>& out, std::string& err) {
  out.clear();
  if (!impl_ || !impl_->opened) {
    err = "decoder not open";
    return false;
  }
  if (max_samples == 0) return true;

  while (out.size() < max_samples) {
    if (impl_->pending.empty()) {
      if (!impl_->pump_more()) {
        err = impl_->err.empty() ? "decode failed" : impl_->err;
        return false;
      }
      if (impl_->pending.empty()) break;
    }
    const size_t take = std::min(max_samples - out.size(), impl_->pending.size());
    out.insert(out.end(), impl_->pending.begin(), impl_->pending.begin() + static_cast<std::ptrdiff_t>(take));
    impl_->pending.erase(impl_->pending.begin(),
                         impl_->pending.begin() + static_cast<std::ptrdiff_t>(take));
  }
  return true;
}

bool decode_audio_file(const std::string& path, DecodedAudio& out, std::string& err) {
  out = DecodedAudio{};
  AudioDecoder dec;
  if (!dec.open(path, err)) return false;

  std::vector<float> pcm;
  std::vector<float> chunk;
  constexpr size_t kReadChunk = 16000 * 30;
  while (!dec.eof()) {
    if (!dec.read(kReadChunk, chunk, err)) return false;
    if (chunk.empty()) break;
    pcm.insert(pcm.end(), chunk.begin(), chunk.end());
  }

  if (pcm.empty()) {
    err = "decoded audio is empty";
    return false;
  }

  out.samples = std::move(pcm);
  out.sample_rate = AudioDecoder::kSampleRate;
  out.channels = AudioDecoder::kChannels;
  out.duration_sec = static_cast<double>(out.samples.size()) / static_cast<double>(out.sample_rate);
  return true;
}

}  // namespace ears
