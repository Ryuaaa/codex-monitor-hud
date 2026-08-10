#pragma once

#include "module_state.h"

#include <filesystem>

namespace codex_monitor {

std::filesystem::path DefaultSettingsPath();
SettingsState LoadSettingsFile(const std::filesystem::path& path);
bool SaveSettingsFile(const std::filesystem::path& path, const SettingsState& settings);

}  // namespace codex_monitor
