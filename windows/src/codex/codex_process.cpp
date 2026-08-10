#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "codex_process.h"

#include "codex_executable.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace codex_monitor::codex {
namespace {

constexpr std::size_t kMaximumQueuedLines = 256;
constexpr std::size_t kMaximumBufferedStdoutBytes = 4 * 1024 * 1024;
constexpr DWORD kGracefulStopMilliseconds = 300;
constexpr DWORD kForcedStopWaitMilliseconds = 2000;

void CloseHandleIfPresent(HANDLE& handle) {
    if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    handle = nullptr;
}

std::wstring QuoteCommandLineArgument(std::wstring_view argument) {
    if (!argument.empty() &&
        argument.find_first_of(L" \t\"") == std::wstring_view::npos) {
        return std::wstring(argument);
    }

    std::wstring quoted(1, L'"');
    std::size_t backslashes = 0;
    for (wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(character);
        }
        backslashes = 0;
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::optional<std::vector<wchar_t>> BuildCommandLine(
    const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments) {
    std::wstring commandLine = QuoteCommandLineArgument(executable.wstring());
    for (const std::wstring& argument : arguments) {
        if (argument.find(L'\0') != std::wstring::npos) return std::nullopt;
        commandLine.push_back(L' ');
        commandLine += QuoteCommandLineArgument(argument);
    }
    if (commandLine.size() + 1 > 32767) return std::nullopt;
    std::vector<wchar_t> mutableLine(commandLine.begin(), commandLine.end());
    mutableLine.push_back(L'\0');
    return mutableLine;
}

bool WriteAll(HANDLE pipe, std::string_view bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD request = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size() - offset, 64 * 1024));
        DWORD written = 0;
        if (!WriteFile(pipe, bytes.data() + offset, request, &written, nullptr) || written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

}  // namespace

struct CodexProcess::Impl {
    HANDLE process = nullptr;
    HANDLE primaryThread = nullptr;
    HANDLE job = nullptr;
    HANDLE stdinWrite = nullptr;
    HANDLE stdoutRead = nullptr;
    HANDLE stderrRead = nullptr;
    std::uint32_t processId = 0;

    std::thread stdoutThread;
    std::thread stderrThread;
    std::thread watchdogThread;
    mutable std::mutex outputMutex;
    mutable std::mutex stderrMutex;
    std::mutex writeMutex;
    std::mutex stopMutex;
    std::mutex watchdogMutex;
    std::condition_variable outputChanged;
    std::condition_variable watchdogChanged;
    std::deque<std::string> lines;
    std::size_t bufferedStdoutBytes = 0;
    std::string stderrBytes;
    bool stderrTruncated = false;
    bool stdoutEnded = false;
    std::atomic<bool> stderrEnded{false};
    std::optional<ReadLineStatus> fatalStatus;
    std::atomic<bool> running{false};
    std::atomic<bool> stopRequested{false};
    std::chrono::steady_clock::time_point deadline{};

    void ResetState() {
        std::lock_guard<std::mutex> outputLock(outputMutex);
        lines.clear();
        bufferedStdoutBytes = 0;
        stdoutEnded = false;
        stderrEnded.store(false);
        fatalStatus.reset();
        {
            std::lock_guard<std::mutex> stderrLock(stderrMutex);
            stderrBytes.clear();
            stderrTruncated = false;
        }
        processId = 0;
        stopRequested.store(false);
        running.store(false);
    }

    void TerminateJob(DWORD code) const {
        if (job) TerminateJobObject(job, code);
    }

    void SignalFatal(ReadLineStatus status, DWORD terminationCode) {
        {
            std::lock_guard<std::mutex> lock(outputMutex);
            if (!fatalStatus) fatalStatus = status;
        }
        outputChanged.notify_all();
        TerminateJob(terminationCode);
    }

    bool EnqueueLine(std::string line) {
        {
            std::lock_guard<std::mutex> lock(outputMutex);
            if (lines.size() >= kMaximumQueuedLines ||
                bufferedStdoutBytes + line.size() > kMaximumBufferedStdoutBytes) {
                if (!fatalStatus) fatalStatus = ReadLineStatus::kBufferLimitExceeded;
            } else {
                bufferedStdoutBytes += line.size();
                lines.push_back(std::move(line));
                outputChanged.notify_all();
                return true;
            }
        }
        outputChanged.notify_all();
        TerminateJob(ERROR_BUFFER_OVERFLOW);
        return false;
    }

