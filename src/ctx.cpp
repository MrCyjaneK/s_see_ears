#include "ctx.h"

#include "whisper_ctx.h"

#include <filesystem>
#include <mutex>

namespace fs = std::filesystem;

namespace ears {

namespace {

std::mutex load_mu;
bool loaded = false;
std::string load_error;
std::string models_path;
std::string work_path;

}  // namespace

bool ensure_loaded(const std::string& work_dir, const std::string& assets_dir, std::string& err) {
  std::lock_guard<std::mutex> lock(load_mu);
  if (loaded) {
    work_path = work_dir;
    return true;
  }
  if (work_dir.empty()) {
    err = "work dir is empty";
    load_error = err;
    return false;
  }
  if (assets_dir.empty()) {
    err = "assets dir is empty";
    load_error = err;
    return false;
  }
  std::error_code ec;
  if (!fs::is_directory(assets_dir, ec)) {
    err = "assets directory not found";
    load_error = err;
    return false;
  }
  try {
    whisper::init(assets_dir);
    loaded = true;
    models_path = assets_dir;
    work_path = work_dir;
    load_error.clear();
    return true;
  } catch (const std::exception& ex) {
    err = ex.what();
    load_error = err;
    return false;
  }
}

void unload() {
  std::lock_guard<std::mutex> lock(load_mu);
  if (!loaded) return;
  whisper::shutdown();
  loaded = false;
  models_path.clear();
  work_path.clear();
  load_error.clear();
}

bool is_loaded() {
  std::lock_guard<std::mutex> lock(load_mu);
  return loaded;
}

const std::string& models_dir() { return models_path; }
const std::string& work_dir() { return work_path; }

}  // namespace ears
