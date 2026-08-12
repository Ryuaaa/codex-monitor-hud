#include "update/github_release_selector.h"

#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using codex_monitor::update::CompareSemVerPrecedence;
using codex_monitor::update::GitHubReleaseAsset;
using codex_monitor::update::GitHubReleaseCandidate;
using codex_monitor::update::ParseSemVerTag;
using codex_monitor::update::SelectLatestWindowsRelease;

int failures = 0;

void Expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

GitHubReleaseAsset Asset(std::string name,
                         std::string_view tag = "unrelated") {
    return {
        name,
        "https://github.com/Ryuaaa/codex-monitor-hud/releases/download/" +
            std::string(tag) + "/" + name,
    };
}

GitHubReleaseCandidate Release(
    std::string tag,
    std::vector<GitHubReleaseAsset> assets,
    bool draft = false,
    bool prerelease = false) {
    return {std::move(tag), draft, prerelease, std::move(assets)};
}

std::vector<GitHubReleaseAsset> Pair(
    std::string_view version,
    std::string_view tag = {}) {
    if (tag.empty()) tag = version;
    const std::string installer =
        "CodexMonitorHUD-windows-x64-" + std::string(version) + ".msi";
    return {Asset(installer, tag), Asset(installer + ".sha256", tag)};
}

void TestStrictSemVerParsing() {
    constexpr std::string_view valid[] = {
        "0.0.0",
        "v1.2.3",
        "1.0.0-alpha",
        "1.0.0-alpha.1+build.009",
        "999999999999999999999999.2.3",
    };
    for (std::string_view value : valid) {
        Expect(ParseSemVerTag(value).has_value(),
               "a valid strict SemVer tag must parse");
    }

    constexpr std::string_view invalid[] = {
        "", "v", "V1.2.3", "1", "1.2", "01.2.3", "1.02.3",
        "1.2.03", "1.2.3-", "1.2.3-alpha..1", "1.2.3-01",
        "1.2.3+", "1.2.3+build..1", " 1.2.3", "1.2.3 ",
        "1.2.3_alpha", "vv1.2.3",
    };
    for (std::string_view value : invalid) {
        Expect(!ParseSemVerTag(value).has_value(),
               "an invalid or non-canonical SemVer tag must be rejected");
    }

    const auto prefixed = ParseSemVerTag("v12.34.56+ci.7");
    Expect(prefixed.has_value() && prefixed->canonical == "12.34.56+ci.7",
           "the accepted v prefix must not enter the canonical version");
}

void TestSemVerPrecedence() {
    constexpr std::string_view ordered[] = {
        "1.0.0-alpha",
        "1.0.0-alpha.1",
        "1.0.0-alpha.beta",
        "1.0.0-beta",
        "1.0.0-beta.2",
        "1.0.0-beta.11",
        "1.0.0-rc.1",
        "1.0.0",
        "2.0.0",
    };
    for (std::size_t index = 1; index < std::size(ordered); ++index) {
        const auto lower = ParseSemVerTag(ordered[index - 1]);
        const auto higher = ParseSemVerTag(ordered[index]);
        Expect(lower.has_value() && higher.has_value() &&
                   CompareSemVerPrecedence(*lower, *higher) < 0,
               "SemVer precedence must follow the specification");
    }

    const auto firstBuild = ParseSemVerTag("1.2.3+build.1");
    const auto secondBuild = ParseSemVerTag("1.2.3+build.2");
    Expect(firstBuild.has_value() && secondBuild.has_value() &&
               CompareSemVerPrecedence(*firstBuild, *secondBuild) == 0,
           "build metadata must not affect SemVer precedence");
}

void TestSelectsHighestStableUpdate() {
    std::vector<GitHubReleaseCandidate> releases;
    releases.push_back(Release("windows-v1.2.0", Pair("1.2.0", "windows-v1.2.0")));
    releases.push_back(Release("windows-v1.10.0", Pair("1.10.0", "windows-v1.10.0")));
    releases.push_back(Release("windows-v2.0.0", Pair("2.0.0", "windows-v2.0.0"), true, false));
    releases.push_back(Release("windows-v2.1.0", Pair("2.1.0", "windows-v2.1.0"), false, true));
    releases.push_back(Release("windows-v3.0.0-rc.1", Pair("3.0.0-rc.1", "windows-v3.0.0-rc.1")));

    const auto selected = SelectLatestWindowsRelease("v1.0.0", releases);
    Expect(selected.has_value() && selected->version.canonical == "1.10.0",
           "the highest non-draft stable release must be selected");
    Expect(selected.has_value() &&
               selected->installer.name ==
                   "CodexMonitorHUD-windows-x64-1.10.0.msi" &&
               selected->checksum.name ==
                   "CodexMonitorHUD-windows-x64-1.10.0.msi.sha256",
           "the selected result must retain the exact installer pair");
}

