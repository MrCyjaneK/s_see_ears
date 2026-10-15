#include "whisper_ctx.h"

#include <filesystem>
#include <stdexcept>

#include "whisper.h"

namespace fs = std::filesystem;

namespace ears::whisper {

namespace {

std::mutex load_mu;
std::mutex infer_mu;
whisper_context* ctx = nullptr;

}  // namespace

void init(const std::string& assets_dir) {
  std::lock_guard<std::mutex> lock(load_mu);
  if (ctx) return;

  const fs::path model_path = fs::path(assets_dir) / "ggml-medium.bin";
  if (!fs::is_regular_file(model_path)) {
    throw std::runtime_error("ggml-medium.bin not found in assets directory");
  }

  whisper_context_params cparams = whisper_context_default_params();
  cparams.use_gpu = false;
  cparams.flash_attn = false;
  // Weights only; compute/KV live in a per-chunk state we free after each pass.
  ctx = whisper_init_from_file_with_params_no_state(model_path.string().c_str(), cparams);
  if (!ctx) {
    throw std::runtime_error("failed to load whisper model");
  }
}

void shutdown() {
  // Wait for any in-flight inference before freeing the context.
  std::lock_guard<std::mutex> infer(infer_mu);
  std::lock_guard<std::mutex> lock(load_mu);
  if (ctx) {
    whisper_free(ctx);
    ctx = nullptr;
  }
}

whisper_context* context() {
  std::lock_guard<std::mutex> lock(load_mu);
  return ctx;
}

std::unique_lock<std::mutex> lock_inference() { return std::unique_lock<std::mutex>(infer_mu); }

}  // namespace ears::whisper
