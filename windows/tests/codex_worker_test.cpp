#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include "codex/codex_worker.h"

#include <winrt/base.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cwchar>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

namespace {

using codex_monitor::codex::CodexWorker;
using codex_monitor::codex::CompletedCodexRefresh;
using namespace std::chrono_literals;

constexpr UINT kWorkerReadyMessage = WM_APP + 41;
int failures = 0;
std::atomic_flag crashLoggerActive = ATOMIC_FLAG_INIT;

void PrintCrashFrame(HANDLE process, DWORD64 address, std::size_t index) {
    alignas(SYMBOL_INFO)
        std::array<std::byte, sizeof(SYMBOL_INFO) + MAX_SYM_NAME> storage{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage.data());
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 symbolDisplacement = 0;
    const BOOL hasSymbol = SymFromAddr(process, address, &symbolDisplacement, symbol);

    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD lineDisplacement = 0;
    const BOOL hasLine = SymGetLineFromAddr64(process, address, &lineDisplacement,
                                              &line);
    std::fprintf(stderr, "worker_crash_frame=%zu address=0x%llx", index,
                 static_cast<unsigned long long>(address));
    if (hasSymbol) {
        std::fprintf(stderr, " symbol=%s+0x%llx", symbol->Name,
                     static_cast<unsigned long long>(symbolDisplacement));
    }
    if (hasLine && line.FileName) {
        std::fprintf(stderr, " source=%s:%lu", line.FileName,
                     static_cast<unsigned long>(line.LineNumber));
    }
    std::fputc('\n', stderr);
}

LONG WINAPI LogUnhandledWorkerException(EXCEPTION_POINTERS* exception) {
    if (!exception || !exception->ExceptionRecord || !exception->ContextRecord ||
        crashLoggerActive.test_and_set()) {
        return EXCEPTION_EXECUTE_HANDLER;
    }

    const DWORD code = exception->ExceptionRecord->ExceptionCode;
    const void* address = exception->ExceptionRecord->ExceptionAddress;
    std::fprintf(stderr, "worker_crash_code=0x%08lx address=%p thread=%lu\n",
                 static_cast<unsigned long>(code), address,
                 static_cast<unsigned long>(GetCurrentThreadId()));

#if defined(_M_X64)
    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();
    CONTEXT context = *exception->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;

    PrintCrashFrame(process, frame.AddrPC.Offset, 0);
    for (std::size_t index = 1; index < 32; ++index) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame,
                         &context, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr) ||
            frame.AddrPC.Offset == 0) {
            break;
        }
        PrintCrashFrame(process, frame.AddrPC.Offset, index);
    }
#endif
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

void InstallCrashStackLogger() {
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES |
                  SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    SetUnhandledExceptionFilter(LogUnhandledWorkerException);
}

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

