#include "codex/weekly_quota_alert_state_store.h"

#include <atomic>
#include <charconv>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace codex_monitor::codex {
namespace {

constexpr std::uintmax_t kMaximumStateBytes = 1024;
constexpr std::size_t kMaximumLineBytes = 128;
constexpr std::int64_t kMaximumUnixSeconds = 253402300799LL;
std::atomic<std::uint64_t> gTemporarySequence{1};

[[nodiscard]] std::string_view ModeName(WeeklyQuotaAlertMode mode) noexcept {
    return mode == WeeklyQuotaAlertMode::kRolling24Hours
               ? "rolling24h"
               : "naturalDay";
}

[[nodiscard]] bool ParseMode(std::string_view text,
                             WeeklyQuotaAlertMode& mode) noexcept {
    if (text == "naturalDay") {
        mode = WeeklyQuotaAlertMode::kNaturalDay;
        return true;
    }
    if (text == "rolling24h") {
        mode = WeeklyQuotaAlertMode::kRolling24Hours;
        return true;
    }
    return false;
}

[[nodiscard]] bool ParseOptionalTimestamp(
    std::string_view text,
    std::optional<std::int64_t>& value) noexcept {
    if (text == "-") {
        value.reset();
        return true;
    }
    if (text.empty()) return false;
    std::int64_t parsed = 0;
    const auto result = std::from_chars(
        text.data(), text.data() + text.size(), parsed, 10);
    if (result.ec != std::errc{} ||
        result.ptr != text.data() + text.size() || parsed < 0 ||
        parsed > kMaximumUnixSeconds) {
        return false;
    }
    value = parsed;
    return true;
}

void AppendOptionalTimestamp(std::string& output,
                             const std::optional<std::int64_t>& value) {
    output += value ? std::to_string(*value) : "-";
    output.push_back('\n');
}

[[nodiscard]] std::string Serialize(const WeeklyQuotaAlertState& state) {
    std::string output = "version=1\nweekly_reset_at=";
    AppendOptionalTimestamp(output, state.weeklyResetAtUnixSeconds);
    output += "mode=";
    output += ModeName(state.mode);
    output += "\nlast_evaluated_at=";
    AppendOptionalTimestamp(output, state.lastEvaluatedAtUnixSeconds);
    output += "last_notified_period_start=";
    AppendOptionalTimestamp(output,
                            state.lastNotifiedPeriodStartUnixSeconds);
    output += "last_notified_at=";
    AppendOptionalTimestamp(output, state.lastNotifiedAtUnixSeconds);
    return output;
}

[[nodiscard]] WeeklyQuotaAlertStateLoadResult LoadAt(
    const std::filesystem::path& path) {
    WeeklyQuotaAlertStateLoadResult result;
    if (path.empty()) {
        result.status = WeeklyQuotaAlertStateLoadStatus::kIoError;
        result.error = std::make_error_code(std::errc::invalid_argument);
        return result;
    }

    std::error_code existsError;
    const bool exists = std::filesystem::exists(path, existsError);
    if (existsError) {
        result.status = WeeklyQuotaAlertStateLoadStatus::kIoError;
        result.error = existsError;
        return result;
    }
    if (!exists) {
        result.status = WeeklyQuotaAlertStateLoadStatus::kNotFound;
        return result;
    }

    std::error_code sizeError;
    const std::uintmax_t size = std::filesystem::file_size(path, sizeError);
    if (sizeError) {
        result.status = WeeklyQuotaAlertStateLoadStatus::kIoError;
        result.error = sizeError;
        return result;
    }
    if (size == 0 || size > kMaximumStateBytes) {
        result.status = WeeklyQuotaAlertStateLoadStatus::kMalformed;
        return result;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        result.status = WeeklyQuotaAlertStateLoadStatus::kIoError;
        result.error = std::make_error_code(std::errc::io_error);
        return result;
    }
    const std::string contents((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    if (input.bad() || contents.empty() || contents.back() != '\n' ||
        contents.find('\0') != std::string::npos) {
        result.status = WeeklyQuotaAlertStateLoadStatus::kMalformed;
        return result;
    }

    bool sawVersion = false;
    bool sawReset = false;
    bool sawMode = false;
    bool sawLastEvaluation = false;
    bool sawNotifiedPeriod = false;
    bool sawNotifiedAt = false;
    WeeklyQuotaAlertState parsedState;
    std::unordered_set<std::string> seen;
    std::size_t begin = 0;
    while (begin < contents.size()) {
        const std::size_t newline = contents.find('\n', begin);
        if (newline == std::string::npos) {
            result.status = WeeklyQuotaAlertStateLoadStatus::kMalformed;
            return result;
        }
        std::string_view line(contents.data() + begin, newline - begin);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) {
            if (newline + 1 == contents.size()) break;
            result.status = WeeklyQuotaAlertStateLoadStatus::kMalformed;
            return result;
        }
        if (line.size() > kMaximumLineBytes) {
            result.status = WeeklyQuotaAlertStateLoadStatus::kMalformed;
            return result;
        }
        const std::size_t separator = line.find('=');
        if (separator == std::string_view::npos || separator == 0) {
            result.status = WeeklyQuotaAlertStateLoadStatus::kMalformed;
            return result;
        }
        const std::string key(line.substr(0, separator));
        const std::string_view value = line.substr(separator + 1);
        if (!seen.insert(key).second) {
            result.status = WeeklyQuotaAlertStateLoadStatus::kMalformed;
            return result;
        }

        bool parsed = true;
        if (key == "version") {
            sawVersion = true;
            if (value != "1") {
                result.status =
                    WeeklyQuotaAlertStateLoadStatus::kUnsupportedVersion;
                return result;
            }
        } else if (key == "weekly_reset_at") {
            sawReset = true;
            parsed = ParseOptionalTimestamp(
                value, parsedState.weeklyResetAtUnixSeconds);
        } else if (key == "mode") {
            sawMode = true;
            parsed = ParseMode(value, parsedState.mode);
        } else if (key == "last_evaluated_at") {
            sawLastEvaluation = true;
            parsed = ParseOptionalTimestamp(
                value, parsedState.lastEvaluatedAtUnixSeconds);
        } else if (key == "last_notified_period_start") {
            sawNotifiedPeriod = true;
            parsed = ParseOptionalTimestamp(
                value, parsedState.lastNotifiedPeriodStartUnixSeconds);
        } else if (key == "last_notified_at") {
            sawNotifiedAt = true;
            parsed = ParseOptionalTimestamp(
                value, parsedState.lastNotifiedAtUnixSeconds);
        } else {
            result.status = WeeklyQuotaAlertStateLoadStatus::kMalformed;
            return result;
        }
        if (!parsed) {
            result.status = WeeklyQuotaAlertStateLoadStatus::kMalformed;
            return result;
        }
        begin = newline + 1;
    }

    if (!sawVersion || !sawReset || !sawMode || !sawLastEvaluation ||
        !sawNotifiedPeriod || !sawNotifiedAt || seen.size() != 6 ||
        !IsValidWeeklyQuotaAlertState(parsedState)) {
        result.status = WeeklyQuotaAlertStateLoadStatus::kMalformed;
        return result;
    }
    result.state = parsedState;
    result.status = WeeklyQuotaAlertStateLoadStatus::kOk;
    return result;
}

[[nodiscard]] std::filesystem::path TemporaryPathFor(
    const std::filesystem::path& path) {
    std::filesystem::path temporary = path;
    temporary += ".tmp." +
                 std::to_string(gTemporarySequence.fetch_add(1));
    return temporary;
}

[[nodiscard]] std::error_code DefaultAtomicReplace(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination) {
#ifdef _WIN32
    if (MoveFileExW(temporary.c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return {};
    }
    return std::error_code(static_cast<int>(GetLastError()),
                           std::system_category());
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    return error;
#endif
}

}  // namespace

WeeklyQuotaAlertStateStore::WeeklyQuotaAlertStateStore(
    std::filesystem::path path,
    WeeklyQuotaAlertStateAtomicReplace atomicReplace)
    : path_(std::move(path)), atomicReplace_(std::move(atomicReplace)) {}

WeeklyQuotaAlertStateLoadResult WeeklyQuotaAlertStateStore::Load()
    const noexcept {
    try {
        return LoadAt(path_);
    } catch (...) {
        WeeklyQuotaAlertStateLoadResult result;
        result.status = WeeklyQuotaAlertStateLoadStatus::kIoError;
        result.error = std::make_error_code(std::errc::io_error);
        return result;
    }
}

WeeklyQuotaAlertStateSaveResult WeeklyQuotaAlertStateStore::Save(
    const WeeklyQuotaAlertState& state) const noexcept {
    WeeklyQuotaAlertStateSaveResult result;
    try {
        if (path_.empty() || !IsValidWeeklyQuotaAlertState(state)) {
            result.status = WeeklyQuotaAlertStateSaveStatus::kInvalidState;
            return result;
        }
        const WeeklyQuotaAlertStateLoadResult current = LoadAt(path_);
        if (current.status ==
            WeeklyQuotaAlertStateLoadStatus::kUnsupportedVersion) {
            result.status =
                WeeklyQuotaAlertStateSaveStatus::kUnsupportedVersion;
            return result;
        }
        if (current.status == WeeklyQuotaAlertStateLoadStatus::kIoError) {
            result.status = WeeklyQuotaAlertStateSaveStatus::kIoError;
            result.error = current.error;
            return result;
        }

        std::error_code directoryError;
        const std::filesystem::path parent = path_.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, directoryError);
        }
        if (directoryError) {
            result.status = WeeklyQuotaAlertStateSaveStatus::kIoError;
            result.error = directoryError;
            return result;
        }

        const std::string contents = Serialize(state);
        if (contents.empty() || contents.size() > kMaximumStateBytes) {
            result.status = WeeklyQuotaAlertStateSaveStatus::kInvalidState;
            return result;
        }
        const std::filesystem::path temporary = TemporaryPathFor(path_);
        {
            std::ofstream output(temporary,
                                 std::ios::binary | std::ios::trunc);
            if (!output) {
                result.status = WeeklyQuotaAlertStateSaveStatus::kIoError;
                result.error = std::make_error_code(std::errc::io_error);
                return result;
            }
            output.write(contents.data(),
                         static_cast<std::streamsize>(contents.size()));
            output.flush();
            output.close();
            if (!output) {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                result.status = WeeklyQuotaAlertStateSaveStatus::kIoError;
                result.error = std::make_error_code(std::errc::io_error);
                return result;
            }
        }

        const std::error_code replaceError =
            atomicReplace_ ? atomicReplace_(temporary, path_)
                           : DefaultAtomicReplace(temporary, path_);
        if (replaceError) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            result.status = WeeklyQuotaAlertStateSaveStatus::kIoError;
            result.error = replaceError;
            return result;
        }
        result.status = WeeklyQuotaAlertStateSaveStatus::kWritten;
        return result;
    } catch (...) {
        result.status = WeeklyQuotaAlertStateSaveStatus::kIoError;
        result.error = std::make_error_code(std::errc::io_error);
        return result;
    }
}

}  // namespace codex_monitor::codex
