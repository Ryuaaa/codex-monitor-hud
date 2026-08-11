#include "system_io_rate.h"
#include "system_io_sampler_win32.h"

#include <windows.h>

#include <cstdlib>
#include <iostream>

namespace {

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    codex_monitor::WindowsSystemIoCounterSampler sampler;
    const codex_monitor::SystemIoCounters first = sampler.Capture();
    Sleep(25);
    const codex_monitor::SystemIoCounters second = sampler.Capture();

    Expect(first.network.available && second.network.available,
           "GetIfTable2 must provide whole-machine network counters");
    Expect(first.network.capturedAt100ns > 0 &&
               second.network.capturedAt100ns > first.network.capturedAt100ns,
           "native samples must use a progressing monotonic timestamp");

    const codex_monitor::SystemIoRates rates =
        codex_monitor::ComputeSystemIoRates(first, second);
    if (first.network.sourceIdentity == second.network.sourceIdentity &&
        second.network.receivedBytes >= first.network.receivedBytes &&
        second.network.sentBytes >= first.network.sentBytes) {
        Expect(rates.networkReceiveBytesPerSecond && rates.networkSendBytesPerSecond,
               "stable native network counters must produce rates after the baseline");
    }

    if (first.disk.available && second.disk.available &&
        second.disk.readBytes >= first.disk.readBytes &&
        second.disk.writtenBytes >= first.disk.writtenBytes) {
        Expect(rates.diskReadBytesPerSecond && rates.diskWriteBytesPerSecond,
               "stable native physical-disk counters must produce rates after the baseline");
    }

    std::cout << "native_system_io_sampler_tests=pass"
              << " network_available=" << second.network.available
              << " disk_available=" << second.disk.available << '\n';
    return 0;
}
