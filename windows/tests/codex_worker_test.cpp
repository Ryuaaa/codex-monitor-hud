#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <tlhelp32.h>

#include "codex/codex_worker.h"

#include <chrono>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

namespace {

using codex_monitor::codex::CodexWorker;
using codex_monitor::codex::CompletedCodexRefresh;
using namespace std::chrono_literals;

constexpr UINT kWorkerReadyMessage = WM_APP + 41;
int failures = 0;

void Stage(const char* value) {
    std::cerr << "worker_test_stage=" << value << '\n';
}

void Expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

std::optional<std::wstring> EnvironmentValue(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) return std::nullopt;
    std::wstring value(required, L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, value.data(), required);
    if (copied == 0 || copied >= required) return std::nullopt;
    value.resize(copied);
    return value;
}

class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(const wchar_t* name, const std::wstring& value)
        : name_(name), previous_(EnvironmentValue(name)) {
        SetEnvironmentVariableW(name_.c_str(), value.c_str());
    }

    ~ScopedEnvironmentVariable() {
        SetEnvironmentVariableW(name_.c_str(),
                                previous_ ? previous_->c_str() : nullptr);
    }

    void Set(const std::wstring& value) {
        SetEnvironmentVariableW(name_.c_str(), value.c_str());
    }

private:
    std::wstring name_;
    std::optional<std::wstring> previous_;
};

std::filesystem::path CreateTestDirectory() {
    wchar_t root[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, root);
    if (length == 0 || length >= MAX_PATH) return {};
    const std::filesystem::path path =
        std::filesystem::path(root) /
        (L"CodexMonitorWorkerTests-" + std::to_wstring(GetCurrentProcessId()) +
         L"-" + std::to_wstring(GetTickCount64()));
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return error ? std::filesystem::path{} : path;
}

std::wstring CreateReadyEventName() {
    return L"Local\\CodexMonitorWorkerReady-" +
           std::to_wstring(GetCurrentProcessId()) + L"-" +
           std::to_wstring(GetTickCount64());
}

bool ProcessWithImagePathExists(const std::filesystem::path& executable) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
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

template <typename Predicate>
bool WaitUntil(Predicate predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return predicate();
}

std::optional<CompletedCodexRefresh> WaitForRefresh(HWND window,
                                                     CodexWorker& worker,
                                                     std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        MSG message{};
        if (PeekMessageW(&message, window, kWorkerReadyMessage,
                         kWorkerReadyMessage, PM_REMOVE)) {
            return worker.TakeLatest();
        }
        std::this_thread::sleep_for(20ms);
    }
    return std::nullopt;
}

void DrainRefreshMessages(HWND window) {
    MSG message{};
    while (PeekMessageW(&message, window, kWorkerReadyMessage,
                        kWorkerReadyMessage, PM_REMOVE)) {
    }
}

void TestSuccessfulBackgroundRefresh(HWND window) {
    Stage("success_begin");
    ScopedEnvironmentVariable scenario(L"CODEX_FAKE_SCENARIO", L"app-success");
    CodexWorker worker;
    Expect(worker.Start(window, kWorkerReadyMessage, "test-version"),
           "the worker must start with a valid completion window");
    Expect(worker.ActivateAndRefresh(),
           "activating the worker must queue an immediate refresh");

    const auto result = WaitForRefresh(window, worker, 10s);
    Expect(result.has_value(), "the worker must publish a successful refresh");
    if (result) {
        Expect(result->report.initialized && result->report.allMethodsCompleted() &&
                   !result->report.failure,
               "the background refresh must complete the app-server handshake");
        Expect(result->data.rateLimits.lastValue &&
                   result->data.account.lastValue && result->data.usage.lastValue &&
                   result->data.threadList.lastValue,
               "the worker must return all four privacy-trimmed data groups");
        Expect(result->nextRefreshDelay == 300s,
               "a successful worker refresh must use the five-minute cadence");
        Expect(result->succeeded,
               "a successful worker refresh must identify its outcome to the UI");
    }
    worker.StopAndJoin();
    Stage("success_end");
}

