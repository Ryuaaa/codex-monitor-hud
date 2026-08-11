#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace codex_monitor::update {

inline constexpr std::size_t kMinimumRetainedUpdateDirectories = 2U;
inline constexpr std::uint64_t kMinimumUpdateDirectoryAge100Nanoseconds =
    7ULL * 24ULL * 60ULL * 60ULL * 10'000'000ULL;

struct WindowsUpdateDirectoryCleanupEntry {
    std::wstring name;
    std::uint64_t lastWriteTime100Nanoseconds = 0U;
    bool isDirectory = false;
    bool isReparsePoint = false;
};

// Managed directories have one exact shape: "update-" followed by the
// 32 lowercase hexadecimal characters produced by the CNG random generator.
[[nodiscard]] bool IsManagedWindowsUpdateDirectoryName(
    std::wstring_view name) noexcept;

// A candidate is deletable only when it is an ordinary managed directory,
// at least seven days old, and not one of the two newest valid directories.
[[nodiscard]] std::vector<std::wstring>
SelectStaleWindowsUpdateDirectories(
    const std::vector<WindowsUpdateDirectoryCleanupEntry>& entries,
    std::uint64_t now100Nanoseconds) noexcept;

// Low-cost, fail-closed cleanup. It never follows a reparse point and only
// removes flat, ordinary files from selected managed directories. Every error
// is ignored so cleanup cannot block HUD startup or an update attempt.
void BestEffortCleanupWindowsUpdateDirectories(
    const std::filesystem::path& updatesRoot) noexcept;

}  // namespace codex_monitor::update