    void ReadStdout() {
        std::string partial;
        partial.reserve(4096);
        char buffer[16 * 1024];
        for (;;) {
            DWORD received = 0;
            const BOOL ok = ReadFile(stdoutRead, buffer, sizeof(buffer), &received, nullptr);
            if (!ok || received == 0) {
                const DWORD error = ok ? ERROR_BROKEN_PIPE : GetLastError();
                const bool expectedClose = error == ERROR_BROKEN_PIPE ||
                                           error == ERROR_OPERATION_ABORTED;
                if (!stopRequested.load() && (!expectedClose || !partial.empty())) {
                    SignalFatal(ReadLineStatus::kIoError, ERROR_INVALID_DATA);
                }
                {
                    std::lock_guard<std::mutex> lock(outputMutex);
                    stdoutEnded = true;
                }
                outputChanged.notify_all();
                return;
            }

            for (DWORD index = 0; index < received; ++index) {
                const char character = buffer[index];
                if (character == '\n') {
                    if (!partial.empty() && partial.back() == '\r') partial.pop_back();
                    if (partial.size() > CodexProcess::kMaximumLineBytes) {
                        SignalFatal(ReadLineStatus::kLineTooLong, ERROR_BUFFER_OVERFLOW);
                        return;
                    }
                    if (!EnqueueLine(std::move(partial))) return;
                    partial.clear();
                    partial.reserve(4096);
                    continue;
                }

                if (partial.size() < CodexProcess::kMaximumLineBytes) {
                    partial.push_back(character);
                    continue;
                }
                // Permit exactly one trailing CR while waiting for an LF. It is
                // removed before applying the one-MiB NDJSON payload limit.
                if (partial.size() == CodexProcess::kMaximumLineBytes && character == '\r') {
                    partial.push_back(character);
                    continue;
                }
                SignalFatal(ReadLineStatus::kLineTooLong, ERROR_BUFFER_OVERFLOW);
                return;
            }
        }
    }

    void ReadStderr() {
        char buffer[4096];
        for (;;) {
            DWORD received = 0;
            const BOOL ok = ReadFile(stderrRead, buffer, sizeof(buffer), &received, nullptr);
            if (!ok || received == 0) {
                stderrEnded.store(true);
                return;
            }
            std::lock_guard<std::mutex> lock(stderrMutex);
            const std::size_t remaining = CodexProcess::kMaximumStderrBytes - stderrBytes.size();
            const std::size_t copy = std::min<std::size_t>(remaining, received);
            stderrBytes.append(buffer, copy);
            if (copy < received) stderrTruncated = true;
        }
    }

    void Watchdog() {
        std::unique_lock<std::mutex> lock(watchdogMutex);
        for (;;) {
            if (stopRequested.load()) return;
            bool outputEnded = false;
            {
                std::lock_guard<std::mutex> outputLock(outputMutex);
                outputEnded = stdoutEnded;
            }
            if (process && WaitForSingleObject(process, 0) == WAIT_OBJECT_0 &&
                outputEnded && stderrEnded.load()) {
                return;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) break;
            const auto nextCheck = std::min(deadline, now + std::chrono::milliseconds(50));
            watchdogChanged.wait_until(lock, nextCheck,
                                       [this] { return stopRequested.load(); });
        }
        lock.unlock();
        SignalFatal(ReadLineStatus::kProcessTimeout, WAIT_TIMEOUT);
    }

    bool Start(const std::filesystem::path& executable,
               const std::vector<std::wstring>& arguments,
               std::chrono::milliseconds requestedTimeout) {
        Stop();
        ResetState();
        if (!IsSafeCodexExecutable(executable)) return false;
        const auto commandLine = BuildCommandLine(executable, arguments);
        if (!commandLine) return false;
        if (requestedTimeout <= std::chrono::milliseconds::zero()) return false;
        const auto totalTimeout = std::min(requestedTimeout,
                                           CodexProcess::kDefaultTotalTimeout);

        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.bInheritHandle = TRUE;

        HANDLE childStdinRead = nullptr;
        HANDLE childStdoutWrite = nullptr;
        HANDLE childStderrWrite = nullptr;
        auto closeChildEnds = [&] {
            CloseHandleIfPresent(childStdinRead);
            CloseHandleIfPresent(childStdoutWrite);
            CloseHandleIfPresent(childStderrWrite);
        };

        if (!CreatePipe(&childStdinRead, &stdinWrite, &security, 0) ||
            !CreatePipe(&stdoutRead, &childStdoutWrite, &security, 0) ||
            !CreatePipe(&stderrRead, &childStderrWrite, &security, 0)) {
            closeChildEnds();
            Stop();
            return false;
        }
        if (!SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0)) {
            closeChildEnds();
            Stop();
            return false;
        }

