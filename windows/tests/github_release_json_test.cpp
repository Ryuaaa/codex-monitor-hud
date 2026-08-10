#include "update/github_release_json_win32.h"

#include <winrt/base.h>

#include <iostream>
#include <string>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

void TestReleaseList() {
    const auto parsed =
        codex_monitor::update::ParseGitHubReleaseListJson(LR"json([
          {
            "tag_name":"v1.2.3",
            "draft":false,
            "prerelease":false,
            "body":"ignored",
            "assets":[
              {
                "name":"CodexMonitorHUD-windows-x64-1.2.3.msi",
                "browser_download_url":"https://github.com/Ryuaaa/codex-monitor-hud/releases/download/v1.2.3/CodexMonitorHUD-windows-x64-1.2.3.msi",
                "size":123
              },
              {
                "name":"CodexMonitorHUD-windows-x64-1.2.3.msi.sha256",
                "browser_download_url":"https://github.com/Ryuaaa/codex-monitor-hud/releases/download/v1.2.3/CodexMonitorHUD-windows-x64-1.2.3.msi.sha256"
              }
            ]
          }
        ])json");
    Expect(parsed && parsed->size() == 1 &&
               parsed->front().tagName == "v1.2.3" &&
               !parsed->front().draft && !parsed->front().prerelease &&
               parsed->front().assets.size() == 2,
           "the parser must retain only the update selector fields");
}

void TestEmptyListIsValid() {
    const auto parsed =
        codex_monitor::update::ParseGitHubReleaseListJson(L"[]");
    Expect(parsed && parsed->empty(),
           "a repository with no releases is a valid empty result");
}

void TestMalformedAndWrongTypesFail() {
    Expect(!codex_monitor::update::ParseGitHubReleaseListJson(L"{}"),
           "the GitHub release root must be an array");
    Expect(!codex_monitor::update::ParseGitHubReleaseListJson(
               LR"json([{"tag_name":"1.0.0","draft":0,"prerelease":false,"assets":[]}])json"),
           "known boolean fields must not accept numbers");
    Expect(!codex_monitor::update::ParseGitHubReleaseListJson(
               LR"json([{"tag_name":"1.0.0","draft":false,"prerelease":false,"assets":[{"name":"x"}]}])json"),
           "every retained asset must include a download URL");
}

void TestLimitsAndControlCharactersFail() {
    std::wstring tooMany = L"[";
    for (int index = 0; index < 21; ++index) {
        if (index != 0) tooMany += L',';
        tooMany += LR"json({"tag_name":"1.0.0","draft":false,"prerelease":false,"assets":[]})json";
    }
    tooMany += L']';
    Expect(!codex_monitor::update::ParseGitHubReleaseListJson(tooMany),
           "more than twenty releases must fail closed");
    Expect(!codex_monitor::update::ParseGitHubReleaseListJson(
               LR"json([{"tag_name":"1.0.0\u0000x","draft":false,"prerelease":false,"assets":[]}])json"),
           "embedded control characters must be rejected");
}

}  // namespace

int main() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    TestReleaseList();
    TestEmptyListIsValid();
    TestMalformedAndWrongTypesFail();
    TestLimitsAndControlCharactersFail();
    if (failures != 0) return 1;
    std::cout << "github_release_json_tests=pass\n";
    return 0;
}