std::optional<std::string> ReadTextFile(
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

bool WriteTextFile(const std::filesystem::path& path,
                   std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.flush();
    return static_cast<bool>(output);
}

bool IsWhitelistedQuotaHistory(std::string_view contents) {
    std::istringstream input{std::string(contents)};
    std::string version;
    std::string sample;
    std::string extra;
    if (!std::getline(input, version) || version != "version=1" ||
        !std::getline(input, sample) || std::getline(input, extra)) {
        return false;
    }

    constexpr std::array<std::string_view, 6> prefixes = {
        "sample",
        "captured_at=",
        "five_hour_remaining=",
        "five_hour_reset_at=",
        "weekly_remaining=",
        "weekly_reset_at=",
    };
    std::size_t start = 0;
    for (std::size_t index = 0; index < prefixes.size(); ++index) {
        const std::size_t separator = sample.find('\t', start);
        if ((index + 1 < prefixes.size()) !=
            (separator != std::string::npos)) {
            return false;
        }
        const std::size_t end = separator == std::string::npos
                                    ? sample.size()
                                    : separator;
        const std::string_view field(sample.data() + start, end - start);
        if (index == 0) {
            if (field != prefixes[index]) return false;
        } else if (field.size() <= prefixes[index].size() ||
                   field.substr(0, prefixes[index].size()) != prefixes[index]) {
            return false;
        }
        start = end + 1;
    }
    return true;
}

std::int64_t CurrentUnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
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
    const std::filesystem::path historyPath =
        executable.parent_path() / L"quota-cancel-history.txt";
    const bool started = worker.Start(window, kWorkerReadyMessage, "test-version",
                                      historyPath);
    Stage("pause_worker_start_returned");
    Expect(started,
           "the cancellation worker must start");
    Expect(worker.SetQuotaForecastEnabled(true),
           "the cancellation test must enable quota history before refresh");
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
    Expect(!std::filesystem::exists(historyPath),
           "a cancelled Codex refresh must not write quota history");

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

void TestQuotaForecastDemandGating(HWND window,
                                  const std::filesystem::path& testRoot) {
    Stage("quota_begin");
    ScopedEnvironmentVariable scenario(L"CODEX_FAKE_SCENARIO", L"app-success");
    const std::filesystem::path historyPath =
        testRoot / L"quota-demand-history.txt";

    CodexWorker worker;
    Expect(worker.Start(window, kWorkerReadyMessage, "test-version", historyPath),
           "the quota worker must start with a history destination");
    Expect(worker.ActivateAndRefresh(),
           "the quota worker must queue its default-disabled refresh");
    const auto disabled = WaitForRefresh(window, worker, 10s);
    Expect(disabled && disabled->succeeded,
           "the default-disabled quota refresh must otherwise succeed");
    Expect(disabled && !disabled->quotaForecastUpdate,
           "quota forecast output must be absent while the feature is disabled");
    Expect(!std::filesystem::exists(historyPath),
           "quota history must not be created while the feature is disabled");

    Expect(worker.SetQuotaForecastEnabled(true),
           "explicitly enabling quota forecast must change worker demand");
    const auto enabled = WaitForRefresh(window, worker, 10s);
    Expect(enabled && enabled->succeeded && enabled->quotaForecastUpdate,
           "enabling quota forecast must publish a forecast update");
    if (enabled && enabled->quotaForecastUpdate) {
        const auto& update = *enabled->quotaForecastUpdate;
        Expect(update.fiveHour.windowReturned && update.weekly.windowReturned,
               "the forecast update must identify both returned quota windows");
        Expect(update.historyStored && !update.historySaveFailed,
               "a successful enabled refresh must persist its quota sample");
    }
    const auto written = ReadTextFile(historyPath);
    Expect(written && IsWhitelistedQuotaHistory(*written),
           "quota history must contain only the versioned whitelist fields");
    if (written) {
        Expect(written->find("email") == std::string::npos &&
                   written->find("thread") == std::string::npos &&
                   written->find("preview") == std::string::npos &&
                   written->find("token") == std::string::npos,
               "quota history must exclude account, task, and token data");
    }

    const std::int64_t now = CurrentUnixSeconds();
    std::ostringstream fixture;
    fixture << "version=1\n"
            << "sample\tcaptured_at=" << now - 120
            << "\tfive_hour_remaining=75\tfive_hour_reset_at=" << now + 18000
            << "\tweekly_remaining=60\tweekly_reset_at=" << now + 604800
            << '\n';
    Expect(WriteTextFile(historyPath, fixture.str()),
           "the disable-gating fixture must replace quota history");
    const auto beforeDisabledRefresh = ReadTextFile(historyPath);

    Expect(worker.SetQuotaForecastEnabled(false),
           "disabling quota forecast must change worker demand");
    Expect(worker.RequestRefresh(),
           "other visible Codex modules must still be able to refresh");
    const auto refreshedWhileDisabled = WaitForRefresh(window, worker, 10s);
    Expect(refreshedWhileDisabled && refreshedWhileDisabled->succeeded,
           "the ordinary Codex refresh must succeed after forecast is disabled");
    Expect(refreshedWhileDisabled &&
               !refreshedWhileDisabled->quotaForecastUpdate,
           "a disabled forecast must not publish stale output");
    const auto afterDisabledRefresh = ReadTextFile(historyPath);
    Expect(beforeDisabledRefresh && afterDisabledRefresh &&
               *beforeDisabledRefresh == *afterDisabledRefresh,
           "ordinary Codex refreshes must not modify disabled quota history");
    worker.StopAndJoin();

    const std::filesystem::path failedHistoryPath =
        testRoot / L"quota-failed-history.txt";
    scenario.Set(L"app-init-error");
    CodexWorker failedWorker;
    Expect(failedWorker.Start(window, kWorkerReadyMessage, "test-version",
                              failedHistoryPath),
           "the failed quota worker must start");
    Expect(failedWorker.SetQuotaForecastEnabled(true),
           "the failed quota worker must enable quota forecast");
    Expect(failedWorker.ActivateAndRefresh(),
           "the failed quota worker must queue a refresh");
    const auto failed = WaitForRefresh(window, failedWorker, 10s);
    Expect(failed && !failed->succeeded && !failed->quotaForecastUpdate,
           "a failed refresh must not publish quota forecast output");
    failedWorker.StopAndJoin();
    Expect(!std::filesystem::exists(failedHistoryPath),
           "a failed refresh must not create quota history");
    Stage("quota_end");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    InstallCrashStackLogger();
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    Stage("main_begin");
    const std::filesystem::path fakeServer =
        argc >= 2 ? std::filesystem::path(argv[1]) : std::filesystem::path{};
    const std::wstring mode = argc >= 3 ? argv[2] : L"all";
    const bool validMode = mode == L"all" || mode == L"success" ||
                           mode == L"pause" || mode == L"backoff" ||
                           mode == L"quota";
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
        DrainRefreshMessages(window);
    }
    if (mode == L"all" || mode == L"quota") {
        TestQuotaForecastDemandGating(window, testRoot);
    }

    Stage("cleanup_begin");
    DestroyWindow(window);
    std::filesystem::remove_all(testRoot, error);
    Stage("cleanup_end");
    if (failures != 0) return 1;
    std::cout << "codex_worker_tests=pass\n";
    return 0;
}
