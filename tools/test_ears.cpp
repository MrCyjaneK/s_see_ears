#include "ears.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fstream>
#include <string>

static int wait_job(int (*progress)(long long, int), const char* (*progress_str)(long long),
                    void (*free_fn)(void*), long long job) {
  std::string last;
  for (;;) {
    int p = progress(job, 100);
    const char* msg = progress_str(job);
    std::string cur = msg ? msg : "";
    if (msg) free_fn((void*)msg);
    if (cur != last) {
      fprintf(stderr, "[ears] %3d%% %s\n", p, cur.c_str());
      fflush(stderr);
      last = cur;
    }
    if (p >= 100) {
      return (last == "done") ? 0 : 1;
    }
    usleep(100000);
  }
}

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <audio-or-video> <output.json>\n", argv[0]);
    return 1;
  }
  const char* lib = getenv("EARS_LIB");
  if (!lib) lib = "./dist/libears.so";
  const char* assets = getenv("EARS_ASSETS_DIR");
  if (!assets) {
    fprintf(stderr, "EARS_ASSETS_DIR must point at the whisper model asset directory\n");
    return 1;
  }
  void* h = dlopen(lib, RTLD_NOW);
  if (!h) {
    fprintf(stderr, "dlopen: %s\n", dlerror());
    return 1;
  }
  auto load = (int (*)(const char*, const char*))dlsym(h, "SPACESEE_ears_load");
  auto analyze = (long long (*)(const char*, const char*, const char*))dlsym(h, "SPACESEE_ears_analyze");
  auto progress = (int (*)(long long, int))dlsym(h, "SPACESEE_ears_progress");
  auto progress_str = (const char* (*)(long long))dlsym(h, "SPACESEE_ears_progressString");
  auto free_fn = (void (*)(void*))dlsym(h, "SPACESEE_ears_free");
  if (!load || !analyze || !progress || !progress_str || !free_fn) {
    fprintf(stderr, "missing symbols\n");
    return 1;
  }
  if (load("/tmp/ears-work", assets) != 0) {
    fprintf(stderr, "load failed\n");
    return 1;
  }
  long long job = analyze(argv[1], argv[2], "");
  if (wait_job(progress, progress_str, free_fn, job) != 0) {
    const char* msg = progress_str(job);
    fprintf(stderr, "analyze failed: %s\n", msg ? msg : "unknown");
    if (msg) free_fn((void*)msg);
    return 1;
  }
  std::ifstream in(argv[2]);
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  printf("%s\n", content.c_str());
  return 0;
}
