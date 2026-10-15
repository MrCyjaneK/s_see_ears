#include "pipeline.h"

#include "decode.h"
#include "whisper_ctx.h"

#include "whisper.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace ears {

namespace {

constexpr int kSampleRate = 16000;
// Whisper encoder window is ~30s; hop leaves 2s overlap for boundary stitching.
constexpr size_t kWindowSamples = static_cast<size_t>(kSampleRate * 30);
constexpr size_t kHopSamples = static_cast<size_t>(kSampleRate * 28);
constexpr double kHopSec = static_cast<double>(kHopSamples) / static_cast<double>(kSampleRate);

double cs_to_sec(int64_t cs) { return static_cast<double>(cs) / 100.0; }

struct ProgressUserData {
  const std::function<void(int, const std::string&)>* progress = nullptr;
  int base = 0;
  int span = 100;
  double chunk_sec = 0;
  double chunk_start_sec = 0;
  int chunk_index = 0;  // 0-based
  int est_chunks = 1;
  std::chrono::steady_clock::time_point job_start{};
  std::chrono::steady_clock::time_point chunk_start{};
  std::chrono::steady_clock::time_point last_report{};
  int last_pct = -1;
  int whisper_pct = 0;
  int segments = 0;
  std::atomic<bool> running{false};
  std::mutex mu;
};

std::string format_msg(const ProgressUserData& data, int mapped_pct, const char* phase) {
  const double wall =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - data.job_start).count();
  char buf[224];
  const int chunk_n = data.chunk_index + 1;
  const int chunk_total = std::max(data.est_chunks, chunk_n);
  if (data.segments > 0) {
    std::snprintf(buf, sizeof(buf),
                  "%s %d%% · chunk %d/%d @ %.0fs · %d segment%s · %.0fs wall", phase, mapped_pct,
                  chunk_n, chunk_total, data.chunk_start_sec, data.segments,
                  data.segments == 1 ? "" : "s", wall);
  } else {
    std::snprintf(buf, sizeof(buf), "%s %d%% · chunk %d/%d @ %.0fs · %.0fs wall", phase, mapped_pct,
                  chunk_n, chunk_total, data.chunk_start_sec, wall);
  }
  return buf;
}

void report_progress(ProgressUserData* data, int whisper_pct, bool force) {
  if (!data || !data->progress) return;

  std::lock_guard<std::mutex> lock(data->mu);
  const auto now = std::chrono::steady_clock::now();
  if (!force) {
    const auto since_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - data->last_report).count();
    if (since_ms < 400 && whisper_pct <= data->whisper_pct) return;
  }

  data->whisper_pct = std::max(data->whisper_pct, whisper_pct);
  const double chunk_elapsed = std::chrono::duration<double>(now - data->chunk_start).count();

  // Within-chunk estimate: whisper ticks sparsely; floor by wall time for this window.
  const double expected = std::max(30.0, data->chunk_sec) * 6.0;
  int time_pct = 0;
  if (expected > 0) {
    time_pct = static_cast<int>((chunk_elapsed / expected) * 95.0);
    if (time_pct < 0) time_pct = 0;
    if (time_pct > 95) time_pct = 95;
  }

  const int effective = std::max(data->whisper_pct, time_pct);
  int mapped = data->base + (effective * data->span) / 100;
  if (mapped < data->base) mapped = data->base;
  if (mapped > data->base + data->span - 1) mapped = data->base + data->span - 1;
  if (whisper_pct >= 100) mapped = data->base + data->span;

  if (!force && mapped == data->last_pct &&
      std::chrono::duration_cast<std::chrono::milliseconds>(now - data->last_report).count() < 1000) {
    return;
  }

  data->last_pct = mapped;
  data->last_report = now;
  (*data->progress)(mapped, format_msg(*data, mapped, "transcribing"));
}

void whisper_progress_cb(struct whisper_context*, struct whisper_state*, int p, void* user_data) {
  report_progress(static_cast<ProgressUserData*>(user_data), p, true);
}

