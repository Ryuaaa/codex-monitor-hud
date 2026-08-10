#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <system_error>
#include <vector>

namespace codex_monitor::codex {

struct QuotaHistoryWindowSample {
    std::optional<double> remainingPercent;
    std::optional<std::int64_t> resetsAtUnixSeconds;
};

struct QuotaHistorySample {
    std::int64_t capturedAtUnixSeconds = 0;
    QuotaHistoryWindowSample fiveHour;
    QuotaHistoryWindowSample weekly;
};

enum class QuotaHistoryLoadStatus {
    kOk,
    kNotFound,
    kInvalidNow,
    kUnsupportedVersion,
    kIoError,
};

struct QuotaHistoryLoadResult {
    QuotaHistoryLoadStatus status = QuotaHistoryLoadStatus::kIoError;
    std::vector<QuotaHistorySample> samples;
    std::size_t skippedMalformedLines = 0;
    std::size_t discardedSamples = 0;
    std::error_code error;

    [[nodiscard]] bool ok() const noexcept {
        return status == QuotaHistoryLoadStatus::kOk ||
               status == QuotaHistoryLoadStatus::kNotFound;
    }
};

enum class QuotaHistoryUpdateStatus {
    kWritten,
    kSkippedTooSoon,
    kInvalidSample,
    kUnsupportedVersion,
    kIoError,
};

struct QuotaHistoryUpdateResult {
    QuotaHistoryUpdateStatus status = QuotaHistoryUpdateStatus::kIoError;
    std::size_t storedSampleCount = 0;
    std::error_code error;

    [[nodiscard]] bool written() const noexcept {
        return status == QuotaHistoryUpdateStatus::kWritten;
    }
};

// The callback exists so callers and tests can supply a platform-specific
// atomic replacement primitive without making this storage layer depend on
// Windows headers. An empty error code means the replacement succeeded.
using QuotaHistoryAtomicReplace = std::function<std::error_code(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination)>;

class QuotaHistoryStore {
public:
    explicit QuotaHistoryStore(
        std::filesystem::path path,
        QuotaHistoryAtomicReplace atomicReplace = {});

    // Load filters the file to the inclusive [now - 30 days, now] window and
    // returns at most the newest 10,000 samples in chronological order.
    [[nodiscard]] QuotaHistoryLoadResult Load(
        std::int64_t nowUnixSeconds) const noexcept;
    [[nodiscard]] QuotaHistoryLoadResult Load() const noexcept;

    // Update rewrites the retained history through a same-directory temporary
    // file. It rejects out-of-order samples and skips a sample less than 60
    // seconds after the newest retained sample in this file.
    [[nodiscard]] QuotaHistoryUpdateResult Update(
        const QuotaHistorySample& sample) const noexcept;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
    QuotaHistoryAtomicReplace atomicReplace_;
};

}  // namespace codex_monitor::codex
