#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <tlhelp32.h>

#include "codex/codex_app_server_client.h"

#include <winrt/base.h>

#include <chrono>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

using codex_monitor::codex::AccountData;
using codex_monitor::codex::AppServerClientFailureKind;
using codex_monitor::codex::AppServerRefreshReport;
using codex_monitor::codex::CodexAppServerClient;
using codex_monitor::codex::CodexDataState;
using codex_monitor::codex::ProcessLocalThread;
using codex_monitor::codex::ProcessLocalThreadStatus;
using namespace std::chrono_literals;

int failures = 0;

void Expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

template <typename T, typename = void>
struct HasEmailMember : std::false_type {};
template <typename T>
struct HasEmailMember<T, std::void_t<decltype(std::declval<T>().email)>>
    : std::true_type {};
template <typename T, typename = void>
struct HasPreviewMember : std::false_type {};
template <typename T>
struct HasPreviewMember<T, std::void_t<decltype(std::declval<T>().preview)>>
    : std::true_type {};
template <typename T, typename = void>
struct HasPathMember : std::false_type {};
template <typename T>
struct HasPathMember<T, std::void_t<decltype(std::declval<T>().path)>>
    : std::true_type {};
template <typename T, typename = void>
struct HasIdMember : std::false_type {};
template <typename T>
struct HasIdMember<T, std::void_t<decltype(std::declval<T>().id)>>
    : std::true_type {};
template <typename T, typename = void>
struct HasCodexHomeMember : std::false_type {};
template <typename T>
struct HasCodexHomeMember<T, std::void_t<decltype(std::declval<T>().codexHome)>>
    : std::true_type {};

static_assert(!HasEmailMember<AccountData>::value,
              "the protocol client must not acquire an account email field");
static_assert(!HasPreviewMember<ProcessLocalThread>::value,
              "the protocol client must not acquire preview content");
static_assert(!HasPathMember<ProcessLocalThread>::value,
              "the protocol client must not acquire rollout paths");
static_assert(!HasIdMember<ProcessLocalThread>::value,
              "the protocol client must not acquire thread identifiers");
static_assert(!HasCodexHomeMember<CodexDataState>::value,
              "Codex home must not enter the product-facing data state");
static_assert(!HasCodexHomeMember<AppServerRefreshReport>::value,
              "Codex home must not enter refresh reports sent to callers");

std::optional<std::wstring> EnvironmentValue(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) return std::nullopt;
    std::wstring value(required, L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, value.data(), required);
    if (copied == 0 || copied >= required) return std::nullopt;
    value.resize(copied);
    return value;
}

class ScopedFakeScenario {
public:
    explicit ScopedFakeScenario(const wchar_t* value)
        : previous_(EnvironmentValue(L"CODEX_FAKE_SCENARIO")) {
        SetEnvironmentVariableW(L"CODEX_FAKE_SCENARIO", value);
    }
    ~ScopedFakeScenario() {
        SetEnvironmentVariableW(L"CODEX_FAKE_SCENARIO",
                                previous_ ? previous_->c_str() : nullptr);
    }

private:
    std::optional<std::wstring> previous_;
};

std::filesystem::path CurrentExecutableDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                            static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

std::filesystem::path CreateTestDirectory() {
    wchar_t root[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, root);
    if (length == 0 || length >= MAX_PATH) return {};
    const std::filesystem::path path =
        std::filesystem::path(root) /
        (L"CodexMonitorAppServerTests-" + std::to_wstring(GetCurrentProcessId()) +
         L"-" + std::to_wstring(GetTickCount64()));
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return error ? std::filesystem::path{} : path;
}

bool ProcessWithImagePathExists(const std::filesystem::path& executable) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return true;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                         entry.th32ProcessID);
            if (!process) continue;
            std::wstring path(32768, L'\0');
            DWORD length = static_cast<DWORD>(path.size());
            if (QueryFullProcessImageNameW(process, 0, path.data(), &length)) {
                path.resize(length);
                found = _wcsicmp(path.c_str(), executable.c_str()) == 0;
            }
            CloseHandle(process);
            if (found) break;
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

bool WaitForNoMatchingProcess(const std::filesystem::path& executable) {
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (!ProcessWithImagePathExists(executable)) return true;
        std::this_thread::sleep_for(50ms);
    }
    return !ProcessWithImagePathExists(executable);
}

void TestSuccessfulRefresh(const std::filesystem::path& executable) {
    ScopedFakeScenario scenario(L"app-success");
    CodexAppServerClient client;
    const auto report = client.Refresh(executable, "test-version");
    Expect(report.initialized && report.allMethodsCompleted() && !report.failure,
           "a valid app-server refresh must initialize and complete all four methods");
    Expect(client.codexHome() &&
               *client.codexHome() ==
                   std::filesystem::path(L"C:\\Users\\Codex Test\\.codex"),
           "a validated initialize Codex home must stay on the client");
    Expect(client.data().rateLimits.lastValue &&
               client.data().rateLimits.lastValue->primary &&
               client.data().rateLimits.lastValue->primary->usedPercent == 25,
           "rate-limit result must use the existing parser");
    Expect(client.data().account.lastValue &&
               client.data().account.lastValue->planType == L"pro",
           "account result must retain only the allowed plan type");
    Expect(client.data().usage.lastValue &&
               client.data().usage.lastValue->summary.lifetimeTokens == 90000,
           "usage result must use the existing parser");
    Expect(client.data().threadList.lastValue &&
               client.data().threadList.lastValue->threads.size() == 1 &&
               client.data().threadList.lastValue->threads[0].processLocalStatus ==
                   ProcessLocalThreadStatus::kActive,
           "thread status must remain explicitly scoped to this app-server process");
}