void whisper_segment_cb(struct whisper_context*, struct whisper_state*, int n_new, void* user_data) {
  auto* data = static_cast<ProgressUserData*>(user_data);
  if (!data) return;
  int whisper_pct = 0;
  {
    std::lock_guard<std::mutex> lock(data->mu);
    data->segments += n_new;
    whisper_pct = data->whisper_pct;
  }
  report_progress(data, whisper_pct, true);
}

bool whisper_encoder_begin_cb(struct whisper_context*, struct whisper_state*, void* user_data) {
  auto* data = static_cast<ProgressUserData*>(user_data);
  if (!data || !data->progress) return true;
  {
    std::lock_guard<std::mutex> lock(data->mu);
    if (data->last_pct < data->base) data->last_pct = data->base;
    data->last_report = std::chrono::steady_clock::now();
    (*data->progress)(data->last_pct, format_msg(*data, data->last_pct, "transcribing encoder"));
  }
  return true;
}

bool whisper_abort_cb(void* user_data) {
  report_progress(static_cast<ProgressUserData*>(user_data), 0, false);
  return false;
}

whisper_full_params make_params(ProgressUserData* progress_data, const char* language,
                                const char* initial_prompt) {
  whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
  params.n_threads = static_cast<int>(std::thread::hardware_concurrency());
  params.print_progress = false;
  params.print_special = false;
  params.print_realtime = false;
  params.print_timestamps = false;
  params.translate = false;
  params.detect_language = false;
  // nullptr = auto-detect; otherwise pin language across chunks.
  params.language = language;
  params.initial_prompt = initial_prompt;
  params.token_timestamps = true;
  params.thold_pt = 0.01f;
  params.thold_ptsum = 0.01f;
  params.max_len = 0;
  params.split_on_word = false;
  params.max_tokens = 0;
  params.suppress_blank = true;
  params.suppress_nst = false;
  params.no_speech_thold = 0.6f;
  // State is freed between chunks; continuity comes from initial_prompt.
  params.no_context = true;
  params.progress_callback = whisper_progress_cb;
  params.progress_callback_user_data = progress_data;
  params.new_segment_callback = whisper_segment_cb;
  params.new_segment_callback_user_data = progress_data;
  params.encoder_begin_callback = whisper_encoder_begin_cb;
  params.encoder_begin_callback_user_data = progress_data;
  params.abort_callback = whisper_abort_cb;
  params.abort_callback_user_data = progress_data;
  return params;
}

bool is_special_token(whisper_context* ctx, whisper_token id) {
  return id >= whisper_token_eot(ctx);
}

Transcript collect_transcript(whisper_context* ctx, whisper_state* state) {
  Transcript out;
  const int lang_id = whisper_full_lang_id_from_state(state);
  const char* lang = whisper_lang_str(lang_id);
  out.language = lang ? lang : "unknown";

  const int n_segments = whisper_full_n_segments_from_state(state);
  out.segments.reserve(n_segments);
  for (int i = 0; i < n_segments; ++i) {
    Segment seg;
    seg.start = cs_to_sec(whisper_full_get_segment_t0_from_state(state, i));
    seg.end = cs_to_sec(whisper_full_get_segment_t1_from_state(state, i));
    const char* text = whisper_full_get_segment_text_from_state(state, i);
    seg.text = text ? text : "";
    seg.no_speech = whisper_full_get_segment_no_speech_prob_from_state(state, i);

    const int n_tokens = whisper_full_n_tokens_from_state(state, i);
    float prob_sum = 0;
    int prob_count = 0;
    seg.tokens.reserve(n_tokens);
    for (int t = 0; t < n_tokens; ++t) {
      const whisper_token id = whisper_full_get_token_id_from_state(state, i, t);
      if (is_special_token(ctx, id)) continue;
      const char* token_text = whisper_full_get_token_text_from_state(ctx, state, i, t);
      if (!token_text || token_text[0] == '\0') continue;
      Token tok;
      tok.text = token_text;
      tok.start = cs_to_sec(whisper_full_get_token_t0_from_state(state, i, t));
      tok.end = cs_to_sec(whisper_full_get_token_t1_from_state(state, i, t));
      tok.confidence = whisper_full_get_token_p_from_state(state, i, t);
      seg.tokens.push_back(std::move(tok));
      prob_sum += tok.confidence;
      ++prob_count;
    }
    seg.confidence = prob_count > 0 ? prob_sum / static_cast<float>(prob_count) : 0;
    out.segments.push_back(std::move(seg));
  }
  return out;
}

