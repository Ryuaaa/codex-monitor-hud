#include "update/update_check_win32.h"

#include <winrt/base.h>

#include <iostream>

namespace {

using codex_monitor::update::EvaluateWindowsUpdateReleaseJson;
using codex_monitor::update::WindowsUpdateCheckStatus;

int failures = 0;

void Expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

std::wstring Release(std::wstring tag, std::wstring version) {
    const std::wstring base =
        L"https://github.com/Ryuaaa/codex-monitor-hud/releases/download/" +
        tag + L"/CodexMonitorHUD-windows-x64-" + version + L".msi";
    return L"[{\"tag_name\":\"" + tag +
           L"\",\"draft\":false,\"prerelease\":false,\"assets\":[" +
           L"{\"name\":\"CodexMonitorHUD-windows-x64-" + version +
           L".msi\",\"browser_download_url\":\"" + base + L"\"}," +
           L"{\"name\":\"CodexMonitorHUD-windows-x64-" + version +
           L".msi.sha256\",\"browser_download_url\":\"" + base +
           L".sha256\"}]}]";
}

void TestUpdateAndUpToDate() {
    const auto available = EvaluateWindowsUpdateReleaseJson(
        "1.0.0", Release(L"windows-v1.2.0", L"1.2.0"));
    Expect(available.status == WindowsUpdateCheckStatus::kUpdateAvailable &&
               available.release &&
               available.release->version.canonical == "1.2.0",
           "a newer signed-asset candidate must be surfaced");

    const auto current = EvaluateWindowsUpdateReleaseJson(
        "1.2.0", Release(L"windows-v1.2.0", L"1.2.0"));
    Expect(current.status == WindowsUpdateCheckStatus::kUpToDate &&
               !current.release,
           "an equal Windows release is up to date");
}

void TestMacOnlyAndMalformed() {
    const auto macOnly = EvaluateWindowsUpdateReleaseJson(
        "1.0.0",
        LR"json([{"tag_name":"v2.0.0","draft":false,"prerelease":false,"assets":[{"name":"Codex-Monitor-HUD.app.zip","browser_download_url":"https://github.com/Ryuaaa/codex-monitor-hud/releases/download/v2.0.0/Codex-Monitor-HUD.app.zip"}]}])json");
    Expect(macOnly.status == WindowsUpdateCheckStatus::kUpToDate,
           "a newer macOS-only release must not be offered to Windows");

    const auto malformed =
        EvaluateWindowsUpdateReleaseJson("1.0.0", L"not-json");
    Expect(malformed.status == WindowsUpdateCheckStatus::kInvalidResponse,
           "malformed release JSON must fail closed");
    const auto invalidCurrent =
        EvaluateWindowsUpdateReleaseJson("development", L"[]");
    Expect(invalidCurrent.status ==
               WindowsUpdateCheckStatus::kInvalidCurrentVersion,
           "an invalid installed version must fail closed");
}

}  // namespace

int main() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    TestUpdateAndUpToDate();
    TestMacOnlyAndMalformed();
    if (failures != 0) return 1;
    std::cout << "update_check_tests=pass\n";
    return 0;
}