        job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimits{};
        jobLimits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                              &jobLimits, sizeof(jobLimits))) {
            closeChildEnds();
            Stop();
            return false;
        }

        SIZE_T attributeBytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
        if (attributeBytes == 0) {
            closeChildEnds();
            Stop();
            return false;
        }
        std::vector<unsigned char> attributeStorage(attributeBytes);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
            attributeStorage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes)) {
            closeChildEnds();
            Stop();
            return false;
        }

        HANDLE inheritedHandles[] = {childStdinRead, childStdoutWrite, childStderrWrite};
        const bool handleListReady = UpdateProcThreadAttribute(
            attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritedHandles,
            sizeof(inheritedHandles), nullptr, nullptr) != FALSE;

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = childStdinRead;
        startup.StartupInfo.hStdOutput = childStdoutWrite;
        startup.StartupInfo.hStdError = childStderrWrite;
        PROCESS_INFORMATION processInfo{};
        std::vector<wchar_t> mutableCommandLine = *commandLine;
        const DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED |
                            EXTENDED_STARTUPINFO_PRESENT;
        const BOOL created = handleListReady && CreateProcessW(
            executable.c_str(), mutableCommandLine.data(), nullptr, nullptr, TRUE, flags,
            nullptr, nullptr, &startup.StartupInfo, &processInfo);
        DeleteProcThreadAttributeList(attributes);
        closeChildEnds();
        if (!created) {
            Stop();
            return false;
        }

        process = processInfo.hProcess;
        primaryThread = processInfo.hThread;
        processId = processInfo.dwProcessId;
        if (!AssignProcessToJobObject(job, process)) {
            TerminateProcess(process, ERROR_ACCESS_DENIED);
            Stop();
            return false;
        }

        deadline = std::chrono::steady_clock::now() + totalTimeout;
        if (ResumeThread(primaryThread) == static_cast<DWORD>(-1)) {
            TerminateJob(ERROR_INVALID_FUNCTION);
            Stop();
            return false;
        }
        CloseHandleIfPresent(primaryThread);
        running.store(true);
        try {
            stdoutThread = std::thread([this] { ReadStdout(); });
            stderrThread = std::thread([this] { ReadStderr(); });
            watchdogThread = std::thread([this] { Watchdog(); });
        } catch (...) {
            TerminateJob(ERROR_NOT_ENOUGH_MEMORY);
            Stop();
            return false;
        }
        return true;
    }

    bool WriteLine(std::string_view line) {
        std::lock_guard<std::mutex> lock(writeMutex);
        if (!running.load() || !stdinWrite || line.size() > CodexProcess::kMaximumLineBytes ||
            line.find('\0') != std::string_view::npos ||
            line.find('\r') != std::string_view::npos ||
            line.find('\n') != std::string_view::npos) {
            return false;
        }
        if (WaitForSingleObject(process, 0) != WAIT_TIMEOUT) return false;
        if (std::chrono::steady_clock::now() >= deadline) {
            SignalFatal(ReadLineStatus::kProcessTimeout, WAIT_TIMEOUT);
            return false;
        }

        std::string payload(line);
        payload.push_back('\n');
        std::promise<bool> completion;
        std::future<bool> finished = completion.get_future();
        const HANDLE pipe = stdinWrite;
        std::thread writer;
        try {
            writer = std::thread([pipe, payload = std::move(payload),
                                  completion = std::move(completion)]() mutable {
                completion.set_value(WriteAll(pipe, payload));
            });
        } catch (...) {
            SignalFatal(ReadLineStatus::kIoError, ERROR_NOT_ENOUGH_MEMORY);
            return false;
        }

        if (finished.wait_until(deadline) == std::future_status::timeout) {
            CancelSynchronousIo(writer.native_handle());
            SignalFatal(ReadLineStatus::kProcessTimeout, WAIT_TIMEOUT);
        }
        writer.join();
        return finished.get();
    }

    ReadLineResult ReadLine(std::chrono::milliseconds requestedTimeout) {
        if (requestedTimeout < std::chrono::milliseconds::zero()) {
            requestedTimeout = std::chrono::milliseconds::zero();
        }
        const auto callDeadline = std::min(std::chrono::steady_clock::now() + requestedTimeout,
                                           deadline);
        std::unique_lock<std::mutex> lock(outputMutex);
        const auto ready = [this] {
            return !lines.empty() || fatalStatus.has_value() || stdoutEnded ||
                   !running.load();
        };
        if (!ready()) outputChanged.wait_until(lock, callDeadline, ready);

        if (!lines.empty()) {
            std::string line = std::move(lines.front());
            lines.pop_front();
            bufferedStdoutBytes -= line.size();
            return {ReadLineStatus::kLine, std::move(line)};
        }
        if (fatalStatus) return {*fatalStatus, {}};
        if (stdoutEnded) return {ReadLineStatus::kEndOfStream, {}};
        if (!running.load()) return {ReadLineStatus::kNotRunning, {}};
        if (std::chrono::steady_clock::now() >= deadline) {
            lock.unlock();
            SignalFatal(ReadLineStatus::kProcessTimeout, WAIT_TIMEOUT);
            return {ReadLineStatus::kProcessTimeout, {}};
        }
        return {ReadLineStatus::kTimeout, {}};
    }

    void Stop() {
        std::lock_guard<std::mutex> stopLock(stopMutex);
        stopRequested.store(true);
        watchdogChanged.notify_all();
        outputChanged.notify_all();

        {
            std::lock_guard<std::mutex> writeLock(writeMutex);
            CloseHandleIfPresent(stdinWrite);
        }
        if (process) WaitForSingleObject(process, kGracefulStopMilliseconds);
        // Terminate the whole job even when the root exited during the grace
        // period: a descendant may otherwise survive while retaining a pipe.
        TerminateJob(ERROR_CANCELLED);
        if (process) WaitForSingleObject(process, kForcedStopWaitMilliseconds);

        if (watchdogThread.joinable()) watchdogThread.join();
        if (stdoutThread.joinable()) CancelSynchronousIo(stdoutThread.native_handle());
        if (stderrThread.joinable()) CancelSynchronousIo(stderrThread.native_handle());
        if (stdoutThread.joinable()) stdoutThread.join();
        if (stderrThread.joinable()) stderrThread.join();
        CloseHandleIfPresent(stdoutRead);
        CloseHandleIfPresent(stderrRead);

        CloseHandleIfPresent(primaryThread);
        CloseHandleIfPresent(process);
        CloseHandleIfPresent(job);
        running.store(false);
        processId = 0;
        outputChanged.notify_all();
    }
};