// Last ~224 tokens-worth of text for the next chunk's decoder prompt.
std::string prompt_tail(const Transcript& t) {
  std::string all;
  for (const auto& seg : t.segments) {
    all += seg.text;
  }
  constexpr size_t kMax = 800;
  if (all.size() <= kMax) return all;
  return all.substr(all.size() - kMax);
}

void offset_transcript(Transcript& t, double offset_sec) {
  for (auto& seg : t.segments) {
    seg.start += offset_sec;
    seg.end += offset_sec;
    for (auto& tok : seg.tokens) {
      tok.start += offset_sec;
      tok.end += offset_sec;
    }
  }
}

// Keep segments in [0, keep_until) of chunk-local time; drop overlap tail.
void trim_overlap_tail(Transcript& t, double keep_until_sec) {
  auto& segs = t.segments;
  segs.erase(std::remove_if(segs.begin(), segs.end(),
                            [keep_until_sec](const Segment& s) { return s.start >= keep_until_sec; }),
             segs.end());
}

bool run_transcribe_chunk(whisper_context* ctx, const std::vector<float>& samples, double chunk_sec,
                          double chunk_start_sec, int chunk_index, int est_chunks, int progress_base,
                          int progress_span, const char* language, const char* initial_prompt,
                          std::chrono::steady_clock::time_point job_start,
                          const std::function<void(int, const std::string&)>& progress,
                          Transcript& out_part) {
  ProgressUserData pdata;
  pdata.progress = &progress;
  pdata.base = progress_base;
  pdata.span = progress_span;
  pdata.chunk_sec = chunk_sec;
  pdata.chunk_start_sec = chunk_start_sec;
  pdata.chunk_index = chunk_index;
  pdata.est_chunks = est_chunks;
  pdata.job_start = job_start;
  pdata.chunk_start = std::chrono::steady_clock::now();
  pdata.last_report = pdata.chunk_start;
  pdata.running = true;

  whisper_state* state = whisper_init_state(ctx);
  if (!state) {
    return false;
  }

  std::thread heartbeat([&pdata]() {
    while (pdata.running.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      if (!pdata.running.load(std::memory_order_relaxed)) break;
      report_progress(&pdata, 0, false);
    }
  });

  whisper_full_params params = make_params(&pdata, language, initial_prompt);
  const bool ok =
      whisper_full_with_state(ctx, state, params, samples.data(), static_cast<int>(samples.size())) == 0;

  pdata.running = false;
  heartbeat.join();

  if (ok) {
    report_progress(&pdata, 100, true);
    out_part = collect_transcript(ctx, state);
  }

  // Drop KV + compute buffers so RSS can return toward weights-only between chunks.
  whisper_free_state(state);
  return ok;
}

// Map chunk i of N into [progress_lo, progress_hi) using float fractions so long
// jobs stay proportional (integer span = total/N collapses to 1 and skews early).
void chunk_progress_range(int chunk_index, int est_chunks, int progress_lo, int progress_hi,
                          int& base, int& span) {
  const int total = std::max(1, progress_hi - progress_lo);
  const int n = std::max(1, est_chunks);
  const int start =
      progress_lo + static_cast<int>((static_cast<double>(chunk_index) / static_cast<double>(n)) * total);
  int end =
      progress_lo + static_cast<int>((static_cast<double>(chunk_index + 1) / static_cast<double>(n)) * total);
  if (end <= start) end = start + 1;
  if (end > progress_hi) end = progress_hi;
  base = std::min(start, progress_hi - 1);
  span = std::max(1, end - base);
}

