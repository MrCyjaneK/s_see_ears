#pragma once

#include <functional>
#include <mutex>
#include <string>

struct whisper_context;

namespace ears::whisper {

void init(const std::string& assets_dir);
void shutdown();
whisper_context* context();

// Serialize whisper_full / unload against concurrent analyze jobs.
std::unique_lock<std::mutex> lock_inference();

}  // namespace ears::whisper
