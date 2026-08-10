#include "update/github_release_selector.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace codex_monitor::update {
namespace {

constexpr std::string_view kAssetPrefix =
    "CodexMonitorHUD-windows-x64-";
constexpr std::string_view kInstallerSuffix = ".msi";
constexpr std::string_view kChecksumSuffix = ".msi.sha256";

bool IsAsciiDigit(char value) noexcept {
    return value >= '0' && value <= '9';
}

bool IsAsciiLetter(char value) noexcept {
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z');
}

bool IsIdentifierCharacter(char value) noexcept {
    return IsAsciiDigit(value) || IsAsciiLetter(value) || value == '-';
}

bool ParseCoreNumber(
    std::string_view input,
    std::size_t* position,
    std::string* output) {
    const std::size_t begin = *position;
    while (*position < input.size() && IsAsciiDigit(input[*position])) {
        ++(*position);
    }
    if (*position == begin) return false;
    if (*position - begin > 1 && input[begin] == '0') return false;
    output->assign(input.substr(begin, *position - begin));
    return true;
}

bool ParseIdentifiers(
    std::string_view input,
    std::size_t* position,
    bool prerelease,
    std::vector<SemVerIdentifier>* prereleaseOutput,
    std::vector<std::string>* buildOutput) {
    while (true) {
        const std::size_t begin = *position;
        bool numeric = true;
        while (*position < input.size() &&
               IsIdentifierCharacter(input[*position])) {
            if (!IsAsciiDigit(input[*position])) numeric = false;
            ++(*position);
        }
        if (*position == begin) return false;

        const std::string_view identifier =
            input.substr(begin, *position - begin);
        if (prerelease && numeric && identifier.size() > 1 &&
            identifier.front() == '0') {
            return false;
        }

        if (prerelease) {
            prereleaseOutput->push_back(
                {std::string(identifier), numeric});
        } else {
            buildOutput->emplace_back(identifier);
        }

        if (*position >= input.size() || input[*position] != '.') break;
        ++(*position);
    }
    return true;
}

int CompareNumericText(
    std::string_view lhs,
    std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) return lhs.size() < rhs.size() ? -1 : 1;
    if (lhs == rhs) return 0;
    return lhs < rhs ? -1 : 1;
}

int CompareIdentifiers(
    const SemVerIdentifier& lhs,
    const SemVerIdentifier& rhs) noexcept {
    if (lhs.numeric && rhs.numeric) {
        return CompareNumericText(lhs.value, rhs.value);
    }
    if (lhs.numeric != rhs.numeric) return lhs.numeric ? -1 : 1;
    if (lhs.value == rhs.value) return 0;
    return lhs.value < rhs.value ? -1 : 1;
}

bool StartsWith(std::string_view value, std::string_view prefix) noexcept {
    return value.size() >= prefix.size() &&
           value.substr(0, prefix.size()) == prefix;
}

bool EndsWith(std::string_view value, std::string_view suffix) noexcept {
    return value.size() >= suffix.size() &&
           value.substr(value.size() - suffix.size()) == suffix;
}

bool IsRelevantAsset(std::string_view name) noexcept {
    if (!StartsWith(name, kAssetPrefix)) return false;
    return EndsWith(name, kInstallerSuffix) ||
           EndsWith(name, kChecksumSuffix);
}

struct ValidatedAssets {
    GitHubReleaseAsset installer;
    GitHubReleaseAsset checksum;
};

bool IsAllowedDownloadUrl(const GitHubReleaseAsset& asset,
                          std::string_view tagName) {
    const std::string expected =
        "https://github.com/Ryuaaa/codex-monitor-hud/releases/download/" +
        std::string(tagName) + "/" + asset.name;
    return asset.browserDownloadUrl == expected;
}

std::optional<ValidatedAssets> ValidateAssets(
    const GitHubReleaseCandidate& release,
    const SemanticVersion& version) {
    const std::string installerName =
        std::string(kAssetPrefix) + version.canonical +
        std::string(kInstallerSuffix);
    const std::string checksumName =
        installerName + ".sha256";

    const GitHubReleaseAsset* installer = nullptr;
    const GitHubReleaseAsset* checksum = nullptr;
    for (const GitHubReleaseAsset& asset : release.assets) {
        if (!IsRelevantAsset(asset.name)) continue;

        // Any relevant Windows x64 asset carrying a different version makes
        // the release internally inconsistent, even if a matching pair also
        // exists.
        if (asset.name != installerName && asset.name != checksumName) {
            return std::nullopt;
        }
        if (!IsAllowedDownloadUrl(asset, release.tagName)) {
            return std::nullopt;
        }

        if (asset.name == installerName) {
            if (installer != nullptr) return std::nullopt;
            installer = &asset;
        } else {
            if (checksum != nullptr) return std::nullopt;
            checksum = &asset;
        }
    }

    if (installer == nullptr || checksum == nullptr) return std::nullopt;
    return ValidatedAssets{*installer, *checksum};
}

}  // namespace

