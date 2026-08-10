#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>

namespace codex_monitor::update {

struct UpdateCheckState {
    std::int64_t lastCheckUnixSeconds = 0;
    std::string availableVersion;
    std::string lastNotifiedVersion;
    std::string checkedVersion;
};

enum class UpdateStateLoadStatus {
    kOk,
    kNotFound,
    kUnsupportedVersion,
    kMalformed,
    kIoError,
};

struct UpdateStateLoadResult {
    UpdateStateLoadStatus status = UpdateStateLoadStatus::kIoError;
    UpdateCheckState state;
    std::error_code error;

    [[nodiscard]] bool ok() const noexcept {
        return status == UpdateStateLoadStatus::kOk ||
               status == UpdateStateLoadStatus::kNotFound;
    }
};

enum class UpdateStateSaveStatus {
    kWritten,
    kInvalidState,
    kUnsupportedVersion,
    kIoError,
};

struct UpdateStateSaveResult {
    UpdateStateSaveStatus status = UpdateStateSaveStatus::kIoError;
    std::error_code error;

    [[nodiscard]] bool written() const noexcept {
        return status == UpdateStateSaveStatus::kWritten;
    }
};

using UpdateStateAtomicReplace = std::function<std::error_code(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination)>;

class UpdateStateStore {
public:
    explicit UpdateStateStore(
        std::filesystem::path path,
        UpdateStateAtomicReplace atomicReplace = {});

    [[nodiscard]] UpdateStateLoadResult Load() const noexcept;
    [[nodiscard]] UpdateStateSaveResult Save(
        const UpdateCheckState& state) const noexcept;

private:
    std::filesystem::path path_;
    UpdateStateAtomicReplace atomicReplace_;
};

}  // namespace codex_monitor::update