void append_needed(AudioDecoder& dec, std::vector<float>& buffer, size_t want, std::string& err) {
  while (buffer.size() < want && !dec.eof()) {
    std::vector<float> chunk;
    const size_t need = want - buffer.size();
    if (!dec.read(need, chunk, err)) {
      throw std::runtime_error(err.empty() ? "failed to decode audio" : err);
    }
    if (chunk.empty()) break;
    buffer.insert(buffer.end(), chunk.begin(), chunk.end());
  }
}

}  // namespace

AnalysisResult analyze_file(const std::string& path,
                            const std::function<void(int, const std::string&)>& progress) {
  auto infer_lock = whisper::lock_inference();
  whisper_context* ctx = whisper::context();
  if (!ctx) {
    throw std::runtime_error("whisper context not loaded");
  }

  progress(5, "opening media");
  AudioDecoder dec;
  std::string err;
  if (!dec.open(path, err)) {
    throw std::runtime_error(err.empty() ? "failed to open media" : err);
  }

  const double known_duration = dec.duration_sec();
  char prep[96];
  if (known_duration > 0) {
    std::snprintf(prep, sizeof(prep), "preparing transcription (%.1fs audio)", known_duration);
  } else {
    std::snprintf(prep, sizeof(prep), "preparing transcription");
  }
  progress(10, prep);

  const auto job_start = std::chrono::steady_clock::now();

  std::vector<float> buffer;
  AnalysisResult result;
  result.sample_rate = kSampleRate;
  result.channels = 1;
  result.transcript.language = "unknown";

  // Sliding window: refill to kWindowSamples, transcribe, keep overlap, repeat.
  size_t absolute_sample = 0;
  size_t total_samples_seen = 0;
  std::string pinned_lang;
  const char* language = nullptr;
  std::string prompt;
  int chunk_index = 0;

  const int progress_lo = 12;
  const int progress_hi = 92;

  while (true) {
    append_needed(dec, buffer, kWindowSamples, err);
    if (buffer.empty()) {
      if (chunk_index == 0) {
        throw std::runtime_error("audio has no samples");
      }
      break;
    }

    const bool input_exhausted = dec.eof();
    const bool last_chunk = input_exhausted && buffer.size() <= kWindowSamples;
    const size_t chunk_n = std::min(buffer.size(), kWindowSamples);
    std::vector<float> chunk(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(chunk_n));

    const double chunk_start_sec =
        static_cast<double>(absolute_sample) / static_cast<double>(kSampleRate);
    const double chunk_sec = static_cast<double>(chunk.size()) / static_cast<double>(kSampleRate);

    int est_chunks = chunk_index + 1;
    if (known_duration > 0) {
      est_chunks = std::max(est_chunks, static_cast<int>((known_duration + kHopSec - 1e-6) / kHopSec));
    }
    int base = 0;
    int span = 1;
    chunk_progress_range(chunk_index, est_chunks, progress_lo, progress_hi, base, span);

    Transcript part;
    if (!run_transcribe_chunk(ctx, chunk, chunk_sec, chunk_start_sec, chunk_index, est_chunks, base,
                              span, language, prompt.empty() ? nullptr : prompt.c_str(), job_start,
                              progress, part)) {
      throw std::runtime_error("transcription failed");
    }

    if (pinned_lang.empty() && !part.language.empty() && part.language != "unknown") {
      pinned_lang = part.language;
      language = pinned_lang.c_str();
    }
    if (result.transcript.language == "unknown" || result.transcript.language.empty()) {
      result.transcript.language = part.language;
    }

    if (!last_chunk && chunk.size() >= kHopSamples) {
      trim_overlap_tail(part, kHopSec);
    }
    offset_transcript(part, chunk_start_sec);
    result.transcript.segments.insert(result.transcript.segments.end(),
                                      std::make_move_iterator(part.segments.begin()),
                                      std::make_move_iterator(part.segments.end()));
    prompt = prompt_tail(result.transcript);

    total_samples_seen = absolute_sample + chunk.size();
    ++chunk_index;

    if (last_chunk || buffer.size() <= kHopSamples) {
      break;
    }

    buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(kHopSamples));
    absolute_sample += kHopSamples;
  }

  progress(93, "collecting transcript");
  result.duration_sec = known_duration > 0
                            ? known_duration
                            : static_cast<double>(total_samples_seen) / static_cast<double>(kSampleRate);
  progress(95, "finalizing");
  return result;
}