void TestCodexHomeMissingAndUntrustedValuesAreNotRetained(
    const std::filesystem::path& executable) {
    CodexAppServerClient client;
    {
        ScopedFakeScenario scenario(L"app-success");
        Expect(client.Refresh(executable, "test-version").allMethodsCompleted() &&
                   client.codexHome().has_value(),
               "the Codex-home clearing test needs a valid baseline");
    }
    {
        ScopedFakeScenario scenario(L"app-init-missing-home");
        const auto report = client.Refresh(executable, "test-version");
        Expect(report.initialized && report.allMethodsCompleted() && !report.failure,
               "older initialize results without Codex home must remain compatible");
        Expect(!client.codexHome(),
               "a successful initialize without Codex home must clear stale path state");
    }
    for (const wchar_t* scenarioName : {
             L"app-init-relative-home",
             L"app-init-nul-home",
         }) {
        ScopedFakeScenario scenario(scenarioName);
        const auto report = client.Refresh(executable, "test-version");
        Expect(report.initialized && report.allMethodsCompleted() && !report.failure,
               "an untrusted optional Codex home must not fail compatible refresh data");
        Expect(!client.codexHome(),
               "relative and NUL-bearing Codex homes must never be retained");
    }
}

void TestOutOfOrderResponsesAndNotification(const std::filesystem::path& executable) {
    ScopedFakeScenario scenario(L"app-out-of-order");
    CodexAppServerClient client;
    const auto report = client.Refresh(executable, "test-version");
    Expect(report.allMethodsCompleted() && !report.failure,
           "responses must be matched by id rather than arrival order");
    Expect(report.ignoredNotificationCount == 1,
           "an unrelated app-server notification must be ignored");
    Expect(report.ignoredUnknownIdCount == 1,
           "an unknown numeric response id must be ignored");
}

void TestOneMethodErrorRetainsIndependentState(const std::filesystem::path& executable) {
    CodexAppServerClient client;
    {
        ScopedFakeScenario scenario(L"app-success");
        const auto first = client.Refresh(executable, "test-version");
        Expect(first.allMethodsCompleted(), "the baseline refresh must complete");
    }
    {
        ScopedFakeScenario scenario(L"app-single-error");
        const auto second = client.Refresh(executable, "test-version");
        Expect(second.allMethodsCompleted() && !second.failure,
               "one JSON-RPC method error must not become a session failure");
    }
    Expect(client.data().account.lastValue && client.data().account.lastFailure &&
               client.data().account.lastValue->planType == L"pro",
           "an account method error must retain its own last successful value");
    Expect(client.data().rateLimits.lastValue &&
               !client.data().rateLimits.lastFailure &&
               client.data().usage.lastValue && !client.data().usage.lastFailure &&
               client.data().threadList.lastValue &&
               !client.data().threadList.lastFailure,
           "an account error must not clear or fail the other three methods");
    Expect(client.codexHome().has_value(),
           "a method-level error must not discard a successfully initialized Codex home");
}

void TestInitializeFailureStopsBeforeMethodResults(const std::filesystem::path& executable) {
    CodexAppServerClient client;
    {
        ScopedFakeScenario scenario(L"app-success");
        Expect(client.Refresh(executable, "test-version").allMethodsCompleted(),
               "the initialize-failure retention test needs a successful baseline");
    }
    ScopedFakeScenario scenario(L"app-init-error");
    const auto report = client.Refresh(executable, "test-version");
    Expect(!report.initialized &&
               report.failure == AppServerClientFailureKind::kInitializeRejected,
           "initialize error must fail the handshake");
    Expect(!report.rateLimitsResponseReceived && !report.accountResponseReceived &&
               !report.usageResponseReceived && !report.threadListResponseReceived,
           "method results cannot be reported before initialization succeeds");
    Expect(client.data().rateLimits.lastValue && client.data().rateLimits.lastFailure &&
               client.data().account.lastValue && client.data().account.lastFailure &&
               client.data().usage.lastValue && client.data().usage.lastFailure &&
               client.data().threadList.lastValue && client.data().threadList.lastFailure,
           "initialize failure must mark all four methods while retaining prior values");
    Expect(!client.codexHome(),
           "initialize failure must clear a previously validated Codex home");
}

