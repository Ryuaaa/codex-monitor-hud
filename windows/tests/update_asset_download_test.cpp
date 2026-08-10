#include "update/update_asset_download_win32.h"

#include <iostream>
#include <filesystem>
#include <iterator>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

int failures = 0;

void Expect(bool condition, std::string_view message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

void ExpectInitialAccepted(std::string_view url,
                           std::string_view filename,
                           std::string_view message) {
    const auto parsed =
        codex_monitor::update::ParseInitialUpdateAssetUrl(url, filename);
    Expect(parsed.has_value(), message);
    if (parsed.has_value()) {
        Expect(parsed->host == L"github.com", "initial host is canonicalized");
    }
}

void ExpectInitialRejected(std::string_view url,
                           std::string_view filename,
                           std::string_view message) {
    Expect(!codex_monitor::update::ParseInitialUpdateAssetUrl(url, filename)
                .has_value(),
           message);
}

void ExpectRedirectAccepted(std::string_view url,
                            std::string_view message) {
    const auto parsed =
        codex_monitor::update::ParseGitHubReleaseAssetRedirectUrl(url);
    Expect(parsed.has_value(), message);
    if (parsed.has_value()) {
        Expect(parsed->host == L"release-assets.githubusercontent.com",
               "redirect host is canonicalized");
    }
}

void ExpectRedirectRejected(std::string_view url,
                            std::string_view message) {
    Expect(!codex_monitor::update::ParseGitHubReleaseAssetRedirectUrl(url)
                .has_value(),
           message);
}

#ifdef _WIN32

void RunWindowsDirectoryPolicyTests(std::string_view url,
                                    std::string_view filename) {
    wchar_t temporaryPath[MAX_PATH + 1]{};
    wchar_t uniquePath[MAX_PATH + 1]{};
    const DWORD temporaryLength =
        GetTempPathW(static_cast<DWORD>(std::size(temporaryPath)),
                     temporaryPath);
    if (temporaryLength == 0 || temporaryLength >= std::size(temporaryPath) ||
        GetTempFileNameW(temporaryPath, L"cmh", 0, uniquePath) == 0 ||
        !DeleteFileW(uniquePath) || !CreateDirectoryW(uniquePath, nullptr)) {
        Expect(false, "Windows test directory can be created");
        return;
    }

    const std::filesystem::path directory(uniquePath);
    int cancellationChecks = 0;
    const auto cancelBeforeNetwork = [&cancellationChecks]() {
        ++cancellationChecks;
        return cancellationChecks >= 2;
    };
    const auto valid =
        codex_monitor::update::DownloadWindowsUpdateAsset(
            url, filename, 1024, directory, cancelBeforeNetwork);
    Expect(!valid.succeeded &&
               valid.failure == codex_monitor::update::
                                    UpdateAssetDownloadFailureKind::kCancelled,
           "canonical local directory is locked before network access");
    Expect(cancellationChecks == 2,
           "directory policy test cancels before opening a connection");

    const auto neverCancel = []() { return false; };
    const auto dotted =
        codex_monitor::update::DownloadWindowsUpdateAsset(
            url, filename, 1024, directory / L".", neverCancel);
    Expect(dotted.failure == codex_monitor::update::
                                 UpdateAssetDownloadFailureKind::kFileSystem,
           "non-canonical dotted directory is rejected");

    const auto relative =
        codex_monitor::update::DownloadWindowsUpdateAsset(
            url, filename, 1024, std::filesystem::path(L"relative"),
            neverCancel);
    Expect(relative.failure == codex_monitor::update::
                                   UpdateAssetDownloadFailureKind::kInvalidInput,
           "relative directory is rejected");

    const auto network =
        codex_monitor::update::DownloadWindowsUpdateAsset(
            url, filename, 1024,
            std::filesystem::path(L"\\\\localhost\\share\\update"),
            neverCancel);
    Expect(network.failure == codex_monitor::update::
                                  UpdateAssetDownloadFailureKind::kFileSystem,
           "UNC and network directories are rejected");

    const std::filesystem::path linkPath = directory.parent_path() /
        (directory.filename().wstring() + L"-link");
    const std::filesystem::path childPath = directory / L"child";
    const bool childCreated =
        CreateDirectoryW(childPath.c_str(), nullptr) != FALSE;
    const DWORD linkFlags = SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2U;
    if (childCreated &&
        CreateSymbolicLinkW(linkPath.c_str(), directory.c_str(), linkFlags)) {
        const auto reparse =
            codex_monitor::update::DownloadWindowsUpdateAsset(
                url, filename, 1024, linkPath / L"child", neverCancel);
        Expect(reparse.failure == codex_monitor::update::
                                      UpdateAssetDownloadFailureKind::kFileSystem,
               "reparse-point ancestor is rejected");
        RemoveDirectoryW(linkPath.c_str());
    }
    if (childCreated) RemoveDirectoryW(childPath.c_str());

    Expect(RemoveDirectoryW(directory.c_str()) != FALSE,
           "Windows test directory is removed");
}

#endif

}  // namespace

