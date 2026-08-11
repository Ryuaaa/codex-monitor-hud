#include "system_io_rate.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void ExpectNear(double actual, double expected, const char* message) {
    Expect(std::fabs(actual - expected) < 0.0001, message);
}

codex_monitor::SystemIoCounters Sample(std::uint64_t timestamp100ns,
                                       std::uint64_t networkReceived,
                                       std::uint64_t networkSent,
                                       std::uint64_t diskRead,
                                       std::uint64_t diskWritten,
                                       std::uint64_t networkIdentity = 7,
                                       std::uint64_t diskIdentity = 1) {
    codex_monitor::SystemIoCounters sample{};
    sample.network = {true, networkReceived, networkSent, timestamp100ns, networkIdentity};
    sample.disk = {true, diskRead, diskWritten, timestamp100ns, diskIdentity};
    return sample;
}

void TestFirstSampleIsUnavailable() {
    const auto rates = codex_monitor::ComputeSystemIoRates(
        codex_monitor::SystemIoCounters{}, Sample(10'000'000, 100, 200, 300, 400));
    Expect(rates.networkNeedsBaseline && rates.diskNeedsBaseline,
           "the first native counter sample must request a baseline");
    Expect(!rates.networkReceiveBytesPerSecond && !rates.networkSendBytesPerSecond &&
               !rates.diskReadBytesPerSecond && !rates.diskWriteBytesPerSecond,
           "the first sample must be unavailable rather than reported as zero");
}

void TestByteRates() {
    const auto previous = Sample(10'000'000, 1'000, 4'000, 8'000, 16'000);
    const auto current = Sample(30'000'000, 3'000, 5'000, 14'000, 24'000);
    const auto rates = codex_monitor::ComputeSystemIoRates(previous, current);
    Expect(rates.networkReceiveBytesPerSecond && rates.networkSendBytesPerSecond &&
               rates.diskReadBytesPerSecond && rates.diskWriteBytesPerSecond,
           "monotonic counters with elapsed time must produce all four rates");
    ExpectNear(*rates.networkReceiveBytesPerSecond, 1'000.0,
               "network receive must be returned in bytes per second");
    ExpectNear(*rates.networkSendBytesPerSecond, 500.0,
               "network send must be returned in bytes per second");
    ExpectNear(*rates.diskReadBytesPerSecond, 3'000.0,
               "disk read must be returned in bytes per second");
    ExpectNear(*rates.diskWriteBytesPerSecond, 4'000.0,
               "disk write must be returned in bytes per second");

    const auto idle = codex_monitor::ComputeSystemIoRates(current, Sample(
        40'000'000, 3'000, 5'000, 14'000, 24'000));
    Expect(idle.networkReceiveBytesPerSecond && *idle.networkReceiveBytesPerSecond == 0.0 &&
               idle.diskWriteBytesPerSecond && *idle.diskWriteBytesPerSecond == 0.0,
           "unchanged cumulative counters must produce an available zero rate");
}

void TestCounterResetAndSourceChangeAreUnavailable() {
    const auto previous = Sample(10'000'000, 1'000, 2'000, 3'000, 4'000);
    auto reset = Sample(20'000'000, 999, 2'100, 3'100, 4'100);
    auto rates = codex_monitor::ComputeSystemIoRates(previous, reset);
    Expect(!rates.networkReceiveBytesPerSecond && !rates.networkSendBytesPerSecond,
           "counter rollback or reset must invalidate the whole source pair");
    Expect(rates.networkNeedsBaseline,
           "a counter reset must be exposed as requiring a replacement baseline");
    Expect(rates.diskReadBytesPerSecond && rates.diskWriteBytesPerSecond,
           "one invalid source must not discard the other source");

    auto changed = Sample(20'000'000, 1'100, 2'100, 3'100, 4'100, 8, 2);
    rates = codex_monitor::ComputeSystemIoRates(previous, changed);
    Expect(!rates.networkReceiveBytesPerSecond && !rates.diskReadBytesPerSecond,
           "an adapter or counter source change must require a fresh baseline");
    Expect(rates.networkNeedsBaseline && rates.diskNeedsBaseline,
           "a changed source identity must be exposed as a baseline transition");
}

void TestTimeAnomaliesAreUnavailable() {
    const auto previous = Sample(20'000'000, 1'000, 2'000, 3'000, 4'000);
    const auto sameTime = Sample(20'000'000, 1'100, 2'100, 3'100, 4'100);
    const auto backwards = Sample(19'000'000, 1'100, 2'100, 3'100, 4'100);
    Expect(!codex_monitor::ComputeSystemIoRates(previous, sameTime)
                .networkReceiveBytesPerSecond,
           "a zero elapsed interval must be unavailable");
    Expect(!codex_monitor::ComputeSystemIoRates(previous, backwards)
                .diskReadBytesPerSecond,
           "a backwards monotonic timestamp must be unavailable");

    const auto stale = Sample(
        20'000'000 + codex_monitor::kMaximumSystemIoRateInterval100ns + 1,
        1'100, 2'100, 3'100, 4'100);
    const auto staleRates = codex_monitor::ComputeSystemIoRates(previous, stale);
    Expect(!staleRates.networkReceiveBytesPerSecond && !staleRates.diskReadBytesPerSecond,
           "an unexpectedly long interval must not be presented as a current rate");
    Expect(staleRates.networkNeedsBaseline && staleRates.diskNeedsBaseline,
           "invalid elapsed time must be exposed as requiring a new baseline");
}

void TestUnavailableSourceStaysUnavailable() {
    auto current = Sample(20'000'000, 1'100, 2'100, 3'100, 4'100);
    current.disk.available = false;
    const auto rates = codex_monitor::ComputeSystemIoRates(
        Sample(10'000'000, 1'000, 2'000, 3'000, 4'000), current);
    Expect(rates.networkReceiveBytesPerSecond.has_value(),
           "an available source must still produce a rate");
    Expect(!rates.diskReadBytesPerSecond && !rates.diskWriteBytesPerSecond &&
               !rates.diskNeedsBaseline,
           "a failed native source must remain unavailable, not become zero or baseline state");
}

}  // namespace

int main() {
    TestFirstSampleIsUnavailable();
    TestByteRates();
    TestCounterResetAndSourceChangeAreUnavailable();
    TestTimeAnomaliesAreUnavailable();
    TestUnavailableSourceStaysUnavailable();
    std::cout << "system_io_rate_tests=pass\n";
    return 0;
}
