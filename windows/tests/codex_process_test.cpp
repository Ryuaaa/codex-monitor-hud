#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include "codex/codex_executable.h"
#include "codex/codex_process.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using namespace std::chrono_literals;
int failures = 0;

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

void RestoreEnvironment(const wchar_t* name, const std::optional<std::wstring>& value) {
    SetEnvironmentVariableW(name, value ? value->c_str() : nullptr);
}

std::filesystem::path CurrentExecutableDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                            static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

std::filesystem::path CreateTestDirectory() {
    wchar_t temporaryRoot[MAX_PATH]{};
    const DWORD rootLength = GetTempPathW(MAX_PATH, temporaryRoot);
    if (rootLength == 0 || rootLength >= MAX_PATH) return {};
    const std::wstring leaf = L"CodexMonitorProcessTests-" +
                              std::to_wstring(GetCurrentProcessId()) + L"-" +
                              std::to_wstring(GetTickCount64());
    const std::filesystem::path path = std::filesystem::path(temporaryRoot) / leaf;
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return error ? std::filesystem::path{} : path;
}

std::uint32_t ParseChildPid(const std::string& line) {
    constexpr std::string_view prefix = "{\"childPid\":";
    if (line.rfind(prefix, 0) != 0 || line.empty() || line.back() != '}') return 0;
    try {
        return static_cast<std::uint32_t>(
            std::stoul(line.substr(prefix.size(), line.size() - prefix.size() - 1)));
    } catch (...) {
        return 0;
    }
}

bool IsProcessTerminated(HANDLE process) {
    return process && WaitForSingleObject(process, 2500) == WAIT_OBJECT_0;
}

void TestExecutableDiscovery(const std::filesystem::path& codexExecutable,
                             const std::filesystem::path& testRoot) {
    const auto oldConfigured = EnvironmentValue(L"CODEX_CLI_PATH");
    const auto oldPath = EnvironmentValue(L"PATH");
    wchar_t oldCurrentDirectory[32768]{};
    const DWORD oldCurrentLength = GetCurrentDirectoryW(32768, oldCurrentDirectory);

    SetEnvironmentVariableW(L"CODEX_CLI_PATH", codexExecutable.c_str());
    auto found = codex_monitor::codex::FindCodexExecutable();
    Expect(found && found->lexically_normal() == codexExecutable.lexically_normal(),
           "an absolute CODEX_CLI_PATH must have priority");

    const std::filesystem::path currentOnly = testRoot / L"current-only";
    std::filesystem::create_directories(currentOnly);
    std::filesystem::copy_file(codexExecutable, currentOnly / L"codex.exe",
                               std::filesystem::copy_options::overwrite_existing);
    SetCurrentDirectoryW(currentOnly.c_str());
    SetEnvironmentVariableW(L"CODEX_CLI_PATH", L"relative\\codex.exe");
    const std::wstring pathValue = L";;.;relative;\"" +
                                   codexExecutable.parent_path().wstring() + L"\";";
    SetEnvironmentVariableW(L"PATH", pathValue.c_str());
    found = codex_monitor::codex::FindCodexExecutable();
    Expect(found && found->lexically_normal() == codexExecutable.lexically_normal(),
           "PATH discovery must ignore empty/current/relative entries");

    if (oldCurrentLength > 0 && oldCurrentLength < 32768) {
        SetCurrentDirectoryW(oldCurrentDirectory);
    }
    RestoreEnvironment(L"CODEX_CLI_PATH", oldConfigured);
    RestoreEnvironment(L"PATH", oldPath);
}

void TestFragmentedAndMultipleLines(const std::filesystem::path& executable) {
    codex_monitor::codex::CodexProcess process;
    Expect(process.Start(executable, {L"--scenario=fragmented"}, 4s),
           "the fake app-server must start without a shell");
    Expect(!process.WriteLine("{}\n{}"),
           "WriteLine must reject attempts to inject a second NDJSON record");
    const std::string withNul("{}\0{}", 5);
    Expect(!process.WriteLine(withNul), "WriteLine must reject embedded NUL bytes");
    Expect(process.WriteLine("{\"request\":1}"),
           "WriteLine must deliver one complete NDJSON request");
    const auto first = process.ReadLine(2s);
    const auto second = process.ReadLine(2s);
    const auto third = process.ReadLine(2s);
    Expect(first.status == codex_monitor::codex::ReadLineStatus::kLine &&
               first.line == "{\"fragmented\":true}",
           "fragmented CRLF output must become one line without CR");
    Expect(second.status == codex_monitor::codex::ReadLineStatus::kLine &&
               second.line == "{\"second\":2}",
           "the first of several lines in one pipe read must be queued");
    Expect(third.status == codex_monitor::codex::ReadLineStatus::kLine &&
               third.line == "{\"third\":3}",
           "all complete lines in one pipe read must be queued");
    process.Stop();
}