AnalysisResult analyze_audio(const std::vector<float>& samples,
                             const std::function<void(int, const std::string&)>& progress) {
  auto infer_lock = whisper::lock_inference();
  whisper_context* ctx = whisper::context();
  if (!ctx) {
    throw std::runtime_error("whisper context not loaded");
  }
  if (samples.empty()) {
    throw std::runtime_error("audio has no samples");
  }
  if (samples.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    throw std::runtime_error("audio is too long for a single transcription pass");
  }

  const double audio_sec = static_cast<double>(samples.size()) / static_cast<double>(kSampleRate);
  char prep[96];
  std::snprintf(prep, sizeof(prep), "preparing transcription (%.1fs audio)", audio_sec);
  progress(10, prep);

  const auto job_start = std::chrono::steady_clock::now();

  // Reuse the same windowing logic over an in-memory buffer.
  std::vector<float> buffer = samples;
  size_t absolute_sample = 0;
  size_t total_samples_seen = 0;
  std::string pinned_lang;
  const char* language = nullptr;
  std::string prompt;
  int chunk_index = 0;

  AnalysisResult result;
  result.sample_rate = kSampleRate;
  result.channels = 1;
  result.transcript.language = "unknown";

  const int progress_lo = 12;
  const int progress_hi = 92;
  const int est_chunks =
      std::max(1, static_cast<int>((audio_sec + kHopSec - 1e-6) / kHopSec));

  while (!buffer.empty()) {
    const bool last_chunk = buffer.size() <= kWindowSamples;
    const size_t chunk_n = std::min(buffer.size(), kWindowSamples);
    std::vector<float> chunk(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(chunk_n));

    const double chunk_start_sec =
        static_cast<double>(absolute_sample) / static_cast<double>(kSampleRate);
    const double chunk_sec = static_cast<double>(chunk.size()) / static_cast<double>(kSampleRate);

    int base = 0;
    int span = 1;
    chunk_progress_range(chunk_index, est_chunks, progress_lo, progress_hi, base, span);

    Transcript part;
    if (!run_transcribe_chunk(ctx, chunk, chunk_sec, chunk_start_sec, chunk_index, est_chunks, base,
                              span, language, prompt.empty() ? nullptr : prompt.c_str(), job_start,
                              progress, part)) {
      throw std::runtime_error("transcription failed");
    }

    if (pinned_lang.empty() && !part.language.empty() && part.language != "unknown") {
      pinned_lang = part.language;
      language = pinned_lang.c_str();
    }
    if (result.transcript.language == "unknown" || result.transcript.language.empty()) {
      result.transcript.language = part.language;
    }

    if (!last_chunk && chunk.size() >= kHopSamples) {
      trim_overlap_tail(part, kHopSec);
    }
    offset_transcript(part, chunk_start_sec);
    result.transcript.segments.insert(result.transcript.segments.end(),
                                      std::make_move_iterator(part.segments.begin()),
                                      std::make_move_iterator(part.segments.end()));
    prompt = prompt_tail(result.transcript);

    total_samples_seen = absolute_sample + chunk.size();
    ++chunk_index;

    if (last_chunk || buffer.size() <= kHopSamples) {
      break;
    }
    buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(kHopSamples));
    absolute_sample += kHopSamples;
  }

  progress(93, "collecting transcript");
  result.duration_sec = audio_sec;
  (void)total_samples_seen;
  progress(95, "finalizing");
  return result;
}

}  // namespace ears