void TestPauseCancelsAndResumeRefreshes(HWND window,
                                        const std::filesystem::path& executable) {
    Stage("pause_begin");
    ScopedEnvironmentVariable scenario(L"CODEX_FAKE_SCENARIO", L"app-cancel");
    const std::wstring readyEventName = CreateReadyEventName();
    ScopedEnvironmentVariable readyEventVariable(L"CODEX_FAKE_READY_EVENT",
                                                   readyEventName);
    HANDLE readyEvent = CreateEventW(nullptr, TRUE, FALSE, readyEventName.c_str());
    Expect(readyEvent != nullptr,
           "the cancellation test must create its ready event");
    Stage("pause_environment_ready");
    CodexWorker worker;
    Stage("pause_worker_constructed");
    const bool started = worker.Start(window, kWorkerReadyMessage, "test-version");
    Stage("pause_worker_start_returned");
    Expect(started,
           "the cancellation worker must start");
    const bool activated = worker.ActivateAndRefresh();
    Stage("pause_worker_activate_returned");
    Expect(activated,
           "the cancellation worker must queue an immediate refresh");
    Stage("pause_ready_wait_begin");
    Expect(readyEvent && WaitForSingleObject(readyEvent, 5000) == WAIT_OBJECT_0,
           "the hanging fake app-server must start before pause");
    if (readyEvent) CloseHandle(readyEvent);
    Stage("pause_process_started");

    worker.PauseAndInvalidate();
    Expect(WaitUntil([&] { return !ProcessWithImagePathExists(executable); }, 5s),
           "pause must terminate the app-server job without an orphan");
    Stage("pause_process_stopped");
    DrainRefreshMessages(window);
    Expect(!worker.TakeLatest(),
           "a result invalidated by pause must not reach the UI");

    scenario.Set(L"app-success");
    Expect(worker.ActivateAndRefresh(),
           "resuming must queue a fresh generation immediately");
    const auto resumed = WaitForRefresh(window, worker, 10s);
    Expect(resumed && resumed->report.allMethodsCompleted() &&
               !resumed->report.failure,
           "the resumed generation must complete normally");
    Stage("pause_resumed");
    worker.StopAndJoin();
    Expect(!ProcessWithImagePathExists(executable),
           "stopping the worker must leave no app-server process");
    Stage("pause_end");
}

void TestFailureBackoffAndRecovery(HWND window) {
    Stage("backoff_begin");
    ScopedEnvironmentVariable scenario(L"CODEX_FAKE_SCENARIO",
                                       L"app-single-error");
    CodexWorker worker;
    Expect(worker.Start(window, kWorkerReadyMessage, "test-version"),
           "the backoff worker must start");
    Expect(worker.ActivateAndRefresh(),
           "the backoff worker must queue its first refresh");

    const auto first = WaitForRefresh(window, worker, 10s);
    Expect(first && !first->succeeded && first->nextRefreshDelay == 60s,
           "the first failed refresh must retry after one minute");
    Stage("backoff_first");
    Expect(worker.RequestRefresh(), "an explicit second refresh must bypass the timer");
    const auto second = WaitForRefresh(window, worker, 10s);
    Expect(second && !second->succeeded && second->nextRefreshDelay == 120s,
           "the second consecutive failure must back off to two minutes");
    Stage("backoff_second");
    Expect(worker.RequestRefresh(), "an explicit third refresh must bypass the timer");
    const auto third = WaitForRefresh(window, worker, 10s);
    Expect(third && !third->succeeded && third->nextRefreshDelay == 300s,
           "the third consecutive failure must back off to five minutes");
    Stage("backoff_third");

    scenario.Set(L"app-success");
    Expect(worker.RequestRefresh(), "recovery must allow an immediate explicit refresh");
    const auto recovered = WaitForRefresh(window, worker, 10s);
    Expect(recovered && recovered->succeeded &&
               recovered->nextRefreshDelay == 300s,
           "a successful refresh must reset failure backoff to normal cadence");
    worker.StopAndJoin();
    Stage("backoff_end");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    Stage("main_begin");
    const std::filesystem::path fakeServer =
        argc >= 2 ? std::filesystem::path(argv[1]) : std::filesystem::path{};
    const std::wstring mode = argc >= 3 ? argv[2] : L"all";
    const bool validMode = mode == L"all" || mode == L"success" ||
                           mode == L"pause" || mode == L"backoff";
    const std::filesystem::path testRoot = CreateTestDirectory();
    if (fakeServer.empty() || testRoot.empty() || !validMode) {
        std::cerr << "FAIL: worker test requires a fake app-server and temp directory\n";
        return 1;
    }
    const std::filesystem::path executable = testRoot / L"codex.exe";
    std::error_code error;
    std::filesystem::copy_file(fakeServer, executable,
                               std::filesystem::copy_options::overwrite_existing,
                               error);
    if (error || !std::filesystem::is_regular_file(executable)) {
        std::cerr << "FAIL: could not prepare fake codex.exe\n";
        return 1;
    }

    ScopedEnvironmentVariable codexPath(L"CODEX_CLI_PATH", executable.wstring());
    HWND window = CreateWindowExW(0, L"STATIC", L"CodexWorkerTest", 0, 0, 0,
                                  0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    if (!window) {
        std::cerr << "FAIL: could not create completion window\n";
        return 1;
    }

    if (mode == L"all" || mode == L"success") {
        TestSuccessfulBackgroundRefresh(window);
        DrainRefreshMessages(window);
    }
    if (mode == L"all" || mode == L"pause") {
        TestPauseCancelsAndResumeRefreshes(window, executable);
        DrainRefreshMessages(window);
    }
    if (mode == L"all" || mode == L"backoff") {
        TestFailureBackoffAndRecovery(window);
    }

    Stage("cleanup_begin");
    DestroyWindow(window);
    std::filesystem::remove_all(testRoot, error);
    Stage("cleanup_end");
    if (failures != 0) return 1;
    std::cout << "codex_worker_tests=pass\n";
    return 0;
}
