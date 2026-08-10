#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace codex_monitor::codex {

enum class ReadLineStatus {
    kLine,
    kTimeout,
    kEndOfStream,
    kLineTooLong,
    kBufferLimitExceeded,
    kProcessTimeout,
    kIoError,
    kNotRunning,
};

struct ReadLineResult {
    ReadLineStatus status = ReadLineStatus::kNotRunning;
    std::string line;
};

class CodexProcess {
public:
    static constexpr std::size_t kMaximumLineBytes = 1024 * 1024;
    static constexpr std::size_t kMaximumStderrBytes = 16 * 1024;
    static constexpr std::chrono::milliseconds kDefaultTotalTimeout{15000};

    CodexProcess();
    ~CodexProcess();
    CodexProcess(const CodexProcess&) = delete;
    CodexProcess& operator=(const CodexProcess&) = delete;

    bool Start(const std::filesystem::path& executable,
               const std::vector<std::wstring>& arguments,
               std::chrono::milliseconds totalTimeout = kDefaultTotalTimeout);
    bool WriteLine(std::string_view line);
    ReadLineResult ReadLine(std::chrono::milliseconds timeout);
    void Stop();

    bool IsRunning() const;
    std::uint32_t ProcessId() const;
    std::size_t CapturedStderrBytes() const;
    bool StderrWasTruncated() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace codex_monitor::codex