void TestOversizedLine(const std::filesystem::path& executable) {
    codex_monitor::codex::CodexProcess process;
    Expect(process.Start(executable, {L"--scenario=oversized"}, 5s),
           "the oversized-output server must start");
    const auto result = process.ReadLine(5s);
    Expect(result.status == codex_monitor::codex::ReadLineStatus::kLineTooLong,
           "a stdout line above one MiB must be rejected before unbounded growth");
    process.Stop();
}

void TestStderrMemoryLimit(const std::filesystem::path& executable) {
    codex_monitor::codex::CodexProcess process;
    Expect(process.Start(executable, {L"--scenario=stderr"}, 5s),
           "the stderr-volume server must start");
    const auto result = process.ReadLine(5s);
    Expect(result.status == codex_monitor::codex::ReadLineStatus::kLine,
           "stderr volume must not block stdout NDJSON");
    process.Stop();
    Expect(process.CapturedStderrBytes() ==
               codex_monitor::codex::CodexProcess::kMaximumStderrBytes,
           "stderr retained in memory must stop at sixteen KiB");
    Expect(process.StderrWasTruncated(), "stderr truncation must be observable without logging it");
}

void TestStopKillsJobTree(const std::filesystem::path& executable) {
    codex_monitor::codex::CodexProcess process;
    Expect(process.Start(executable, {L"--scenario=hang"}, 5s),
           "the hanging server must start");
    const std::uint32_t parentId = process.ProcessId();
    const auto announcement = process.ReadLine(2s);
    const std::uint32_t childId = ParseChildPid(announcement.line);
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parentId);
    HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, childId);
    Expect(parent != nullptr && child != nullptr,
           "the fake hanging parent and descendant must exist before Stop");
    process.Stop();
    Expect(IsProcessTerminated(parent), "Stop must terminate the hanging app-server");
    Expect(IsProcessTerminated(child), "the kill-on-close Job must leave no child orphan");
    if (parent) CloseHandle(parent);
    if (child) CloseHandle(child);
}

void TestTotalTimeoutKillsSilentServer(const std::filesystem::path& executable) {
    codex_monitor::codex::CodexProcess process;
    Expect(process.Start(executable, {L"--scenario=child-hang"}, 600ms),
           "the silent hanging server must start");
    HANDLE server = OpenProcess(SYNCHRONIZE, FALSE, process.ProcessId());
    const auto result = process.ReadLine(2s);
    Expect(result.status == codex_monitor::codex::ReadLineStatus::kProcessTimeout,
           "the total process deadline must override a longer ReadLine timeout");
    process.Stop();
    Expect(IsProcessTerminated(server), "the total timeout must not leave its server orphaned");
    if (server) CloseHandle(server);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const std::filesystem::path fakeServer =
        argc >= 2 ? std::filesystem::path(argv[1])
                  : CurrentExecutableDirectory() / L"fake_codex_app_server.exe";
    const std::filesystem::path testRoot = CreateTestDirectory();
    if (testRoot.empty()) {
        std::cerr << "FAIL: could not create isolated test directory\n";
        return 1;
    }
    const std::filesystem::path serverDirectory = testRoot / L"server with spaces";
    const std::filesystem::path codexExecutable = serverDirectory / L"codex.exe";
    std::error_code error;
    std::filesystem::create_directories(serverDirectory, error);
    if (!error) {
        std::filesystem::copy_file(fakeServer, codexExecutable,
                                   std::filesystem::copy_options::overwrite_existing, error);
    }
    if (error || !std::filesystem::is_regular_file(codexExecutable)) {
        std::cerr << "FAIL: could not prepare fake codex.exe\n";
        return 1;
    }

    TestExecutableDiscovery(codexExecutable, testRoot);
    TestFragmentedAndMultipleLines(codexExecutable);
    TestOversizedLine(codexExecutable);
    TestStderrMemoryLimit(codexExecutable);
    TestStopKillsJobTree(codexExecutable);
    TestTotalTimeoutKillsSilentServer(codexExecutable);

    std::filesystem::remove_all(testRoot, error);
    if (failures != 0) return 1;
    std::cout << "codex_process_tests=pass\n";
    return 0;
}