CodexProcess::CodexProcess() : impl_(std::make_unique<Impl>()) {}
CodexProcess::~CodexProcess() { impl_->Stop(); }

bool CodexProcess::Start(const std::filesystem::path& executable,
                         const std::vector<std::wstring>& arguments,
                         std::chrono::milliseconds totalTimeout) {
    return impl_->Start(executable, arguments, totalTimeout);
}

bool CodexProcess::WriteLine(std::string_view line) { return impl_->WriteLine(line); }

ReadLineResult CodexProcess::ReadLine(std::chrono::milliseconds timeout) {
    return impl_->ReadLine(timeout);
}

void CodexProcess::Stop() { impl_->Stop(); }

bool CodexProcess::IsRunning() const {
    return impl_->running.load() && impl_->process &&
           WaitForSingleObject(impl_->process, 0) == WAIT_TIMEOUT;
}

std::uint32_t CodexProcess::ProcessId() const { return impl_->processId; }

std::size_t CodexProcess::CapturedStderrBytes() const {
    std::lock_guard<std::mutex> lock(impl_->stderrMutex);
    return impl_->stderrBytes.size();
}

bool CodexProcess::StderrWasTruncated() const {
    std::lock_guard<std::mutex> lock(impl_->stderrMutex);
    return impl_->stderrTruncated;
}

}  // namespace codex_monitor::codex