std::optional<SemanticVersion> ParseSemVerTag(std::string_view tag) {
    if (tag.empty()) return std::nullopt;
    if (tag.front() == 'v') {
        tag.remove_prefix(1);
        if (tag.empty()) return std::nullopt;
    }

    SemanticVersion result;
    result.canonical.assign(tag);
    std::size_t position = 0;
    if (!ParseCoreNumber(tag, &position, &result.major) ||
        position >= tag.size() || tag[position++] != '.' ||
        !ParseCoreNumber(tag, &position, &result.minor) ||
        position >= tag.size() || tag[position++] != '.' ||
        !ParseCoreNumber(tag, &position, &result.patch)) {
        return std::nullopt;
    }

    if (position < tag.size() && tag[position] == '-') {
        ++position;
        if (!ParseIdentifiers(tag, &position, true,
                              &result.prerelease,
                              &result.buildMetadata)) {
            return std::nullopt;
        }
    }
    if (position < tag.size() && tag[position] == '+') {
        ++position;
        if (!ParseIdentifiers(tag, &position, false,
                              &result.prerelease,
                              &result.buildMetadata)) {
            return std::nullopt;
        }
    }
    if (position != tag.size()) return std::nullopt;
    return result;
}

int CompareSemVerPrecedence(
    const SemanticVersion& lhs,
    const SemanticVersion& rhs) noexcept {
    int comparison = CompareNumericText(lhs.major, rhs.major);
    if (comparison != 0) return comparison;
    comparison = CompareNumericText(lhs.minor, rhs.minor);
    if (comparison != 0) return comparison;
    comparison = CompareNumericText(lhs.patch, rhs.patch);
    if (comparison != 0) return comparison;

    if (lhs.prerelease.empty() != rhs.prerelease.empty()) {
        return lhs.prerelease.empty() ? 1 : -1;
    }
    const std::size_t shared =
        std::min(lhs.prerelease.size(), rhs.prerelease.size());
    for (std::size_t index = 0; index < shared; ++index) {
        comparison = CompareIdentifiers(lhs.prerelease[index],
                                        rhs.prerelease[index]);
        if (comparison != 0) return comparison;
    }
    if (lhs.prerelease.size() == rhs.prerelease.size()) return 0;
    return lhs.prerelease.size() < rhs.prerelease.size() ? -1 : 1;
}

std::optional<SelectedWindowsRelease> SelectLatestWindowsRelease(
    std::string_view currentVersion,
    const std::vector<GitHubReleaseCandidate>& releases) {
    const std::optional<SemanticVersion> current =
        ParseSemVerTag(currentVersion);
    if (!current.has_value()) return std::nullopt;

    std::optional<SelectedWindowsRelease> selected;
    for (const GitHubReleaseCandidate& release : releases) {
        if (release.draft || release.prerelease) continue;

        std::optional<SemanticVersion> version =
            ParseSemVerTag(release.tagName);
        // The WiX packaging pipeline and MSI ProductVersion use exactly three
        // numeric components. Build metadata cannot be represented there, so
        // it is not a valid automatic-install target even though SemVer treats
        // it as stable.
        if (!version.has_value() || !version->IsStable() ||
            !version->buildMetadata.empty()) {
            continue;
        }
        if (CompareSemVerPrecedence(*version, *current) <= 0) continue;

        std::optional<ValidatedAssets> assets =
            ValidateAssets(release, *version);
        if (!assets.has_value()) continue;

        if (selected.has_value()) {
            const int precedence = CompareSemVerPrecedence(
                *version, selected->version);
            if (precedence < 0) continue;
            if (precedence == 0 &&
                version->canonical <= selected->version.canonical) {
                continue;
            }
        }

        selected = SelectedWindowsRelease{
            std::move(*version),
            release.tagName,
            std::move(assets->installer),
            std::move(assets->checksum),
        };
    }
    return selected;
}

}  // namespace codex_monitor::update
