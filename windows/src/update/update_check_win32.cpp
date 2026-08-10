#include "update/update_check_win32.h"

#include "update/github_release_json_win32.h"

#include <utility>

namespace codex_monitor::update {

WindowsUpdateCheckResult EvaluateWindowsUpdateReleaseJson(
    std::string_view currentVersion,
    std::wstring_view json) noexcept {
    try {
        const std::optional<SemanticVersion> current =
            ParseSemVerTag(currentVersion);
        if (!current || !current->IsStable() ||
            !current->buildMetadata.empty()) {
            WindowsUpdateCheckResult result;
            result.status = WindowsUpdateCheckStatus::kInvalidCurrentVersion;
            result.error = L"The installed Windows version is invalid";
            return result;
        }

        std::optional<std::vector<GitHubReleaseCandidate>> releases =
            ParseGitHubReleaseListJson(json);
        if (!releases) {
            WindowsUpdateCheckResult result;
            result.status = WindowsUpdateCheckStatus::kInvalidResponse;
            result.error = L"GitHub returned invalid update information";
            return result;
        }

        WindowsUpdateCheckResult result;
        result.release =
            SelectLatestWindowsRelease(currentVersion, *releases);
        result.status = result.release
                            ? WindowsUpdateCheckStatus::kUpdateAvailable
                            : WindowsUpdateCheckStatus::kUpToDate;
        return result;
    } catch (...) {
        WindowsUpdateCheckResult result;
        result.status = WindowsUpdateCheckStatus::kInvalidResponse;
        result.error = L"Update check failed unexpectedly";
        return result;
    }
}

WindowsUpdateCheckResult CheckForWindowsUpdate(
    std::string_view currentVersion,
    const WindowsUpdateCancellationCheck& cancelled) noexcept {
    try {
        const std::optional<SemanticVersion> current =
            ParseSemVerTag(currentVersion);
        if (!current || !current->IsStable() ||
            !current->buildMetadata.empty()) {
            WindowsUpdateCheckResult result;
            result.status = WindowsUpdateCheckStatus::kInvalidCurrentVersion;
            result.error = L"The installed Windows version is invalid";
            return result;
        }
        WindowsUpdateFetchResult fetched =
            FetchWindowsUpdateReleasesJson(cancelled);
        if (!fetched.succeeded) {
            WindowsUpdateCheckResult result;
            result.status = WindowsUpdateCheckStatus::kFetchFailed;
            result.error = std::move(fetched.error);
            return result;
        }
        return EvaluateWindowsUpdateReleaseJson(currentVersion, fetched.json);
    } catch (...) {
        WindowsUpdateCheckResult result;
        result.status = WindowsUpdateCheckStatus::kInvalidResponse;
        result.error = L"Update check failed unexpectedly";
        return result;
    }
}

}  // namespace codex_monitor::update
