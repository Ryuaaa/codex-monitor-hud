#include "system_io_rate.h"

namespace codex_monitor {
namespace {

struct ByteRatePair {
    double firstBytesPerSecond = 0.0;
    double secondBytesPerSecond = 0.0;
};

std::optional<ByteRatePair> ComputePair(std::uint64_t previousFirst,
                                        std::uint64_t previousSecond,
                                        std::uint64_t currentFirst,
                                        std::uint64_t currentSecond,
                                        std::uint64_t previousTime100ns,
                                        std::uint64_t currentTime100ns,
                                        std::uint64_t previousSourceIdentity,
                                        std::uint64_t currentSourceIdentity) noexcept {
    if (previousSourceIdentity != currentSourceIdentity ||
        currentFirst < previousFirst || currentSecond < previousSecond ||
        currentTime100ns <= previousTime100ns) {
        return std::nullopt;
    }

    const std::uint64_t elapsed100ns = currentTime100ns - previousTime100ns;
    if (elapsed100ns > kMaximumSystemIoRateInterval100ns) return std::nullopt;

    const double seconds = static_cast<double>(elapsed100ns) /
                           static_cast<double>(kSystemIoHundredNanosecondsPerSecond);
    return ByteRatePair{
        static_cast<double>(currentFirst - previousFirst) / seconds,
        static_cast<double>(currentSecond - previousSecond) / seconds,
    };
}

}  // namespace

SystemIoRates ComputeSystemIoRates(const SystemIoCounters& previous,
                                   const SystemIoCounters& current) noexcept {
    SystemIoRates result{};

    if (current.network.available) {
        result.networkNeedsBaseline = !previous.network.available;
        if (previous.network.available) {
            const auto rates = ComputePair(
                previous.network.receivedBytes,
                previous.network.sentBytes,
                current.network.receivedBytes,
                current.network.sentBytes,
                previous.network.capturedAt100ns,
                current.network.capturedAt100ns,
                previous.network.sourceIdentity,
                current.network.sourceIdentity);
            if (rates) {
                result.networkReceiveBytesPerSecond = rates->firstBytesPerSecond;
                result.networkSendBytesPerSecond = rates->secondBytesPerSecond;
            } else {
                result.networkNeedsBaseline = true;
            }
        }
    }

    if (current.disk.available) {
        result.diskNeedsBaseline = !previous.disk.available;
        if (previous.disk.available) {
            const auto rates = ComputePair(
                previous.disk.readBytes,
                previous.disk.writtenBytes,
                current.disk.readBytes,
                current.disk.writtenBytes,
                previous.disk.capturedAt100ns,
                current.disk.capturedAt100ns,
                previous.disk.sourceIdentity,
                current.disk.sourceIdentity);
            if (rates) {
                result.diskReadBytesPerSecond = rates->firstBytesPerSecond;
                result.diskWriteBytesPerSecond = rates->secondBytesPerSecond;
            } else {
                result.diskNeedsBaseline = true;
            }
        }
    }

    return result;
}

}  // namespace codex_monitor
