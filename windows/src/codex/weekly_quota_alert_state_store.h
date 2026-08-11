#pragma once

#include "codex/weekly_quota_alert.h"

#include <filesystem>
#include <functional>
#include <system_error>

namespace codex_monitor::codex {

enum class WeeklyQuotaAlertStateLoadStatus {
    kOk,
    kNotFound,
    kMalformed,
    kUnsupportedVersion,
    kIoError,
};

struct WeeklyQuotaAlertStateLoadResult {
    WeeklyQuotaAlertStateLoadStatus status =
        WeeklyQuotaAlertStateLoadStatus::kIoError;
    WeeklyQuotaAlertState state;
    std::error_code error;

    [[nodiscard]] bool ok() const noexcept {
        return status == WeeklyQuotaAlertStateLoadStatus::kOk ||
               status == WeeklyQuotaAlertStateLoadStatus::kNotFound;
    }
};

enum class WeeklyQuotaAlertStateSaveStatus {
    kWritten,
    kInvalidState,
    kUnsupportedVersion,
    kIoError,
};

struct WeeklyQuotaAlertStateSaveResult {
    WeeklyQuotaAlertStateSaveStatus status =
        WeeklyQuotaAlertStateSaveStatus::kIoError;
    std::error_code error;

    [[nodiscard]] bool written() const noexcept {
        return status == WeeklyQuotaAlertStateSaveStatus::kWritten;
    }
};

using WeeklyQuotaAlertStateAtomicReplace = std::function<std::error_code(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination)>;

class WeeklyQuotaAlertStateStore {
public:
    explicit WeeklyQuotaAlertStateStore(
        std::filesystem::path path,
        WeeklyQuotaAlertStateAtomicReplace atomicReplace = {});

    [[nodiscard]] WeeklyQuotaAlertStateLoadResult Load() const noexcept;
    [[nodiscard]] WeeklyQuotaAlertStateSaveResult Save(
        const WeeklyQuotaAlertState& state) const noexcept;

private:
    std::filesystem::path path_;
    WeeklyQuotaAlertStateAtomicReplace atomicReplace_;
};

}  // namespace codex_monitor::codex
