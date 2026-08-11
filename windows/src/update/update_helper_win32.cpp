#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "update/update_helper_win32.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace codex_monitor::update {
namespace {

#ifdef _WIN32
constexpr std::wstring_view kHelperMode =
    L"--codex-monitor-update-helper-v1";
constexpr std::string_view kInstallerNamePrefix =
    "CodexMonitorHUD-windows-x64-";
constexpr std::string_view kInstallerNameSuffix = ".msi";
#endif

WindowsUpdateHelperResult HelperFailure(
    WindowsUpdateHelperStatus status) noexcept {
    WindowsUpdateHelperResult result;
    result.status = status;
    return result;
}

#if defined(_WIN32) || \
    defined(CODEX_MONITOR_UPDATE_HELPER_TESTING)
using WaitOperation =
    std::function<WindowsUpdateHelperWaitStatus()>;
using ApplyOperation = std::function<WindowsUpdateApplyResult()>;
using VerifyAndLaunchOperation =
    std::function<WindowsInstalledHudLaunchResult()>;

WindowsUpdateHelperResult RunSequence(
    const WaitOperation& waitForOldProcess,
    const ApplyOperation& applyUpdate,
    const VerifyAndLaunchOperation& verifyAndLaunchInstalledHud) noexcept {
    WindowsUpdateHelperResult result;
    try {
        if (!waitForOldProcess || !applyUpdate ||
            !verifyAndLaunchInstalledHud) {
            result.status = WindowsUpdateHelperStatus::kInvalidInput;
            return result;
        }

        result.waitStatus = waitForOldProcess();
        switch (result.waitStatus) {
            case WindowsUpdateHelperWaitStatus::kExited:
                break;
            case WindowsUpdateHelperWaitStatus::kInvalidHandle:
            case WindowsUpdateHelperWaitStatus::kProcessIdentityMismatch:
                result.status =
                    WindowsUpdateHelperStatus::kOldProcessRejected;
                return result;
            case WindowsUpdateHelperWaitStatus::kTimedOut:
                result.status =
                    WindowsUpdateHelperStatus::kOldProcessWaitTimedOut;
                return result;
            case WindowsUpdateHelperWaitStatus::kWaitFailed:
                result.status =
                    WindowsUpdateHelperStatus::kOldProcessWaitFailed;
                return result;
            case WindowsUpdateHelperWaitStatus::kUnsupportedPlatform:
                result.status =
                    WindowsUpdateHelperStatus::kUnsupportedPlatform;
                return result;
        }

        result.apply = applyUpdate();
        if (!result.apply.installed()) {
            result.status =
                WindowsUpdateHelperStatus::kInstallRejectedOrFailed;
            return result;
        }

        result.launch = verifyAndLaunchInstalledHud();
        if (result.launch.started()) {
            result.status =
                WindowsUpdateHelperStatus::kCompletedAndRestarted;
            return result;
        }
        if (result.launch.status ==
            WindowsInstalledHudLaunchStatus::kProcessStartFailed) {
            result.status = WindowsUpdateHelperStatus::kRestartFailed;
            return result;
        }
        if (result.launch.status ==
            WindowsInstalledHudLaunchStatus::kUnsupportedPlatform) {
            result.status = WindowsUpdateHelperStatus::kUnsupportedPlatform;
            return result;
        }
        result.status =
            WindowsUpdateHelperStatus::kInstalledExecutableRejected;
        return result;
    } catch (...) {
        result.status = WindowsUpdateHelperStatus::kUnexpected;
        return result;
    }
}
#endif

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

std::optional<std::wstring> ComparableAbsolutePath(
    const std::filesystem::path& path) {
    const std::wstring input = path.native();
    if (input.size() < 4U || input.size() >= 32760U ||
        input[1] != L':' || input[2] != L'\\' ||
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

WindowsUpdateHelperWaitStatus WaitForOldWindowsHudProcess(
    std::uintptr_t inheritedProcessHandle,
    std::uint32_t expectedProcessId,
    std::uint64_t expectedCreationTime,
    const std::filesystem::path& expectedExecutablePath,
    std::chrono::milliseconds timeout) noexcept {
    KernelHandle process(
        reinterpret_cast<HANDLE>(inheritedProcessHandle));
    if (!process || expectedProcessId == 0U ||
        expectedProcessId == GetCurrentProcessId() ||
        expectedCreationTime == 0U ||
        expectedExecutablePath.empty() ||
        timeout <= std::chrono::milliseconds::zero() ||
        timeout > std::chrono::minutes(10)) {
        return WindowsUpdateHelperWaitStatus::kInvalidHandle;
    }

    if (GetProcessId(process.get()) != expectedProcessId) {
        return WindowsUpdateHelperWaitStatus::kProcessIdentityMismatch;
    }
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(process.get(), &creation, &exit, &kernel, &user) ||
        FileTimeTicks(creation) != expectedCreationTime) {
        return WindowsUpdateHelperWaitStatus::kProcessIdentityMismatch;
    }

    std::wstring actualImage(32768U, L'\0');
    DWORD actualImageCharacters =
        static_cast<DWORD>(actualImage.size());
    if (!QueryFullProcessImageNameW(process.get(), 0, actualImage.data(),
                                    &actualImageCharacters) ||
        actualImageCharacters == 0U ||
        actualImageCharacters >= actualImage.size()) {
        return WindowsUpdateHelperWaitStatus::kProcessIdentityMismatch;
    }
    actualImage.resize(actualImageCharacters);
    const std::optional<std::wstring> expectedImage =
        ComparableAbsolutePath(expectedExecutablePath);
    if (!expectedImage.has_value() ||
        !SameWindowsPath(actualImage, *expectedImage)) {
        return WindowsUpdateHelperWaitStatus::kProcessIdentityMismatch;
    }

    const auto milliseconds = timeout.count();
    if (milliseconds <= 0 ||
        milliseconds > static_cast<long long>(MAXDWORD - 1U)) {
        return WindowsUpdateHelperWaitStatus::kInvalidHandle;
    }
    const DWORD wait = WaitForSingleObject(
        process.get(), static_cast<DWORD>(milliseconds));
    if (wait == WAIT_TIMEOUT) {
        return WindowsUpdateHelperWaitStatus::kTimedOut;
    }
    if (wait != WAIT_OBJECT_0) {
        return WindowsUpdateHelperWaitStatus::kWaitFailed;
    }
    DWORD exitCode = STILL_ACTIVE;
    if (!GetExitCodeProcess(process.get(), &exitCode) ||
        exitCode == STILL_ACTIVE) {
        return WindowsUpdateHelperWaitStatus::kWaitFailed;
    }
    return WindowsUpdateHelperWaitStatus::kExited;
}

std::optional<std::filesystem::path> InstalledHudExecutablePath() {
    constexpr wchar_t kRegistryKey[] = L"Software\\CodexMonitorHUD";
    constexpr wchar_t kRegistryValue[] = L"InstallFolder";
    std::array<wchar_t, 32768> value{};
    DWORD type = 0;
    DWORD bytes = static_cast<DWORD>(sizeof(value));
    const LSTATUS status = RegGetValueW(
        HKEY_CURRENT_USER, kRegistryKey, kRegistryValue,
        RRF_RT_REG_SZ | RRF_NOEXPAND | RRF_ZEROONFAILURE,
        &type, value.data(), &bytes);
    if (status != ERROR_SUCCESS || type != REG_SZ ||
        bytes < sizeof(wchar_t) * 2U ||
        bytes > sizeof(value) || bytes % sizeof(wchar_t) != 0U) {
        return std::nullopt;
    }
    const std::size_t characters = bytes / sizeof(wchar_t);
    if (value[characters - 1U] != L'\0') return std::nullopt;
    std::wstring directory(value.data(), characters - 1U);
    if (directory.empty() ||
        directory.find(L'\0') != std::wstring::npos) {
        return std::nullopt;
    }
    while (directory.size() > 3U &&
           (directory.back() == L'\\' || directory.back() == L'/')) {
        directory.pop_back();
    }
    return std::filesystem::path(directory) /
           std::filesystem::path(kWindowsHudExecutableName);
}

int HexValue(wchar_t value) noexcept {
    if (value >= L'0' && value <= L'9') return value - L'0';
    if (value >= L'a' && value <= L'f') return value - L'a' + 10;
    if (value >= L'A' && value <= L'F') return value - L'A' + 10;
    return -1;
}

std::optional<PublisherCertificateSha256>
ConfiguredPublisherFingerprint() noexcept {
#if defined(CODEX_MONITOR_WINDOWS_PUBLISHER_SHA256)
    constexpr std::string_view configured =
        CODEX_MONITOR_WINDOWS_PUBLISHER_SHA256;
    if (configured.size() != PublisherCertificateSha256{}.size() * 2U) {
        return std::nullopt;
    }
    PublisherCertificateSha256 fingerprint{};
    for (std::size_t index = 0; index < fingerprint.size(); ++index) {
        const char highCharacter = configured[index * 2U];
        const char lowCharacter = configured[index * 2U + 1U];
        const int high = highCharacter >= '0' && highCharacter <= '9'
            ? highCharacter - '0'
            : highCharacter >= 'a' && highCharacter <= 'f'
                ? highCharacter - 'a' + 10
                : highCharacter >= 'A' && highCharacter <= 'F'
                    ? highCharacter - 'A' + 10
                    : -1;
        const int low = lowCharacter >= '0' && lowCharacter <= '9'
            ? lowCharacter - '0'
            : lowCharacter >= 'a' && lowCharacter <= 'f'
                ? lowCharacter - 'a' + 10
                : lowCharacter >= 'A' && lowCharacter <= 'F'
                    ? lowCharacter - 'A' + 10
                    : -1;
        if (high < 0 || low < 0) return std::nullopt;
        fingerprint[index] =
            static_cast<std::uint8_t>((high << 4) | low);
    }
    return fingerprint;
#else
    return std::nullopt;
#endif
}

template <typename Unsigned>
std::optional<Unsigned> ParseUnsignedDecimal(
    std::wstring_view value) noexcept {
    static_assert(std::numeric_limits<Unsigned>::is_integer &&
                  !std::numeric_limits<Unsigned>::is_signed);
    if (value.empty()) return std::nullopt;
    Unsigned parsed = 0;
    for (const wchar_t character : value) {
        if (character < L'0' || character > L'9') return std::nullopt;
        const Unsigned digit = static_cast<Unsigned>(character - L'0');
        if (parsed >
            (std::numeric_limits<Unsigned>::max() - digit) / 10U) {
            return std::nullopt;
        }
        parsed = static_cast<Unsigned>(parsed * 10U + digit);
    }
    return parsed;
}

std::optional<std::string> PrintableAscii(
    std::wstring_view value,
    std::size_t maximumCharacters) {
    if (value.empty() || value.size() > maximumCharacters) {
        return std::nullopt;
    }
    std::string converted;
    converted.reserve(value.size());
    for (const wchar_t character : value) {
        if (character < 0x21 || character > 0x7e) {
            return std::nullopt;
        }
        converted.push_back(static_cast<char>(character));
    }
    return converted;
}

std::optional<std::string> NormalizeDigest(
    std::wstring_view value) {
    if (value.size() != 64U) return std::nullopt;
    std::string digest;
    digest.reserve(value.size());
    for (const wchar_t character : value) {
        const int parsed = HexValue(character);
        if (parsed < 0) return std::nullopt;
        digest.push_back("0123456789abcdef"[parsed]);
    }
    return digest;
}

int ExitCodeForHelperResult(
    const WindowsUpdateHelperResult& result) noexcept {
    switch (result.status) {
        case WindowsUpdateHelperStatus::kCompletedAndRestarted:
            return 0;
        case WindowsUpdateHelperStatus::kInvalidInput:
            return 10;
        case WindowsUpdateHelperStatus::kPublisherNotConfigured:
            return 11;
        case WindowsUpdateHelperStatus::kOldProcessRejected:
            return 20;
        case WindowsUpdateHelperStatus::kOldProcessWaitTimedOut:
            return 21;
        case WindowsUpdateHelperStatus::kOldProcessWaitFailed:
            return 22;
        case WindowsUpdateHelperStatus::kInstallRejectedOrFailed:
            return 30;
        case WindowsUpdateHelperStatus::kInstalledExecutableRejected:
            return 40;
        case WindowsUpdateHelperStatus::kRestartFailed:
            return 41;
        case WindowsUpdateHelperStatus::kUnsupportedPlatform:
            return 50;
        case WindowsUpdateHelperStatus::kUnexpected:
            return 51;
    }
    return 51;
}

#endif

}  // namespace

WindowsUpdateHelperResult RunWindowsUpdateHelper(
    const WindowsUpdateHelperRequest& request) noexcept {
    if (!request.trustedPublisherFingerprint.has_value()) {
#ifdef _WIN32
        KernelHandle consumed(reinterpret_cast<HANDLE>(
            request.inheritedOldProcessHandle));
#endif
        return HelperFailure(
            WindowsUpdateHelperStatus::kPublisherNotConfigured);
    }
    if (request.inheritedOldProcessHandle == 0U ||
        request.expectedOldProcessId == 0U ||
        request.expectedOldProcessCreationTime == 0U ||
        request.oldProcessExitTimeout <=
            std::chrono::milliseconds::zero() ||
        request.oldProcessExitTimeout > std::chrono::minutes(10) ||
        request.installerPath.empty() ||
        request.expectedInstallerFileName.empty() ||
        request.sha256Manifest.empty() || request.expectedVersion.empty() ||
        request.installedExecutablePath.empty()) {
#ifdef _WIN32
        KernelHandle consumed(reinterpret_cast<HANDLE>(
            request.inheritedOldProcessHandle));
#endif
        return HelperFailure(WindowsUpdateHelperStatus::kInvalidInput);
    }

#ifdef _WIN32
    try {
        return RunSequence(
            [&request] {
                return WaitForOldWindowsHudProcess(
                    request.inheritedOldProcessHandle,
                    request.expectedOldProcessId,
                    request.expectedOldProcessCreationTime,
                    request.installedExecutablePath,
                    request.oldProcessExitTimeout);
            },
            [&request] {
                return ApplyVerifiedWindowsMsiUpdate(
                    request.installerPath,
                    request.expectedInstallerFileName,
                    request.sha256Manifest, request.expectedVersion,
                    request.trustedPublisherFingerprint);
            },
            [&request] {
                return VerifyAndLaunchInstalledWindowsHud(
                    request.installedExecutablePath,
                    request.expectedVersion,
                    request.trustedPublisherFingerprint);
            });
    } catch (...) {
        KernelHandle consumed(reinterpret_cast<HANDLE>(
            request.inheritedOldProcessHandle));
        return HelperFailure(WindowsUpdateHelperStatus::kUnexpected);
    }
#else
    (void)request;
    return HelperFailure(WindowsUpdateHelperStatus::kUnsupportedPlatform);
#endif
}

std::optional<int> TryRunWindowsUpdateHelperCommandLine() noexcept {
#ifdef _WIN32
    int argumentCount = 0;
    wchar_t** arguments = CommandLineToArgvW(
        GetCommandLineW(), &argumentCount);
    if (arguments == nullptr || argumentCount <= 0) {
        if (arguments != nullptr) LocalFree(arguments);
        return 51;
    }
    const auto releaseArguments = [&arguments] {
        LocalFree(arguments);
        arguments = nullptr;
    };
    if (argumentCount < 2 ||
        std::wstring_view(arguments[1]) != kHelperMode) {
        releaseArguments();
        return std::nullopt;
    }
    if (argumentCount != 8) {
        releaseArguments();
        return 10;
    }

    const std::optional<std::uintptr_t> inheritedHandle =
        ParseUnsignedDecimal<std::uintptr_t>(arguments[2]);
    const std::optional<std::uint32_t> processId =
        ParseUnsignedDecimal<std::uint32_t>(arguments[3]);
    const std::optional<std::uint64_t> creationTime =
        ParseUnsignedDecimal<std::uint64_t>(arguments[4]);
    const std::filesystem::path installer(arguments[5]);
    const std::optional<std::string> digest =
        NormalizeDigest(arguments[6]);
    const std::optional<std::string> version =
        PrintableAscii(arguments[7], 64U);
    const std::optional<std::string> filename =
        PrintableAscii(installer.filename().native(), 240U);
    const std::optional<std::filesystem::path> installedExecutable =
        InstalledHudExecutablePath();
    const std::optional<PublisherCertificateSha256> fingerprint =
        ConfiguredPublisherFingerprint();

    if (!inheritedHandle || *inheritedHandle == 0U || !processId ||
        *processId == 0U || !creationTime || *creationTime == 0U ||
        installer.empty() || !digest || !version || !filename ||
        !installedExecutable) {
        if (inheritedHandle && *inheritedHandle != 0U) {
            KernelHandle consumed(
                reinterpret_cast<HANDLE>(*inheritedHandle));
        }
        releaseArguments();
        return 10;
    }
    std::string expectedFilename(kInstallerNamePrefix);
    expectedFilename.append(*version);
    expectedFilename.append(kInstallerNameSuffix);
    if (*filename != expectedFilename) {
        KernelHandle consumed(
            reinterpret_cast<HANDLE>(*inheritedHandle));
        releaseArguments();
        return 10;
    }

    WindowsUpdateHelperRequest request;
    request.inheritedOldProcessHandle = *inheritedHandle;
    request.expectedOldProcessId = *processId;
    request.expectedOldProcessCreationTime = *creationTime;
    request.installerPath = installer;
    request.expectedInstallerFileName = std::move(expectedFilename);
    request.sha256Manifest = *digest + "  " + *filename + "\n";
    request.expectedVersion = *version;
    request.trustedPublisherFingerprint = fingerprint;
    request.installedExecutablePath = *installedExecutable;
    releaseArguments();
    return ExitCodeForHelperResult(RunWindowsUpdateHelper(request));
#else
    return std::nullopt;
#endif
}

#if defined(CODEX_MONITOR_UPDATE_HELPER_TESTING)

WindowsUpdateHelperResult RunWindowsUpdateHelperSequenceForTesting(
    const WindowsUpdateHelperWaitOperation& waitForOldProcess,
    const WindowsUpdateHelperApplyOperation& applyUpdate,
    const WindowsUpdateHelperVerifyAndLaunchOperation&
        verifyAndLaunchInstalledHud) noexcept {
    return RunSequence(waitForOldProcess, applyUpdate,
                       verifyAndLaunchInstalledHud);
}

#ifdef _WIN32
WindowsUpdateHelperWaitStatus WaitForOldWindowsHudProcessForTesting(
    std::uintptr_t inheritedProcessHandle,
    std::uint32_t expectedProcessId,
    std::uint64_t expectedCreationTime,
    const std::filesystem::path& expectedExecutablePath,
    std::chrono::milliseconds timeout) noexcept {
    return WaitForOldWindowsHudProcess(
        inheritedProcessHandle, expectedProcessId, expectedCreationTime,
        expectedExecutablePath, timeout);
}
#endif
#endif

}  // namespace codex_monitor::update
