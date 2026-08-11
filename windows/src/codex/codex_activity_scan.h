#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace codex_monitor::codex {

inline constexpr std::int64_t kCodexActivityWindowSeconds = 120;
inline constexpr std::uint64_t kCodexActivityMaximumTailBytes =
    1024ULL * 1024ULL;
inline constexpr std::size_t kCodexActivityMaximumCandidateFiles = 64;

enum class CodexActivityLineDisposition {
    kIgnored,
    kStarted,
    kFinished,
    kMalformed,
};

struct CodexActivityLineResult {
    CodexActivityLineDisposition disposition =
        CodexActivityLineDisposition::kIgnored;
    std::int64_t timestampUnixMilliseconds = 0;
};

// Parses only timestamp, type, payload.type, payload.role and payload.phase.
// Every other JSON value is structurally skipped without retaining its text.
[[nodiscard]] CodexActivityLineResult ParseCodexActivityJsonlLine(
    std::string_view line) noexcept;

struct CodexActivityFileState {
    bool active = false;
    std::int64_t startedAtUnixMilliseconds = 0;
    std::int64_t finishedAtUnixMilliseconds = 0;
    std::int64_t durationSeconds = 0;
    std::size_t malformedLineCount = 0;
};

// Pure inference used by fixed tests and by the bounded filesystem scanner.
[[nodiscard]] CodexActivityFileState InferCodexActivityFileState(
    const std::vector<std::string>& jsonLines,
    std::int64_t modifiedAtUnixMilliseconds,
    std::int64_t nowUnixMilliseconds) noexcept;

enum class CodexActivityScanStatus {
    kAvailable,
    kInvalidArgument,
    kUnsafeRoot,
    kRootNotFound,
    kRootNotDirectory,
    kRecentFilesUnresolved,
    kIoError,
    kCancelled,
};

struct CodexActivityScanRequest {
    // The sessions directory itself, normally %USERPROFILE%\.codex\sessions.
    std::filesystem::path sessionsRoot;
    std::int64_t nowUnixSeconds = 0;
    std::function<bool()> shouldCancel;
};

struct CodexActivityScanResult {
    CodexActivityScanStatus status = CodexActivityScanStatus::kIoError;
    std::size_t activeTaskCount = 0;
    std::int64_t longestActiveTaskSeconds = 0;
    std::size_t readableRecentFileCount = 0;
    std::size_t unresolvedRecentFileCount = 0;
    std::size_t skippedCompressedFileCount = 0;
    std::size_t malformedLineCount = 0;
    std::uint64_t bytesRead = 0;
    bool partial = false;
    std::error_code error;

    [[nodiscard]] bool available() const noexcept {
        return status == CodexActivityScanStatus::kAvailable;
    }
};

// Reads at most the newest 64 recent candidates and at most the final 1 MiB
// from each ordinary JSONL file. Paths and line contents never leave this call.
// Recent compressed or unreadable files produce partial/unavailable output so
// their absence is never reported as a reliable zero.
[[nodiscard]] CodexActivityScanResult ScanRecentCodexActivity(
    const CodexActivityScanRequest& request) noexcept;

}  // namespace codex_monitor::codex
