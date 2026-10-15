#include "assets.h"
#include "ctx.h"
#include "jobs.h"
#include "output_json.h"
#include "pipeline.h"

#include "ears.h"

#include <cstring>
#include <fstream>
#include <string>

namespace {

std::string load_error;

char* dup_cstr(const std::string& s) { return strdup(s.c_str()); }

}  // namespace

extern "C" int SPACESEE_ears_load(const char* workDir, const char* assetsDir) {
  if (!workDir || !workDir[0]) {
    load_error = "work dir is empty";
    return 1;
  }
  if (!assetsDir || !assetsDir[0]) {
    load_error = "assets dir is empty";
    return 1;
  }
  std::string err;
  if (!ears::ensure_loaded(workDir, assetsDir, err)) {
    load_error = err;
    return 1;
  }
  load_error.clear();
  return 0;
}

extern "C" const char* SPACESEE_ears_loadError(void) {
  if (load_error.empty()) return nullptr;
  return dup_cstr(load_error);
}

extern "C" void SPACESEE_ears_unload(void) { ears::unload(); }

extern "C" int SPACESEE_ears_isHeavy(void) { return 1; }

extern "C" int SPACESEE_ears_assetCount(void) { return ears::assets::count(); }

extern "C" const char* SPACESEE_ears_asset(int index) {
  std::string s = ears::assets::describe(index);
  if (s.empty()) return nullptr;
  return dup_cstr(s);
}

extern "C" const char* SPACESEE_ears_version(void) { return dup_cstr("1.0.0"); }

extern "C" const char* SPACESEE_ears_extension(void) { return dup_cstr("json"); }

extern "C" const char* SPACESEE_ears_filetypes(void) {
  return dup_cstr(
      ".wav,.mp3,.m4a,.aac,.flac,.ogg,.opus,.wma,.aiff,.aif,.ape,.wv,"
      ".mp4,.mov,.mkv,.webm,.avi,.m4v,.mpeg,.mpg,.3gp,.ts,.wmv,.flv");
}

extern "C" long long SPACESEE_ears_maxInputSize(void) { return 16LL * 1024 * 1024 * 1024; }

namespace ears::jobs {
void run_analyze(JobId id, std::string input, std::string output) {
  try {
    auto result = analyze_file(input, [&](int p, const std::string& msg) { update_progress(id, p, msg); });

    update_progress(id, 98, "writing output");
    std::string json = to_json(result);
    std::ofstream out(output);
    if (!out) {
      fail_job(id, "failed to write output");
      return;
    }
    out << json;
    finish_job(id);
  } catch (const std::exception& ex) {
    fail_job(id, ex.what());
  }
}
}  // namespace ears::jobs

extern "C" long long SPACESEE_ears_analyze(const char* inputPath, const char* outputPath, const char* dataDir) {
  (void)dataDir;
  auto id = ears::jobs::new_job_id();
  ears::jobs::start_analyze(id, inputPath ? inputPath : "", outputPath ? outputPath : "");
  return static_cast<long long>(id);
}

extern "C" int SPACESEE_ears_progress(long long jobId, int scale) {
  return ears::jobs::progress(jobId, scale);
}

extern "C" const char* SPACESEE_ears_progressString(long long jobId) {
  return dup_cstr(ears::jobs::progress_string(jobId));
}

extern "C" void SPACESEE_ears_free(void* ptr) { free(ptr); }
