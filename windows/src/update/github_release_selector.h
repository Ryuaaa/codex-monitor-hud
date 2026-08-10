#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace codex_monitor::update {

struct SemVerIdentifier {
    std::string value;
    bool numeric = false;
};

// A syntactically valid Semantic Version 2.0.0 tag. The optional leading
// lowercase "v" accepted by ParseSemVerTag is not part of canonical.
struct SemanticVersion {
    std::string major;
    std::string minor;
    std::string patch;
    std::vector<SemVerIdentifier> prerelease;
    std::vector<std::string> buildMetadata;
    std::string canonical;

    bool IsStable() const noexcept { return prerelease.empty(); }
};

// Parses the complete input using SemVer 2.0.0 syntax. A single lowercase
// leading "v" is also accepted. Whitespace and partial versions are rejected.
std::optional<SemanticVersion> ParseSemVerTag(std::string_view tag);

// Returns a negative value when lhs has lower SemVer precedence, zero when the
// versions have equal precedence, and a positive value otherwise. Build
// metadata does not affect precedence, as required by SemVer 2.0.0.
int CompareSemVerPrecedence(
    const SemanticVersion& lhs,
    const SemanticVersion& rhs) noexcept;

struct GitHubReleaseAsset {
    std::string name;
    std::string browserDownloadUrl;
};

struct GitHubReleaseCandidate {
    std::string tagName;
    bool draft = false;
    bool prerelease = false;
    std::vector<GitHubReleaseAsset> assets;
};

struct SelectedWindowsRelease {
    SemanticVersion version;
    std::string tagName;
    GitHubReleaseAsset installer;
    GitHubReleaseAsset checksum;
};

// Selects the highest stable release newer than currentVersion. A candidate is
// eligible only when it contains exactly one installer named
// CodexMonitorHUD-windows-x64-<version>.msi and exactly one adjacent checksum
// asset named <installer>.sha256. Relevant x64 assets carrying another version,
// duplicate assets, empty download URLs, drafts, and prereleases are rejected.
std::optional<SelectedWindowsRelease> SelectLatestWindowsRelease(
    std::string_view currentVersion,
    const std::vector<GitHubReleaseCandidate>& releases);

}  // namespace codex_monitor::update
