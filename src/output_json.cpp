#include "output_json.h"

#include <nlohmann/json.hpp>

namespace ears {

namespace {

nlohmann::json tokens_json(const std::vector<Token>& tokens) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& t : tokens) {
    arr.push_back({{"text", t.text}, {"start", t.start}, {"end", t.end}, {"confidence", t.confidence}});
  }
  return arr;
}

nlohmann::json segments_json(const std::vector<Segment>& segments) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& s : segments) {
    arr.push_back({{"start", s.start},
                   {"end", s.end},
                   {"text", s.text},
                   {"confidence", s.confidence},
                   {"noSpeech", s.no_speech},
                   {"tokens", tokens_json(s.tokens)}});
  }
  return arr;
}

nlohmann::json transcript_json(const Transcript& t) {
  return {{"language", t.language}, {"segments", segments_json(t.segments)}};
}

}  // namespace

std::string to_json(const AnalysisResult& r) {
  nlohmann::json j;
  j["media"] = {{"durationSec", r.duration_sec}, {"sampleRate", r.sample_rate}, {"channels", r.channels}};
  j["transcript"] = transcript_json(r.transcript);
  return j.dump(2);
}

}  // namespace ears
