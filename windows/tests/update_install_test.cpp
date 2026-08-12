#include "update/update_install_win32.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace codex_monitor::update;

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

WindowsUpdateInstallRequest ValidRequest() {
    const auto version = ParseSemVerTag("1.2.0");
    Require(version.has_value(), "fixture version must parse");
    WindowsUpdateInstallRequest request;
    request.currentVersion = "1.1.0";
#ifdef _WIN32
    request.updatesRoot = L"C:\\CodexMonitorUpdates";
#else
    request.updatesRoot = "/tmp/codex-monitor-updates";
#endif
    request.release.version = *version;
    request.release.tagName = "windows-v1.2.0";
    request.release.installer = {
        "CodexMonitorHUD-windows-x64-1.2.0.msi",
        "https://github.com/Ryuaaa/codex-monitor-hud/releases/download/"
        "windows-v1.2.0/CodexMonitorHUD-windows-x64-1.2.0.msi"};
    request.release.checksum = {
        "CodexMonitorHUD-windows-x64-1.2.0.msi.sha256",
        "https://github.com/Ryuaaa/codex-monitor-hud/releases/download/"
        "windows-v1.2.0/CodexMonitorHUD-windows-x64-1.2.0.msi.sha256"};
    return request;
}

WindowsUpdateInstallOperations SuccessfulOperations(
    std::vector<int>* order,
    const WindowsUpdateInstallRequest& request) {
    WindowsUpdateInstallOperations operations;
    operations.publisherConfigured = [order] {
        order->push_back(1);
        return true;
    };
    operations.createPrivateDirectory = [order](const auto& root) {
        order->push_back(2);
        return std::optional<std::filesystem::path>(root / "private");
    };
    operations.downloadAsset =
        [order, &request](const GitHubReleaseAsset& asset,
                          std::uint64_t maximumBytes,
                          const std::filesystem::path& directory,
                          const auto&) {
            UpdateAssetDownloadResult result;
            result.succeeded = true;
            result.failure = UpdateAssetDownloadFailureKind::kNone;
            result.filePath = directory / asset.name;
            if (asset.name == request.release.checksum.name) {
                order->push_back(3);
                Require(maximumBytes == kMaximumSha256ManifestBytes,
                        "checksum has the small fixed download limit");
            } else {
                order->push_back(5);
                Require(maximumBytes == kMaximumWindowsInstallerBytes,
                        "MSI has the fixed installer size limit");
            }
            return result;
        };
    operations.readSmallFile = [order, &request](const auto&, std::size_t limit) {
        order->push_back(4);
        Require(limit == kMaximumSha256ManifestBytes,
                "manifest read keeps the checksum size limit");
        return std::optional<std::string>(
            std::string(64U, 'a') + "  " +
            request.release.installer.name + "\n");
    };
    operations.launchHelper = [order, &request](const auto& launch) {
        order->push_back(6);
        Require(launch.currentVersion == request.currentVersion &&
                    launch.targetVersion ==
                        request.release.version.canonical &&
                    launch.installerSha256 == std::string(64U, 'a') &&
                    launch.installerPath.parent_path() ==
                        launch.privateUpdateDirectory,
                "only the exact verified release reaches the helper");
        WindowsUpdateHelperLauncherResult result;
        result.status = WindowsUpdateHelperLauncherStatus::kStarted;
        return result;
    };
    return operations;
}

void TestSuccessfulPlanIsStrictlyOrdered() {
    WindowsUpdateInstallRequest request = ValidRequest();
    std::vector<int> order;
    const auto result = RunWindowsUpdateInstallPlan(
        request, SuccessfulOperations(&order, request));
    Require(result.helperStarted() &&
                order == std::vector<int>({1, 2, 3, 4, 5, 6}),
            "publisher, checksum, MSI, and helper must run in strict order");
}

void TestPublisherGateStopsBeforeDiskAndNetwork() {
    WindowsUpdateInstallRequest request = ValidRequest();
    std::vector<int> order;
    auto operations = SuccessfulOperations(&order, request);
    operations.publisherConfigured = [&order] {
        order.push_back(1);
        return false;
    };
    const auto result = RunWindowsUpdateInstallPlan(request, operations);
    Require(result.status ==
                WindowsUpdateInstallStatus::kPublisherNotConfigured &&
                order == std::vector<int>({1}),
            "a build without a publisher pin must not download anything");
}

