#include "update/update_state_store.h"

#include <charconv>
#include <fstream>
#include <iterator>
#include <string_view>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace codex_monitor::update {
namespace {

constexpr std::uintmax_t kMaximumStateBytes = 16 * 1024;
constexpr std::size_t kMaximumLineBytes = 256;

bool IsVersionString(std::string_view value) noexcept {
    if (value.empty() || value.size() > 32) return false;
    int dots = 0;
    bool digitInPart = false;
    for (const char character : value) {
        if (character >= '0' && character <= '9') {
            digitInPart = true;
            continue;
        }
        if (character != '.' || !digitInPart || dots >= 2) return false;
        ++dots;
        digitInPart = false;
    }
    return dots == 2 && digitInPart;
}

bool ParseNonNegativeInteger(std::string_view text,
                             std::int64_t& value) noexcept {
    if (text.empty()) return false;
    std::int64_t parsed = 0;
    const auto result = std::from_chars(
        text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} ||
        result.ptr != text.data() + text.size() || parsed < 0) {
        return false;
    }
    value = parsed;
    return true;
}

bool IsValidState(const UpdateCheckState& state) noexcept {
    if (state.lastCheckUnixSeconds < 0) return false;
    return (state.availableVersion.empty() ||
            IsVersionString(state.availableVersion)) &&
           (state.lastNotifiedVersion.empty() ||
            IsVersionString(state.lastNotifiedVersion)) &&
           (state.checkedVersion.empty() ||
            IsVersionString(state.checkedVersion));
}

std::string Serialize(const UpdateCheckState& state) {
    return "version=2\nlast_check=" +
           std::to_string(state.lastCheckUnixSeconds) +
           "\navailable_version=" + state.availableVersion +
           "\nlast_notified_version=" + state.lastNotifiedVersion +
           "\nchecked_version=" + state.checkedVersion + "\n";
}

