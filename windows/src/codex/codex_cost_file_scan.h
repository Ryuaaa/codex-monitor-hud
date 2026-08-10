#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace codex_monitor::codex {

inline constexpr std::size_t kCodexCostHistoryDays = 30;
inline constexpr std::uint64_t kCodexCostMaximumScanBytes =
    64ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kCodexCostMaximumLineBytes = 512ULL * 1024ULL;

enum class CodexCostFileScanStatus {
    kOk,
    kInvalidArgument,
    kUnsafeRoot,
    kRootNotFound,
    kRootNotDirectory,
    kIoError,
};

// This is the only per-file state a caller needs to persist. The identifier is
// derived from the filesystem identity, not the user's full path.
struct CodexCostFileCursor {
    std::string fileId;
    std::uint64_t observedSizeBytes = 0;
    std::int64_t modifiedUnixNanoseconds = 0;
    std::uint64_t parsedOffsetBytes = 0;
    bool discardingOversizedLine = false;
    bool hasSkippedOversizedLine = false;
    bool complete = false;
    bool resetAfterTruncation = false;
};

struct CodexCostFileLine {
    std::string fileId;
    std::uint64_t beginOffsetBytes = 0;
    std::uint64_t endOffsetBytes = 0;
    std::string text;
};

struct CodexCostFileScanRequest {
    std::filesystem::path codexHome;
    std::int64_t nowUnixSeconds = 0;
    std::vector<CodexCostFileCursor> previousFiles;

    // Values above the product limits are clamped. Smaller values exist so
    // callers can deliberately reduce work and tests can exercise boundaries.
    std::uint64_t byteBudgetBytes = kCodexCostMaximumScanBytes;
    std::size_t maximumLineBytes = kCodexCostMaximumLineBytes;
};

struct CodexCostFileScanResult {
    CodexCostFileScanStatus status = CodexCostFileScanStatus::kIoError;
    std::vector<CodexCostFileCursor> files;
    std::vector<CodexCostFileLine> lines;
    std::uint64_t bytesRead = 0;
    std::size_t skippedCompressedFiles = 0;
    std::size_t skippedOversizedLines = 0;
    std::size_t rejectedUnsafeEntries = 0;
    std::size_t ioErrorCount = 0;
    std::size_t ignoredExpiredArchivedFiles = 0;
    bool budgetExhausted = false;
    bool coverageIncomplete = false;
    std::error_code error;

    [[nodiscard]] bool ok() const noexcept {
        return status == CodexCostFileScanStatus::kOk;
    }
};

// Pure syntax check used before any Windows filesystem access. It accepts a
// drive-absolute local path such as C:\\Users\\name\\.codex and rejects UNC,
// device, root-relative, drive-relative, and embedded-NUL paths. Whether a
// drive is mapped to a network share is checked separately during scanning.
[[nodiscard]] bool IsSafeAbsoluteWindowsLocalPath(
    std::wstring_view path) noexcept;

// Returns relative YYYY/MM/DD paths for the current local calendar day and the
// preceding days. This keeps date selection independently testable.
[[nodiscard]] std::vector<std::filesystem::path>
RecentLocalCodexSessionDatePaths(std::int64_t nowUnixSeconds,
                                 std::size_t dayCount =
                                     kCodexCostHistoryDays) noexcept;

// Scans only:
//   sessions/YYYY/MM/DD/rollout-*.jsonl
//   archived_sessions/*.jsonl
// Archived files are retained by modification time for 30 * 24 hours. The
// returned value never contains the raw full source path. Unterminated trailing
// lines are left for a later pass so a concurrently written JSON object is not
// emitted early.
[[nodiscard]] CodexCostFileScanResult ScanCodexCostRolloutFiles(
    const CodexCostFileScanRequest& request) noexcept;

}  // namespace codex_monitor::codex
