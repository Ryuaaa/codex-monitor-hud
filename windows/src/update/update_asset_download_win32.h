#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace codex_monitor::update {

// The small parsed surface used by WinHTTP. These parsers are deliberately
// independent of WinHTTP so their allow-list behavior can be tested on every
// development platform.
struct AllowedUpdateAssetUrl {
    std::wstring host;
    std::wstring pathAndQuery;
};

// Accepts only the exact public GitHub release-download shape for this
// repository and the expected base filename. Credentials, ports, queries,
// fragments, escapes, and non-canonical path segments are rejected.
[[nodiscard]] std::optional<AllowedUpdateAssetUrl>
ParseInitialUpdateAssetUrl(
    std::string_view url,
    std::string_view expectedFilename) noexcept;

// Accepts only an absolute HTTPS redirect to GitHub's release-asset CDN. The
// CDN path and signed query remain opaque, but credentials, ports, fragments,
// backslashes, malformed escapes, and non-canonical path segments are rejected.
[[nodiscard]] std::optional<AllowedUpdateAssetUrl>
ParseGitHubReleaseAssetRedirectUrl(
    std::string_view url) noexcept;

enum class UpdateAssetDownloadFailureKind {
    kNone,
    kInvalidInput,
    kCancelled,
    kNetwork,
    kHttp,
    kRedirectRejected,
    kResponseTooLarge,
    kFileSystem,
    kUnexpected,
};

struct UpdateAssetDownloadResult {
    bool succeeded = false;
    std::filesystem::path filePath;
    std::uint64_t bytesWritten = 0;
    UpdateAssetDownloadFailureKind failure =
        UpdateAssetDownloadFailureKind::kUnexpected;
    std::wstring error;
};

using UpdateAssetDownloadCancellationCheck = std::function<bool()>;

// Downloads but never opens or executes an update asset. The destination
// directory must already exist as a canonical absolute path on a local fixed
// disk. Every ancestor is held open and must be a non-reparse directory. The
// output file is created with CREATE_NEW and without write/delete sharing.
// A partial file is removed on every failure. Exactly one allow-listed redirect
// may be followed manually; automatic redirects, cookies, and authentication
// are disabled.
[[nodiscard]] UpdateAssetDownloadResult DownloadWindowsUpdateAsset(
    std::string_view browserDownloadUrl,
    std::string_view expectedFilename,
    std::uint64_t maximumBytes,
    const std::filesystem::path& privateDirectory,
    const UpdateAssetDownloadCancellationCheck& cancelled = {}) noexcept;

}  // namespace codex_monitor::update
