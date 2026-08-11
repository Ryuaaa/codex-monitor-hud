#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "update/update_helper_launcher_win32.h"

#include "update/update_helper_win32.h"

#include <limits>
#include <optional>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace codex_monitor::update {
namespace {

WindowsUpdateHelperLauncherResult LauncherFailure(
    WindowsUpdateHelperLauncherStatus status) noexcept {
    WindowsUpdateHelperLauncherResult result;
    result.status = status;
    return result;
}

#ifdef _WIN32

class KernelHandle {
public:
    explicit KernelHandle(HANDLE value) noexcept : value_(value) {}
    ~KernelHandle() {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
    }
    KernelHandle(const KernelHandle&) = delete;
    KernelHandle& operator=(const KernelHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE value_ = nullptr;
};

std::uint64_t FileTimeTicks(const FILETIME& value) noexcept {
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32U) |
           static_cast<std::uint64_t>(value.dwLowDateTime);
}

std::optional<std::wstring> AbsoluteComparablePath(
    const std::filesystem::path& path) {
    const std::wstring input = path.native();
    if (input.empty() || input.size() >= 32760U ||
        input.find(L'\0') != std::wstring::npos) {
        return std::nullopt;
    }
    std::wstring normalized(32768U, L'\0');
    const DWORD written = GetFullPathNameW(
        input.c_str(), static_cast<DWORD>(normalized.size()),
        normalized.data(), nullptr);
    if (written == 0U || written >= normalized.size()) {
        return std::nullopt;
    }
    normalized.resize(written);
    return normalized;
}

bool SameWindowsPath(std::wstring_view lhs,
                     std::wstring_view rhs) noexcept {
    if (lhs.size() > static_cast<std::size_t>(
                         std::numeric_limits<int>::max()) ||
        rhs.size() > static_cast<std::size_t>(
                         std::numeric_limits<int>::max())) {
        return false;
    }
    return CompareStringOrdinal(
               lhs.data(), static_cast<int>(lhs.size()), rhs.data(),
               static_cast<int>(rhs.size()), TRUE) == CSTR_EQUAL;
}

std::optional<std::filesystem::path> CurrentExecutablePath() {
    std::wstring path(32768U, L'\0');
    const DWORD written = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (written == 0U || written >= path.size()) return std::nullopt;
    path.resize(written);
    return std::filesystem::path(std::move(path));
}

bool IsCurrentInstalledHud(
    const std::filesystem::path& currentExecutable,
    const std::filesystem::path& installedExecutable) {
    const std::optional<std::wstring> current =
        AbsoluteComparablePath(currentExecutable);
    const std::optional<std::wstring> installed =
        AbsoluteComparablePath(installedExecutable);
    return current.has_value() && installed.has_value() &&
           SameWindowsPath(*current, *installed);
}

#endif

}  // namespace

WindowsUpdateHelperLauncherResult LaunchPreparedWindowsUpdateHelper(
    const WindowsUpdateHelperLauncherRequest& request) noexcept {
    try {
        if (request.privateUpdateDirectory.empty() ||
            !request.privateUpdateDirectory.is_absolute() ||
            request.installerPath.empty() ||
            !request.installerPath.is_absolute() ||
            request.installerPath.parent_path() !=
                request.privateUpdateDirectory ||
            request.installerSha256.size() != 64U ||
            request.currentVersion.empty() ||
            request.targetVersion.empty()) {
            return LauncherFailure(
                WindowsUpdateHelperLauncherStatus::kInvalidInput);
        }

#ifdef _WIN32
        const std::optional<PublisherCertificateSha256> fingerprint =
            ConfiguredWindowsUpdatePublisherFingerprint();
        if (!fingerprint.has_value()) {
            return LauncherFailure(
                WindowsUpdateHelperLauncherStatus::
                    kPublisherNotConfigured);
        }
        const std::optional<std::filesystem::path> currentExecutable =
            CurrentExecutablePath();
        const std::optional<std::filesystem::path> installedExecutable =
            InstalledWindowsHudExecutablePath();
        if (!currentExecutable.has_value() ||
            !installedExecutable.has_value() ||
            !IsCurrentInstalledHud(*currentExecutable,
                                   *installedExecutable)) {
            return LauncherFailure(
                WindowsUpdateHelperLauncherStatus::
                    kNotRunningFromInstalledHud);
        }

        WindowsUpdateHelperLauncherResult result;
        result.helperCopyPath = request.privateUpdateDirectory /
            std::filesystem::path(kWindowsHudExecutableName);
        if (!CopyFileW(currentExecutable->c_str(),
                       result.helperCopyPath.c_str(), TRUE)) {
            result.status =
                WindowsUpdateHelperLauncherStatus::kHelperCopyFailed;
            return result;
        }

        HANDLE inheritedProcess = nullptr;
        if (!DuplicateHandle(
                GetCurrentProcess(), GetCurrentProcess(),
                GetCurrentProcess(), &inheritedProcess,
                SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                TRUE, 0)) {
            result.status = WindowsUpdateHelperLauncherStatus::
                kCurrentProcessHandleFailed;
            return result;
        }
        KernelHandle inherited(inheritedProcess);
        FILETIME creation{};
        FILETIME exit{};
        FILETIME kernel{};
        FILETIME user{};
        if (!GetProcessTimes(inherited.get(), &creation, &exit, &kernel,
                             &user)) {
            result.status = WindowsUpdateHelperLauncherStatus::
                kCurrentProcessHandleFailed;
            return result;
        }

        WindowsUpdateHelperChildRequest child;
        child.inheritedOldProcessHandle =
            reinterpret_cast<std::uintptr_t>(inherited.get());
        child.expectedOldProcessId = GetCurrentProcessId();
        child.expectedOldProcessCreationTime = FileTimeTicks(creation);
        child.installerPath = request.installerPath;
        child.installerSha256 = request.installerSha256;
        child.targetVersion = request.targetVersion;
        child.previousVersion = request.currentVersion;
        result.launch = VerifyAndLaunchWindowsUpdateHelperCopy(
            result.helperCopyPath, request.currentVersion, fingerprint,
            child);
        result.status = result.launch.started()
            ? WindowsUpdateHelperLauncherStatus::kStarted
            : WindowsUpdateHelperLauncherStatus::
                  kHelperCopyRejectedOrStartFailed;
        return result;
#else
        (void)request;
        return LauncherFailure(
            WindowsUpdateHelperLauncherStatus::kUnsupportedPlatform);
#endif
    } catch (...) {
        return LauncherFailure(
            WindowsUpdateHelperLauncherStatus::kUnexpected);
    }
}

}  // namespace codex_monitor::update