void TestRejectsUnsafeAssetSets() {
    const std::vector<GitHubReleaseCandidate> releases = {
        Release("windows-v1.8.0", {Asset(
            "CodexMonitorHUD-windows-x64-1.8.0.msi", "windows-v1.8.0")}),
        Release("windows-v1.7.0", {
            Asset("CodexMonitorHUD-windows-x64-1.7.0.msi", "windows-v1.7.0"),
            Asset("CodexMonitorHUD-windows-x64-1.7.1.msi.sha256", "windows-v1.7.0"),
        }),
        Release("windows-v1.6.0", {
            Asset("CodexMonitorHUD-windows-x64-1.6.0.msi", "windows-v1.6.0"),
            Asset("CodexMonitorHUD-windows-x64-1.6.0.msi", "windows-v1.6.0"),
            Asset("CodexMonitorHUD-windows-x64-1.6.0.msi.sha256", "windows-v1.6.0"),
        }),
        Release("windows-v1.5.0", {
            Asset("CodexMonitorHUD-windows-x64-1.5.0.msi", "windows-v1.5.0"),
            Asset("CodexMonitorHUD-windows-x64-1.5.0.msi.sha256", "windows-v1.5.0"),
            Asset("CodexMonitorHUD-windows-x64-9.9.9.msi", "windows-v1.5.0"),
        }),
        Release("windows-v1.4.0", {
            {"CodexMonitorHUD-windows-x64-1.4.0.msi", ""},
            Asset("CodexMonitorHUD-windows-x64-1.4.0.msi.sha256", "windows-v1.4.0"),
        }),
        Release("windows-v1.3.0", {
            {"CodexMonitorHUD-windows-x64-1.3.0.msi",
             "https://example.invalid/CodexMonitorHUD-windows-x64-1.3.0.msi"},
            Asset("CodexMonitorHUD-windows-x64-1.3.0.msi.sha256", "windows-v1.3.0"),
        }),
    };

    Expect(!SelectLatestWindowsRelease("1.0.0", releases).has_value(),
           "missing, mismatched, duplicate, or unusable assets must reject a release");
}

void TestRequiresANewerVersionAndValidCurrentVersion() {
    const std::vector<GitHubReleaseCandidate> releases = {
        Release("windows-v1.0.0", Pair("1.0.0", "windows-v1.0.0")),
        Release("windows-v0.9.9", Pair("0.9.9", "windows-v0.9.9")),
    };
    Expect(!SelectLatestWindowsRelease("1.0.0", releases).has_value(),
           "equal and older releases must not be selected");
    Expect(!SelectLatestWindowsRelease("not-a-version", releases).has_value(),
           "an invalid current version must fail closed");
}

void TestIgnoresUnrelatedAssetsAndInputOrder() {
    std::vector<GitHubReleaseAsset> lowerAssets = Pair("2.0.0", "windows-v2.0.0");
    lowerAssets.push_back(Asset("source.tar.gz"));
    const std::vector<GitHubReleaseCandidate> releases = {
        Release("windows-v2.0.1", Pair("2.0.1", "windows-v2.0.1")),
        Release("windows-v2.0.0", std::move(lowerAssets)),
        Release("windows-v2.1.0+build.1", Pair("2.1.0+build.1", "windows-v2.1.0+build.1")),
    };
    const auto selected = SelectLatestWindowsRelease("1.9.0", releases);
    Expect(selected.has_value() &&
               selected->version.canonical == "2.0.1",
           "input order and non-installable build metadata must not affect selection");
}

}  // namespace

int main() {
    TestStrictSemVerParsing();
    TestSemVerPrecedence();
    TestSelectsHighestStableUpdate();
    TestRejectsUnsafeAssetSets();
    TestRequiresANewerVersionAndValidCurrentVersion();
    TestIgnoresUnrelatedAssetsAndInputOrder();
    if (failures != 0) return 1;
    std::cout << "github_release_selector_tests=pass\n";
    return 0;
}
