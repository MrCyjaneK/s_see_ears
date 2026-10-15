#pragma once

#include <string>

namespace ears {

bool ensure_loaded(const std::string& work_dir, const std::string& assets_dir, std::string& err);
void unload();
bool is_loaded();
const std::string& models_dir();
const std::string& work_dir();

}  // namespace ears
