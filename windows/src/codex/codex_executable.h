#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>

namespace codex_monitor::codex {

inline constexpr std::size_t kMaximumCodexExecutableCandidates = 64;
inline constexpr std::size_t kMaximumDirectPathDirectories = 32;
static_assert(kMaximumDirectPathDirectories <
              kMaximumCodexExecutableCandidates);

// Returns an absolute, regular, non-reparse-point codex.exe. CODEX_CLI_PATH is
// checked first; direct PATH entries remain second. A fixed PATH sub-budget
// preserves room for known user-level layouts before optional npm-prefix
// expansion, all without recursive traversal, scripts, shells, or WindowsApps.
// Discovery inspects at most kMaximumCodexExecutableCandidates paths; versioned
// groups select the safe candidate with the newest last-write time.
std::optional<std::filesystem::path> FindCodexExecutable();

bool IsSafeCodexExecutable(const std::filesystem::path& candidate);

}  // namespace codex_monitor::codex