void TestPortableUiPreflightStopsBeforeDownload() {
    WindowsUpdateInstallPreflight preflight;
    preflight.installWorkerAvailable = true;
    preflight.publisherConfigured = true;
    preflight.freshReleaseAvailable = true;
    preflight.settingsPathAvailable = true;

    Require(!CanRequestWindowsUpdateInstall(preflight),
            "a signed portable HUD must not enqueue an update download");
    preflight.runningFromMsiInstalledHud = true;
    Require(CanRequestWindowsUpdateInstall(preflight),
            "the same ready state must remain available to the MSI install");

    preflight.updateCheckBusy = true;
    Require(!CanRequestWindowsUpdateInstall(preflight),
            "an MSI install cannot start while its update check is busy");
    preflight.updateCheckBusy = false;
    preflight.updateInstallBusy = true;
    Require(!CanRequestWindowsUpdateInstall(preflight),
            "an MSI install cannot enqueue a second update operation");
}

void TestEveryPreparationFailureStopsBeforeHelper() {
    WindowsUpdateInstallRequest request = ValidRequest();

    std::vector<int> checksumOrder;
    auto checksumFailure = SuccessfulOperations(&checksumOrder, request);
    checksumFailure.downloadAsset =
        [&checksumOrder](const auto&, std::uint64_t, const auto&, const auto&) {
            checksumOrder.push_back(3);
            UpdateAssetDownloadResult result;
            result.failure = UpdateAssetDownloadFailureKind::kNetwork;
            return result;
        };
    auto result = RunWindowsUpdateInstallPlan(request, checksumFailure);
    Require(result.status ==
                WindowsUpdateInstallStatus::kChecksumDownloadFailed &&
                checksumOrder == std::vector<int>({1, 2, 3}),
            "checksum download failure must stop before parsing and MSI");

    std::vector<int> malformedOrder;
    auto malformed = SuccessfulOperations(&malformedOrder, request);
    malformed.readSmallFile = [&malformedOrder](const auto&, std::size_t) {
        malformedOrder.push_back(4);
        return std::optional<std::string>("not a checksum manifest");
    };
    result = RunWindowsUpdateInstallPlan(request, malformed);
    Require(result.status == WindowsUpdateInstallStatus::kChecksumRejected &&
                malformedOrder == std::vector<int>({1, 2, 3, 4}),
            "malformed checksum must stop before MSI download");

    std::vector<int> installerOrder;
    auto installerFailure = SuccessfulOperations(&installerOrder, request);
    installerFailure.downloadAsset =
        [&installerOrder, &request](const GitHubReleaseAsset& asset,
                                    std::uint64_t, const auto& directory,
                                    const auto&) {
            UpdateAssetDownloadResult result;
            if (asset.name == request.release.checksum.name) {
                installerOrder.push_back(3);
                result.succeeded = true;
                result.failure = UpdateAssetDownloadFailureKind::kNone;
                result.filePath = directory / asset.name;
            } else {
                installerOrder.push_back(5);
                result.failure = UpdateAssetDownloadFailureKind::kHttp;
            }
            return result;
        };
    result = RunWindowsUpdateInstallPlan(request, installerFailure);
    Require(result.status ==
                WindowsUpdateInstallStatus::kInstallerDownloadFailed &&
                installerOrder == std::vector<int>({1, 2, 3, 4, 5}),
            "MSI failure must never reach the helper");
}

void TestCancellationAndInvalidPathsFailClosed() {
    WindowsUpdateInstallRequest request = ValidRequest();
    std::vector<int> order;
    auto operations = SuccessfulOperations(&order, request);
    const auto cancelled = RunWindowsUpdateInstallPlan(
        request, operations, [] { return true; });
    Require(cancelled.status == WindowsUpdateInstallStatus::kCancelled &&
                order.empty(),
            "pre-cancelled work must not touch publisher, disk, or network");

    order.clear();
    operations = SuccessfulOperations(&order, request);
    operations.createPrivateDirectory = [&order](const auto&) {
        order.push_back(2);
#ifdef _WIN32
        return std::optional<std::filesystem::path>(
            L"C:\\NotTheRequestedRoot\\private");
#else
        return std::optional<std::filesystem::path>(
            "/tmp/not-the-requested-root/private");
#endif
    };
    const auto escaped = RunWindowsUpdateInstallPlan(request, operations);
    Require(escaped.status ==
                WindowsUpdateInstallStatus::kUpdateDirectoryUnavailable &&
                order == std::vector<int>({1, 2}),
            "the fresh directory must remain directly under the update root");

    request.release.installer.name = "different.msi";
    order.clear();
    const auto invalid = RunWindowsUpdateInstallPlan(
        request, SuccessfulOperations(&order, request));
    Require(invalid.status == WindowsUpdateInstallStatus::kInvalidInput &&
                order.empty(),
            "a release with inconsistent asset identity is rejected first");
}

}  // namespace

int main() {
    TestSuccessfulPlanIsStrictlyOrdered();
    TestPublisherGateStopsBeforeDiskAndNetwork();
    TestPortableUiPreflightStopsBeforeDownload();
    TestEveryPreparationFailureStopsBeforeHelper();
    TestCancellationAndInvalidPathsFailClosed();
    std::cout << "update_install_tests=pass\n";
    return 0;
}