UpdateStateLoadResult LoadAt(const std::filesystem::path& path) {
    UpdateStateLoadResult result;
    if (path.empty()) {
        result.status = UpdateStateLoadStatus::kIoError;
        result.error = std::make_error_code(std::errc::invalid_argument);
        return result;
    }

    std::error_code existsError;
    const bool exists = std::filesystem::exists(path, existsError);
    if (existsError) {
        result.status = UpdateStateLoadStatus::kIoError;
        result.error = existsError;
        return result;
    }
    if (!exists) {
        result.status = UpdateStateLoadStatus::kNotFound;
        return result;
    }

    std::error_code sizeError;
    const std::uintmax_t size = std::filesystem::file_size(path, sizeError);
    if (sizeError) {
        result.status = UpdateStateLoadStatus::kIoError;
        result.error = sizeError;
        return result;
    }
    if (size == 0 || size > kMaximumStateBytes) {
        result.status = UpdateStateLoadStatus::kMalformed;
        return result;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        result.status = UpdateStateLoadStatus::kIoError;
        result.error = std::make_error_code(std::errc::io_error);
        return result;
    }
    const std::string contents((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    if (input.bad() || contents.find('\0') != std::string::npos) {
        result.status = UpdateStateLoadStatus::kMalformed;
        return result;
    }

    std::unordered_set<std::string> seen;
    bool sawVersion = false;
    int formatVersion = 0;
    bool sawLastCheck = false;
    bool sawAvailable = false;
    bool sawNotified = false;
    bool sawCheckedVersion = false;
    std::size_t begin = 0;
    while (begin < contents.size()) {
        const std::size_t newline = contents.find('\n', begin);
        const std::size_t end =
            newline == std::string::npos ? contents.size() : newline;
        std::string_view line(contents.data() + begin, end - begin);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line.size() > kMaximumLineBytes) {
            result.status = UpdateStateLoadStatus::kMalformed;
            return result;
        }
        const std::size_t separator = line.find('=');
        if (separator == std::string_view::npos || separator == 0) {
            result.status = UpdateStateLoadStatus::kMalformed;
            return result;
        }
        const std::string key(line.substr(0, separator));
        const std::string_view value = line.substr(separator + 1);
        if (!seen.insert(key).second) {
            result.status = UpdateStateLoadStatus::kMalformed;
            return result;
        }
        if (key == "version") {
            sawVersion = true;
            if (value == "1") {
                formatVersion = 1;
            } else if (value == "2") {
                formatVersion = 2;
            } else {
                result.status = UpdateStateLoadStatus::kUnsupportedVersion;
                return result;
            }
        } else if (key == "last_check") {
            sawLastCheck = ParseNonNegativeInteger(
                value, result.state.lastCheckUnixSeconds);
            if (!sawLastCheck) {
                result.status = UpdateStateLoadStatus::kMalformed;
                return result;
            }
        } else if (key == "available_version") {
            sawAvailable = true;
            result.state.availableVersion.assign(value);
        } else if (key == "last_notified_version") {
            sawNotified = true;
            result.state.lastNotifiedVersion.assign(value);
        } else if (key == "checked_version") {
            sawCheckedVersion = true;
            result.state.checkedVersion.assign(value);
        } else {
            result.status = UpdateStateLoadStatus::kMalformed;
            return result;
        }
        if (newline == std::string::npos) break;
        begin = newline + 1;
    }

    const bool expectedFieldsPresent =
        formatVersion == 1
            ? sawVersion && sawLastCheck && sawAvailable && sawNotified &&
                  !sawCheckedVersion && seen.size() == 4
            : formatVersion == 2 && sawVersion && sawLastCheck && sawAvailable &&
                  sawNotified && sawCheckedVersion && seen.size() == 5;
    if (!expectedFieldsPresent || !IsValidState(result.state)) {
        result.status = UpdateStateLoadStatus::kMalformed;
        return result;
    }
    result.status = UpdateStateLoadStatus::kOk;
    return result;
}

std::error_code DefaultAtomicReplace(
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

UpdateStateStore::UpdateStateStore(
    std::filesystem::path path,
    UpdateStateAtomicReplace atomicReplace)
    : path_(std::move(path)), atomicReplace_(std::move(atomicReplace)) {}

UpdateStateLoadResult UpdateStateStore::Load() const noexcept {
    try {
        return LoadAt(path_);
    } catch (...) {
        UpdateStateLoadResult result;
        result.status = UpdateStateLoadStatus::kIoError;
        result.error = std::make_error_code(std::errc::io_error);
        return result;
    }
}

UpdateStateSaveResult UpdateStateStore::Save(
    const UpdateCheckState& state) const noexcept {
    UpdateStateSaveResult result;
    try {
        if (path_.empty() || !IsValidState(state)) {
            result.status = UpdateStateSaveStatus::kInvalidState;
            return result;
        }
        const UpdateStateLoadResult current = LoadAt(path_);
        if (current.status == UpdateStateLoadStatus::kUnsupportedVersion) {
            result.status = UpdateStateSaveStatus::kUnsupportedVersion;
            return result;
        }
        if (current.status == UpdateStateLoadStatus::kIoError) {
            result.status = UpdateStateSaveStatus::kIoError;
            result.error = current.error;
            return result;
        }

        std::error_code directoryError;
        const std::filesystem::path parent = path_.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, directoryError);
        }
        if (directoryError) {
            result.status = UpdateStateSaveStatus::kIoError;
            result.error = directoryError;
            return result;
        }

        std::filesystem::path temporary = path_;
        temporary += L".tmp";
        const std::string contents = Serialize(state);
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) {
                result.status = UpdateStateSaveStatus::kIoError;
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
                result.status = UpdateStateSaveStatus::kIoError;
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
            result.status = UpdateStateSaveStatus::kIoError;
            result.error = replaceError;
            return result;
        }
        result.status = UpdateStateSaveStatus::kWritten;
        return result;
    } catch (...) {
        result.status = UpdateStateSaveStatus::kIoError;
        result.error = std::make_error_code(std::errc::io_error);
        return result;
    }
}

}  // namespace codex_monitor::update
