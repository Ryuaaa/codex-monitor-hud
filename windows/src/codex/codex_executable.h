#pragma once

#include <filesystem>
#include <optional>

namespace codex_monitor::codex {

// Returns an absolute, regular, non-reparse-point codex.exe. CODEX_CLI_PATH is
// checked first; PATH is parsed manually and relative/empty entries are ignored.
std::optional<std::filesystem::path> FindCodexExecutable();

bool IsSafeCodexExecutable(const std::filesystem::path& candidate);

}  // namespace codex_monitor::codex
