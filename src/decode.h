#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ears {

struct DecodedAudio {
  std::vector<float> samples;
  int sample_rate = 16000;
  int channels = 1;
  double duration_sec = 0;
};

// Streaming mono 16 kHz float PCM from a media file.
class AudioDecoder {
 public:
  static constexpr int kSampleRate = 16000;
  static constexpr int kChannels = 1;

  AudioDecoder();
  ~AudioDecoder();

  AudioDecoder(const AudioDecoder&) = delete;
  AudioDecoder& operator=(const AudioDecoder&) = delete;

  bool open(const std::string& path, std::string& err);
  void close();

  // Container duration in seconds when known; 0 if unavailable.
  double duration_sec() const;
  bool eof() const;

  // Append up to max_samples of PCM into out (cleared first). Returns false on error.
  // Returns true with empty out when already at EOF.
  bool read(size_t max_samples, std::vector<float>& out, std::string& err);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

bool decode_audio_file(const std::string& path, DecodedAudio& out, std::string& err);

}  // namespace ears
