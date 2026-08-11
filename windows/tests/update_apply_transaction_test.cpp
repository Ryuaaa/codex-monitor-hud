#include "update/update_apply_transaction_win32.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <msi.h>
#include <msiquery.h>
#endif

namespace {

using codex_monitor::update::ApplyVerifiedWindowsMsiUpdate;
using codex_monitor::update::ApplyVerifiedWindowsMsiUpdateForTesting;
using codex_monitor::update::PublisherCertificateSha256;
using codex_monitor::update::WindowsInstallerVerificationStatus;
using codex_monitor::update::WindowsMsiIdentityVerificationStatus;
using codex_monitor::update::WindowsUpdateApplyStatus;

constexpr std::string_view kFileName =
    "CodexMonitorHUD-windows-x64-1.2.3.msi";
#ifdef _WIN32
constexpr std::string_view kUnsignedContents =
    "not a signed Windows Installer package";
#endif
constexpr std::string_view kUnsignedSha256 =
    "c11fb33bef83c390e42d3f18bcc2d0df"
    "6a8f110d7f5708481a8f571839e7fe15";

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::string Manifest(std::string_view digest,
                     std::string_view filename = kFileName) {
    std::string manifest(digest);
    manifest.append("  ");
    manifest.append(filename);
    manifest.push_back('\n');
    return manifest;
}

#ifdef _WIN32

std::filesystem::path CanonicalWindowsTemporaryDirectory() {
    const std::filesystem::path temporary =
        std::filesystem::temp_directory_path();
    std::wstring expanded(32768U, L'\0');
    const DWORD written = GetLongPathNameW(
        temporary.c_str(), expanded.data(),
        static_cast<DWORD>(expanded.size()));
    Require(written > 0U && written < expanded.size(),
            "the Windows temporary directory must resolve to a long path");
    expanded.resize(written);
    return std::filesystem::path(std::move(expanded));
}

int HexValue(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

std::optional<PublisherCertificateSha256> ParsePublisherFingerprint(
    std::string_view value) noexcept {
    if (value.size() != PublisherCertificateSha256{}.size() * 2U) {
        return std::nullopt;
    }
    PublisherCertificateSha256 fingerprint{};
    for (std::size_t index = 0; index < fingerprint.size(); ++index) {
        const int high = HexValue(value[index * 2U]);
        const int low = HexValue(value[index * 2U + 1U]);
        if (high < 0 || low < 0) return std::nullopt;
        fingerprint[index] =
            static_cast<std::uint8_t>((high << 4) | low);
    }
    return fingerprint;
}

#endif

void TestFailuresNeverInvokeInstaller() {
    PublisherCertificateSha256 fingerprint{};
    int callbackCount = 0;
    const auto callback = [&](const std::filesystem::path&) {
        ++callbackCount;
        return 0;
    };

    const auto invalidManifest = ApplyVerifiedWindowsMsiUpdateForTesting(
        std::filesystem::path("relative.msi"), kFileName, "not-a-manifest",
        "1.2.3", fingerprint, callback);
    Require(invalidManifest.status ==
                WindowsUpdateApplyStatus::kChecksumRejected &&
                invalidManifest.checksum.status ==
                    WindowsInstallerVerificationStatus::kInvalidManifest &&
                callbackCount == 0 && !invalidManifest.installAttempted,
            "a malformed checksum manifest must reject before the callback");

    const auto relative = ApplyVerifiedWindowsMsiUpdateForTesting(
        std::filesystem::path(std::string(kFileName)), kFileName,
        Manifest(kUnsignedSha256), "1.2.3", fingerprint, callback);
    Require(relative.status == WindowsUpdateApplyStatus::kPathNotAbsolute &&
                callbackCount == 0 && !relative.installAttempted,
            "a relative MSI path must reject before the callback");

    const auto missingCallback = ApplyVerifiedWindowsMsiUpdateForTesting(
        std::filesystem::path(std::string(kFileName)), kFileName,
        Manifest(kUnsignedSha256), "1.2.3", fingerprint, {});
    Require(missingCallback.status == WindowsUpdateApplyStatus::kInvalidInput &&
                !missingCallback.installAttempted,
            "a missing synchronous installer callback must fail closed");
}

void TestPlatformVerificationBoundary() {
    PublisherCertificateSha256 fingerprint{};
    int callbackCount = 0;
    const auto callback = [&](const std::filesystem::path&) {
        ++callbackCount;
        return 0;
    };

#ifdef _WIN32
    // The hosted Windows runner can expose %TEMP% through an 8.3 alias
    // (RUNNER~1). Expand it first because the production boundary intentionally
    // rejects aliases after resolving handles.
    const std::filesystem::path root =
        CanonicalWindowsTemporaryDirectory() /
        ("codex-monitor-update-apply-" +
         std::to_string(static_cast<long long>(
             std::filesystem::file_time_type::clock::now()
                 .time_since_epoch().count())));
    std::error_code error;
    std::filesystem::create_directories(root, error);
    Require(!error, "the apply transaction test directory must be created");
    const std::filesystem::path installer =
        root / std::filesystem::path(std::string(kFileName));
    {
        std::ofstream output(installer, std::ios::binary | std::ios::trunc);
        output << kUnsignedContents;
        Require(static_cast<bool>(output),
                "the unsigned apply transaction fixture must be written");
    }

    std::string wrongDigest(kUnsignedSha256);
    wrongDigest.front() = wrongDigest.front() == '0' ? '1' : '0';
    const auto checksumRejected = ApplyVerifiedWindowsMsiUpdateForTesting(
        installer, kFileName, Manifest(wrongDigest), "1.2.3", fingerprint,
        callback);
    const bool digestMismatchRejected =
        checksumRejected.status ==
            WindowsUpdateApplyStatus::kChecksumRejected &&
        checksumRejected.checksum.status ==
            WindowsInstallerVerificationStatus::kDigestMismatch &&
        callbackCount == 0 && !checksumRejected.installAttempted;
    if (!digestMismatchRejected) {
        std::cerr << "digest_mismatch_status="
                  << static_cast<int>(checksumRejected.status)
                  << " checksum_status="
                  << static_cast<int>(checksumRejected.checksum.status)
                  << " callback_count=" << callbackCount
                  << " install_attempted="
                  << checksumRejected.installAttempted << '\n';
    }
    Require(digestMismatchRejected,
            "a digest mismatch must never reach the installer callback");

    const auto unsignedResult = ApplyVerifiedWindowsMsiUpdateForTesting(
        installer, kFileName, Manifest(kUnsignedSha256), "1.2.3",
        fingerprint, callback);
    const bool unsignedRejected =
        unsignedResult.status ==
            WindowsUpdateApplyStatus::kPublisherOrIdentityRejected &&
        unsignedResult.checksum.checksumVerified() &&
        unsignedResult.publisherAndIdentity.status ==
            WindowsMsiIdentityVerificationStatus::
                kSignatureVerificationFailed &&
        callbackCount == 0 && !unsignedResult.installAttempted;
    if (!unsignedRejected) {
        std::cerr << "unsigned_status="
                  << static_cast<int>(unsignedResult.status)
                  << " checksum_status="
                  << static_cast<int>(unsignedResult.checksum.status)
                  << " identity_status="
                  << static_cast<int>(
                         unsignedResult.publisherAndIdentity.status)
                  << " callback_count=" << callbackCount
                  << " install_attempted="
                  << unsignedResult.installAttempted << '\n';
    }
    Require(unsignedRejected,
            "an unsigned MSI must fail after SHA-256 and before the callback");

    const std::filesystem::path nonCanonical =
        root / "." / std::filesystem::path(std::string(kFileName));
    const auto nonCanonicalResult = ApplyVerifiedWindowsMsiUpdateForTesting(
        nonCanonical, kFileName, Manifest(kUnsignedSha256), "1.2.3",
        fingerprint, callback);
    Require((nonCanonicalResult.status ==
                 WindowsUpdateApplyStatus::kPathNotCanonical ||
             nonCanonicalResult.status ==
                 WindowsUpdateApplyStatus::kPublisherOrIdentityRejected) &&
                callbackCount == 0,
            "a lexical alias must never create an unchecked callback path");

    std::filesystem::remove_all(root, error);
#else
    const auto unsupported = ApplyVerifiedWindowsMsiUpdateForTesting(
        std::filesystem::temp_directory_path() /
            std::filesystem::path(std::string(kFileName)),
        kFileName, Manifest(kUnsignedSha256), "1.2.3", fingerprint, callback);
    Require(unsupported.status ==
                WindowsUpdateApplyStatus::kUnsupportedPlatform &&
                callbackCount == 0 && !unsupported.installAttempted,
            "portable builds must not invoke a Windows installer callback");
#endif
}

#ifdef _WIN32

bool CanOpenDirectoryForDelete(const std::filesystem::path& directory) {
    HANDLE handle = CreateFileW(
        directory.c_str(), DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    CloseHandle(handle);
    return true;
}

struct UpdatePathLockProbe {
    bool renameBlocked = false;
    bool deleteBlocked = false;
    bool writeBlocked = false;
    bool parentDeleteAccessBlocked = false;

    [[nodiscard]] bool allHeld() const noexcept {
        return renameBlocked && deleteBlocked && writeBlocked &&
               parentDeleteAccessBlocked;
    }
};

UpdatePathLockProbe ProbePathAndAncestorLocks(
    const std::filesystem::path& installer) {
    const std::filesystem::path moved =
        installer.parent_path() / L"transaction-lock-probe.msi";
    DeleteFileW(moved.c_str());
    UpdatePathLockProbe probe;
    probe.renameBlocked =
        MoveFileExW(installer.c_str(), moved.c_str(),
                    MOVEFILE_REPLACE_EXISTING) == FALSE;
    probe.deleteBlocked = DeleteFileW(installer.c_str()) == FALSE;
    HANDLE writeHandle = CreateFileW(
        installer.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    probe.writeBlocked = writeHandle == INVALID_HANDLE_VALUE;
    if (writeHandle != INVALID_HANDLE_VALUE) CloseHandle(writeHandle);

    probe.parentDeleteAccessBlocked =
        !CanOpenDirectoryForDelete(installer.parent_path());
    return probe;
}

int RunSignedApplyCli(int argc, char* argv[]) {
    if (argc != 7 ||
        std::string_view(argv[1]) != "--verify-apply-signed-msi") {
        std::cerr << "usage: --verify-apply-signed-msi <absolute-msi> "
                     "<expected-version> <64hex-cert-sha256> "
                     "<64hex-msi-sha256> "
                     "<installed|publisher-rejected|checksum-rejected|"
                     "signature-rejected>\n";
        return 2;
    }

    std::filesystem::path installer;
    try {
        installer = std::filesystem::u8path(argv[2]);
    } catch (...) {
        std::cerr << "signed_apply=invalid-msi-path\n";
        return 2;
    }
    const std::optional<PublisherCertificateSha256> fingerprint =
        ParsePublisherFingerprint(argv[4]);
    const std::string_view digest(argv[5]);
    if (!installer.is_absolute() || !fingerprint.has_value() ||
        digest.size() != 64) {
        std::cerr << "signed_apply=invalid-input\n";
        return 2;
    }

    const std::string filename = installer.filename().u8string();
    const bool controlledParentDeleteAvailableBefore =
        CanOpenDirectoryForDelete(installer.parent_path());
    int callbackCount = 0;
    UpdatePathLockProbe lockProbe;
    const auto result = ApplyVerifiedWindowsMsiUpdateForTesting(
        installer, filename, Manifest(digest, filename), argv[3], fingerprint,
        [&](const std::filesystem::path& verifiedPath) {
            ++callbackCount;
            lockProbe = ProbePathAndAncestorLocks(verifiedPath);
            return lockProbe.allHeld() ? 0 : 1603;
        });

    const std::string_view expected(argv[6]);
    bool matched = false;
    if (expected == "installed") {
        matched = result.status == WindowsUpdateApplyStatus::kInstalled &&
                  result.installed() && callbackCount == 1 &&
                  controlledParentDeleteAvailableBefore &&
                  lockProbe.allHeld();
    } else if (expected == "publisher-rejected") {
        matched = result.status ==
                      WindowsUpdateApplyStatus::kPublisherOrIdentityRejected &&
                  callbackCount == 0;
    } else if (expected == "checksum-rejected") {
        matched = result.status ==
                      WindowsUpdateApplyStatus::kChecksumRejected &&
                  callbackCount == 0;
    } else if (expected == "signature-rejected") {
        matched = result.status ==
                      WindowsUpdateApplyStatus::
                          kPublisherOrIdentityRejected &&
                  result.publisherAndIdentity.status ==
                      WindowsMsiIdentityVerificationStatus::
                          kSignatureVerificationFailed &&
                  callbackCount == 0;
    } else {
        std::cerr << "signed_apply=invalid-expected-result\n";
        return 2;
    }

    if (!matched) {
        std::cerr << "signed_apply=fail status="
                  << static_cast<int>(result.status)
                  << " checksum_status="
                  << static_cast<int>(result.checksum.status)
                  << " identity_status="
                  << static_cast<int>(result.publisherAndIdentity.status)
                  << " callback_count=" << callbackCount
                  << " parent_delete_before="
                  << controlledParentDeleteAvailableBefore
                  << " rename_blocked=" << lockProbe.renameBlocked
                  << " delete_blocked=" << lockProbe.deleteBlocked
                  << " write_blocked=" << lockProbe.writeBlocked
                  << " parent_delete_blocked="
                  << lockProbe.parentDeleteAccessBlocked << '\n';
        return 1;
    }

    if (expected == "installed") {
        const std::filesystem::path moved =
            installer.parent_path() / L"transaction-lock-release-probe.msi";
        DeleteFileW(moved.c_str());
        if (!MoveFileExW(installer.c_str(), moved.c_str(),
                         MOVEFILE_REPLACE_EXISTING) ||
            !MoveFileExW(moved.c_str(), installer.c_str(),
                         MOVEFILE_REPLACE_EXISTING)) {
            std::cerr << "signed_apply=locks-not-released\n";
            return 1;
        }
    }

    std::cout << "signed_apply=pass expected=" << expected << '\n';
    return 0;
}

int RunSignedInstallCli(int argc, char* argv[]) {
    if (argc != 6 ||
        std::string_view(argv[1]) != "--install-signed-msi") {
        std::cerr << "usage: --install-signed-msi <absolute-msi> "
                     "<expected-version> <64hex-cert-sha256> "
                     "<64hex-msi-sha256>\n";
        return 2;
    }

    std::filesystem::path installer;
    try {
        installer = std::filesystem::u8path(argv[2]);
    } catch (...) {
        std::cerr << "signed_install=invalid-msi-path\n";
        return 2;
    }
    const std::optional<PublisherCertificateSha256> fingerprint =
        ParsePublisherFingerprint(argv[4]);
    const std::string_view digest(argv[5]);
    if (!installer.is_absolute() || !fingerprint.has_value() ||
        digest.size() != 64) {
        std::cerr << "signed_install=invalid-input\n";
        return 2;
    }

    const std::string filename = installer.filename().u8string();
    const auto result = ApplyVerifiedWindowsMsiUpdate(
        installer, filename, Manifest(digest, filename), argv[3], fingerprint);
    if (!result.installed() || !result.installAttempted ||
        (result.installerExitCode != 0 &&
         result.installerExitCode != 3010)) {
        std::cerr << "signed_install=fail status="
                  << static_cast<int>(result.status)
                  << " checksum_status="
                  << static_cast<int>(result.checksum.status)
                  << " identity_status="
                  << static_cast<int>(result.publisherAndIdentity.status)
                  << " installer_exit=" << result.installerExitCode << '\n';
        return 1;
    }

    std::cout << "signed_install=pass reboot_required="
              << result.rebootRequired << '\n';
    return 0;
}

int RunTamperSignedMsiCli(int argc, char* argv[]) {
    if (argc != 3 ||
        std::string_view(argv[1]) != "--tamper-msi-product-name") {
        std::cerr << "usage: --tamper-msi-product-name <absolute-msi>\n";
        return 2;
    }

    std::filesystem::path installer;
    try {
        installer = std::filesystem::u8path(argv[2]);
    } catch (...) {
        std::cerr << "tamper_msi=invalid-path\n";
        return 2;
    }
    if (!installer.is_absolute()) {
        std::cerr << "tamper_msi=path-not-absolute\n";
        return 2;
    }

    MSIHANDLE database = 0;
    MSIHANDLE view = 0;
    UINT status = MsiOpenDatabaseW(
        installer.c_str(), MSIDBOPEN_DIRECT, &database);
    if (status == ERROR_SUCCESS) {
        status = MsiDatabaseOpenViewW(
            database,
            L"UPDATE `Property` SET `Value`='Codex Monitor HUD Tampered' "
            L"WHERE `Property`='ProductName'",
            &view);
    }
    if (status == ERROR_SUCCESS) {
        status = MsiViewExecute(view, 0);
    }
    if (status == ERROR_SUCCESS) {
        status = MsiDatabaseCommit(database);
    }
    if (view != 0) MsiCloseHandle(view);
    if (database != 0) MsiCloseHandle(database);

    if (status != ERROR_SUCCESS) {
        std::cerr << "tamper_msi=failed status=" << status << '\n';
        return 1;
    }
    std::cout << "tamper_msi=pass\n";
    return 0;
}

#endif

}  // namespace

int main(int argc, char* argv[]) {
#ifdef _WIN32
    if (argc != 1) {
        if (std::string_view(argv[1]) == "--verify-apply-signed-msi") {
            return RunSignedApplyCli(argc, argv);
        }
        if (std::string_view(argv[1]) == "--install-signed-msi") {
            return RunSignedInstallCli(argc, argv);
        }
        if (std::string_view(argv[1]) == "--tamper-msi-product-name") {
            return RunTamperSignedMsiCli(argc, argv);
        }
        std::cerr << "update_apply=unknown-command\n";
        return 2;
    }
#else
    (void)argc;
    (void)argv;
#endif
    TestFailuresNeverInvokeInstaller();
    TestPlatformVerificationBoundary();
    std::cout << "update_apply_transaction_tests=pass\n";
    return 0;
}
