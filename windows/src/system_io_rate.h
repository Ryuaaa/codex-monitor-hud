#pragma once

#include <cstdint>
#include <optional>

namespace codex_monitor {

constexpr std::uint64_t kSystemIoHundredNanosecondsPerSecond = 10'000'000ULL;
constexpr std::uint64_t kMaximumSystemIoRateInterval100ns =
    5ULL * 60ULL * kSystemIoHundredNanosecondsPerSecond;

struct NetworkIoCounters {
    bool available = false;
    std::uint64_t receivedBytes = 0;
    std::uint64_t sentBytes = 0;
    std::uint64_t capturedAt100ns = 0;
    std::uint64_t sourceIdentity = 0;
};

struct DiskIoCounters {
    bool available = false;
    std::uint64_t readBytes = 0;
    std::uint64_t writtenBytes = 0;
    std::uint64_t capturedAt100ns = 0;
    std::uint64_t sourceIdentity = 0;
};

struct SystemIoCounters {
    NetworkIoCounters network;
    DiskIoCounters disk;
};

struct SystemIoRates {
    std::optional<double> networkReceiveBytesPerSecond;
    std::optional<double> networkSendBytesPerSecond;
    std::optional<double> diskReadBytesPerSecond;
    std::optional<double> diskWriteBytesPerSecond;
    bool networkNeedsBaseline = false;
    bool diskNeedsBaseline = false;
};

SystemIoRates ComputeSystemIoRates(const SystemIoCounters& previous,
                                   const SystemIoCounters& current) noexcept;

}  // namespace codex_monitor