int main() {
    constexpr std::string_view kInstaller =
        "CodexMonitorHUD-windows-x64-1.2.3.msi";
    constexpr std::string_view kInitial =
        "https://github.com/Ryuaaa/codex-monitor-hud/releases/download/"
        "v1.2.3/CodexMonitorHUD-windows-x64-1.2.3.msi";

    ExpectInitialAccepted(kInitial, kInstaller,
                          "canonical installer URL is accepted");
    ExpectInitialAccepted(
        "HTTPS://GITHUB.COM/Ryuaaa/codex-monitor-hud/releases/download/"
        "1.2.3/CodexMonitorHUD-windows-x64-1.2.3.msi",
        kInstaller, "scheme and host are case-insensitive");
    ExpectInitialAccepted(
        "https://github.com/Ryuaaa/codex-monitor-hud/releases/download/"
        "v1.2.3/CodexMonitorHUD-windows-x64-1.2.3.msi.sha256",
        "CodexMonitorHUD-windows-x64-1.2.3.msi.sha256",
        "checksum URL is accepted");

    ExpectInitialRejected(
        "http://github.com/Ryuaaa/codex-monitor-hud/releases/download/"
        "v1.2.3/CodexMonitorHUD-windows-x64-1.2.3.msi",
        kInstaller, "HTTP is rejected");
    ExpectInitialRejected(
        "https://github.com.evil.test/Ryuaaa/codex-monitor-hud/releases/"
        "download/v1.2.3/CodexMonitorHUD-windows-x64-1.2.3.msi",
        kInstaller, "host suffix attack is rejected");
    ExpectInitialRejected(
        "https://user@github.com/Ryuaaa/codex-monitor-hud/releases/download/"
        "v1.2.3/CodexMonitorHUD-windows-x64-1.2.3.msi",
        kInstaller, "credentials are rejected");
    ExpectInitialRejected(
        "https://github.com:443/Ryuaaa/codex-monitor-hud/releases/download/"
        "v1.2.3/CodexMonitorHUD-windows-x64-1.2.3.msi",
        kInstaller, "explicit ports are rejected");
    ExpectInitialRejected(
        "https://github.com/Ryuaaa/other/releases/download/v1.2.3/"
        "CodexMonitorHUD-windows-x64-1.2.3.msi",
        kInstaller, "another repository is rejected");
    ExpectInitialRejected(
        "https://github.com/Ryuaaa/codex-monitor-hud/releases/download/"
        "v1.2.3/Other.msi",
        kInstaller, "a mismatched filename is rejected");
    ExpectInitialRejected(
        "https://github.com/Ryuaaa/codex-monitor-hud/releases/download/"
        "v2.0.0/CodexMonitorHUD-windows-x64-1.2.3.msi",
        kInstaller, "a tag and filename version mismatch is rejected");
    ExpectInitialRejected(
        "https://github.com/Ryuaaa/codex-monitor-hud/releases/download/"
        "v1.2.3/Other.msi",
        "Other.msi", "an arbitrary expected release asset is rejected");
    ExpectInitialRejected(
        "https://github.com/Ryuaaa/codex-monitor-hud/releases/download/"
        "v1.2.3/CodexMonitorHUD-windows-x64-1.2.3.msi?raw=1",
        kInstaller, "initial URL queries are rejected");
    ExpectInitialRejected(
        "https://github.com/Ryuaaa/codex-monitor-hud/releases/download/"
        "v1.2.3/CodexMonitorHUD-windows-x64-1.2.3.msi#fragment",
        kInstaller, "initial URL fragments are rejected");
    ExpectInitialRejected(
        "https://github.com/Ryuaaa/codex-monitor-hud/releases/download/"
        "v1%2F2/CodexMonitorHUD-windows-x64-1.2.3.msi",
        kInstaller, "escaped tag separators are rejected");
    ExpectInitialRejected(kInitial, "../installer.msi",
                          "path-like expected filenames are rejected");
    ExpectInitialRejected(kInitial, "installer.msi:stream",
                          "alternate data stream filenames are rejected");
    ExpectInitialRejected(
        "https://github.com/Ryuaaa/codex-monitor-hud/releases/download/"
        "v1.2.3\\CodexMonitorHUD-windows-x64-1.2.3.msi",
        kInstaller, "backslashes are rejected");

    ExpectRedirectAccepted(
        "https://release-assets.githubusercontent.com/"
        "github-production-release-asset/123456/abcdef?sp=r&sig=abc%2B123",
        "canonical signed CDN redirect is accepted");
    ExpectRedirectAccepted(
        "HTTPS://RELEASE-ASSETS.GITHUBUSERCONTENT.COM/"
        "github-production-release-asset/123456/abcdef",
        "CDN scheme and host are case-insensitive");

    ExpectRedirectRejected(
        "http://release-assets.githubusercontent.com/"
        "github-production-release-asset/123/abc?sig=x",
        "redirect HTTP is rejected");
    ExpectRedirectRejected(
        "https://release-assets.githubusercontent.com.evil.test/"
        "github-production-release-asset/123/abc?sig=x",
        "redirect host suffix attack is rejected");
    ExpectRedirectRejected(
        "https://user@release-assets.githubusercontent.com/"
        "github-production-release-asset/123/abc?sig=x",
        "redirect credentials are rejected");
    ExpectRedirectRejected(
        "https://release-assets.githubusercontent.com:443/"
        "github-production-release-asset/123/abc?sig=x",
        "redirect explicit ports are rejected");
    ExpectRedirectRejected(
        "https://objects.githubusercontent.com/"
        "github-production-release-asset/123/abc?sig=x",
        "unlisted content hosts are rejected");
    ExpectRedirectRejected(
        "https://release-assets.githubusercontent.com/other/123/abc?sig=x",
        "non-release CDN paths are rejected");
    ExpectRedirectRejected(
        "https://release-assets.githubusercontent.com/"
        "github-production-release-asset/123/../abc?sig=x",
        "dot segments are rejected");
    ExpectRedirectRejected(
        "https://release-assets.githubusercontent.com/"
        "github-production-release-asset/123/%2e%2e/abc?sig=x",
        "escaped dot segments are rejected");
    ExpectRedirectRejected(
        "https://release-assets.githubusercontent.com/"
        "github-production-release-asset/123%2Fabc?sig=x",
        "escaped path separators are rejected");
    ExpectRedirectRejected(
        "https://release-assets.githubusercontent.com/"
        "github-production-release-asset/123/abc?sig=x#fragment",
        "redirect fragments are rejected");
    ExpectRedirectRejected(
        "https://release-assets.githubusercontent.com/"
        "github-production-release-asset/123/abc?sig=%ZZ",
        "malformed query escapes are rejected");
    ExpectRedirectRejected(
        "https://release-assets.githubusercontent.com/"
        "github-production-release-asset/123/abc\r\nX-Test:bad",
        "header injection characters are rejected");

#ifdef _WIN32
    RunWindowsDirectoryPolicyTests(kInitial, kInstaller);
#endif

    if (failures != 0) {
        std::cerr << failures << " update asset URL test(s) failed\n";
        return 1;
    }
    std::cout << "All update asset URL tests passed\n";
    return 0;
}
