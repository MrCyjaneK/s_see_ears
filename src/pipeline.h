#pragma once

#include <functional>
#include <string>
#include <vector>

namespace ears {

struct Token {
  std::string text;
  double start = 0;
  double end = 0;
  float confidence = 0;
};

struct Segment {
  double start = 0;
  double end = 0;
  std::string text;
  float confidence = 0;
  float no_speech = 0;
  std::vector<Token> tokens;
};

struct Transcript {
  std::string language;
  std::vector<Segment> segments;
};

struct AnalysisResult {
  double duration_sec = 0;
  int sample_rate = 16000;
  int channels = 1;
  Transcript transcript;
};

// Stream-decode path and run whisper in overlapping windows (peak PCM ~30s).
AnalysisResult analyze_file(const std::string& path,
                            const std::function<void(int, const std::string&)>& progress);

// Transcribe an in-memory PCM buffer (16 kHz mono float). Kept for tests/tools.
AnalysisResult analyze_audio(const std::vector<float>& samples,
                             const std::function<void(int, const std::string&)>& progress);

}  // namespace ears
