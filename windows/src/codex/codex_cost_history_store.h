#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

#include "codex/codex_cost_model.h"

namespace codex_monitor::codex {

inline constexpr std::uintmax_t kCodexCostHistoryCacheMaximumBytes =
    8ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kCodexCostHistoryCacheMaximumLineBytes = 4096;
inline constexpr std::size_t kCodexCostHistoryCacheMaximumFiles = 4096;
inline constexpr std::size_t kCodexCostHistoryCacheMaximumRows = 32768;

// This deliberately mirrors only the incremental state needed after a
// restart. It cannot carry a source path, raw JSON, task/account data, or a
// line fingerprint.
struct CodexCostHistoryParserSnapshot {
    std::string currentModel = "unknown";
    bool baselinePending = false;
    bool hasRawTotalsWatermark = false;
    CodexTokenUsage rawTotalsWatermark;
};

struct CodexCostHistoryRowSnapshot {
    std::string localDate;
    std::string model;
    CodexTokenUsage usage;
    double cachedEstimatedUsd = 0.0;
    std::int64_t cachedPricedTokens = 0;
};

struct CodexCostHistoryFileSnapshot {
    // fileId is a privacy-trimmed filesystem identity emitted by the scanner,
    // never a source path.
    std::string fileId;
    std::uint64_t observedSizeBytes = 0;
    std::int64_t modifiedUnixNanoseconds = 0;
    std::uint64_t parsedOffsetBytes = 0;
    bool discardingOversizedLine = false;
    bool hasSkippedOversizedLine = false;
    bool complete = false;
    CodexCostHistoryParserSnapshot parser;
    std::vector<CodexCostHistoryRowSnapshot> rows;
};

struct CodexCostHistorySnapshot {
    std::int64_t trackingStartedAtUnixSeconds = 0;
    std::int64_t updatedAtUnixSeconds = 0;
    std::vector<CodexCostHistoryFileSnapshot> files;
};

enum class CodexCostHistoryLoadStatus {
    kOk,
    kNotFound,
    kUnsupportedVersion,
    kCorrupt,
    kTooLarge,
    kIoError,
};

struct CodexCostHistoryLoadResult {
    CodexCostHistoryLoadStatus status = CodexCostHistoryLoadStatus::kIoError;
    CodexCostHistorySnapshot snapshot;
    std::error_code error;

    [[nodiscard]] bool ok() const noexcept {
        return status == CodexCostHistoryLoadStatus::kOk ||
               status == CodexCostHistoryLoadStatus::kNotFound;
    }
};

enum class CodexCostHistorySaveStatus {
    kWritten,
    kInvalidSnapshot,
    kTooLarge,
    kUnsupportedVersion,
    kCancelled,
    kIoError,
};

struct CodexCostHistorySaveResult {
    CodexCostHistorySaveStatus status = CodexCostHistorySaveStatus::kIoError;
    std::size_t storedFileCount = 0;
    std::size_t storedRowCount = 0;
    std::error_code error;

    [[nodiscard]] bool written() const noexcept {
        return status == CodexCostHistorySaveStatus::kWritten;
    }
};

// The default uses the platform's atomic replacement primitive. Keeping it
// injectable lets the worker add its visibility/epoch gate and lets tests
// prove that a failed or cancelled replacement preserves the old file.
using CodexCostHistoryAtomicReplace = std::function<std::error_code(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination)>;

class CodexCostHistoryStore {
public:
    explicit CodexCostHistoryStore(
        std::filesystem::path path,
        CodexCostHistoryAtomicReplace atomicReplace = {});

    [[nodiscard]] CodexCostHistoryLoadResult Load() const noexcept;

    // Save refuses to overwrite a destination carrying an unknown version.
    // Current-version corruption may be repaired only by replacing it with a
    // fully validated snapshot through the atomic callback. The application
    // is a single writer; a caller that permits concurrent processes must
    // serialize the final version check and replacement inside its callback.
    [[nodiscard]] CodexCostHistorySaveResult Save(
        const CodexCostHistorySnapshot& snapshot) const noexcept;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
    CodexCostHistoryAtomicReplace atomicReplace_;
};

}  // namespace codex_monitor::codex