void TestMalformedEnvelopesAndUnknownIdsAreIsolated(
    const std::filesystem::path& executable) {
    ScopedFakeScenario scenario(L"app-malformed");
    CodexAppServerClient client;
    const auto report = client.Refresh(executable, "test-version");
    Expect(report.allMethodsCompleted() && !report.failure,
           "malformed unrelated envelopes must not erase later valid responses");
    Expect(report.malformedEnvelopeCount == 3,
           "invalid JSON, invalid notification, and result-plus-error must be counted");
    Expect(report.ignoredUnknownIdCount == 2,
           "string and numeric unknown ids must both be ignored");
}

void TestStartFailureMarksAllMethods(const std::filesystem::path& executable) {
    CodexAppServerClient client;
    {
        ScopedFakeScenario scenario(L"app-success");
        Expect(client.Refresh(executable, "test-version").allMethodsCompleted(),
               "the start-failure retention test needs a successful baseline");
    }
    const std::filesystem::path missing = executable.parent_path() / L"missing" /
                                          L"codex.exe";
    const auto report = client.Refresh(missing, "test-version");
    Expect(report.failure == AppServerClientFailureKind::kStartFailed,
           "an unavailable safe executable must report a start failure");
    Expect(client.data().rateLimits.lastValue && client.data().rateLimits.lastFailure &&
               client.data().account.lastValue && client.data().account.lastFailure &&
               client.data().usage.lastValue && client.data().usage.lastFailure &&
               client.data().threadList.lastValue &&
               client.data().threadList.lastFailure,
           "start failure must mark all methods while retaining prior values");
    Expect(!client.codexHome(),
           "transport start failure must clear a previously validated Codex home");
}

void TestCancellationStopsTheJobWithoutMethodFailures(
    const std::filesystem::path& executable) {
    CodexAppServerClient client;
    {
        ScopedFakeScenario scenario(L"app-success");
        Expect(client.Refresh(executable, "test-version").allMethodsCompleted() &&
                   client.codexHome().has_value(),
               "the cancellation clearing test needs a valid baseline");
    }
    ScopedFakeScenario scenario(L"app-cancel");
    const auto cancelAt = std::chrono::steady_clock::now() + 600ms;
    const auto report = client.Refresh(executable, "test-version", [&] {
        return std::chrono::steady_clock::now() >= cancelAt;
    });
    Expect(report.failure == AppServerClientFailureKind::kCancelled,
           "the optional callback must cancel a blocked refresh");
    Expect(!client.data().rateLimits.lastFailure && !client.data().account.lastFailure &&
               !client.data().usage.lastFailure && !client.data().threadList.lastFailure,
           "user cancellation must not masquerade as four interface failures");
    Expect(!client.codexHome(),
           "a refresh cancelled after initialize must clear stale Codex home state");
    Expect(WaitForNoMatchingProcess(executable),
           "cancellation must stop the fake app-server and its descendant job process");
}

void TestResponseLineLimitStopsFlood(const std::filesystem::path& executable) {
    ScopedFakeScenario scenario(L"app-line-flood");
    CodexAppServerClient client;
    const auto report = client.Refresh(executable, "test-version");
    Expect(report.failure == AppServerClientFailureKind::kTransportFailed,
           "more than 512 response lines must fail the bounded transport");
    Expect(report.malformedEnvelopeCount ==
               CodexAppServerClient::kMaximumResponseLines,
           "the client must parse at most the documented response-line limit");
    Expect(client.data().rateLimits.lastFailure && client.data().account.lastFailure &&
               client.data().usage.lastFailure && client.data().threadList.lastFailure,
           "a response flood must fail all unfinished methods");
    Expect(WaitForNoMatchingProcess(executable),
           "a response flood must not leave the fake app-server running");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    const std::filesystem::path fakeServer =
        argc >= 2 ? std::filesystem::path(argv[1])
                  : CurrentExecutableDirectory() / L"fake_codex_app_server.exe";
    const std::filesystem::path testRoot = CreateTestDirectory();
    if (testRoot.empty()) {
        std::cerr << "FAIL: could not create isolated test directory\n";
        return 1;
    }
    const std::filesystem::path serverDirectory = testRoot / L"server with spaces";
    const std::filesystem::path executable = serverDirectory / L"codex.exe";
    std::error_code error;
    std::filesystem::create_directories(serverDirectory, error);
    if (!error) {
        std::filesystem::copy_file(fakeServer, executable,
                                   std::filesystem::copy_options::overwrite_existing, error);
    }
    if (error || !std::filesystem::is_regular_file(executable)) {
        std::cerr << "FAIL: could not prepare fake codex.exe\n";
        return 1;
    }

    TestSuccessfulRefresh(executable);
    TestCodexHomeMissingAndUntrustedValuesAreNotRetained(executable);
    TestOutOfOrderResponsesAndNotification(executable);
    TestOneMethodErrorRetainsIndependentState(executable);
    TestInitializeFailureStopsBeforeMethodResults(executable);
    TestMalformedEnvelopesAndUnknownIdsAreIsolated(executable);
    TestStartFailureMarksAllMethods(executable);
    TestCancellationStopsTheJobWithoutMethodFailures(executable);
    TestResponseLineLimitStopsFlood(executable);

    std::filesystem::remove_all(testRoot, error);
    if (failures != 0) return 1;
    std::cout << "codex_app_server_client_tests=pass\n";
    return 0;
}
