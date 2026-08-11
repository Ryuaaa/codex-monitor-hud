#pragma once

#include "system_io_rate.h"

#include <windows.h>
#include <pdh.h>

namespace codex_monitor {

class WindowsSystemIoCounterSampler {
public:
    WindowsSystemIoCounterSampler() = default;
    ~WindowsSystemIoCounterSampler();

    WindowsSystemIoCounterSampler(const WindowsSystemIoCounterSampler&) = delete;
    WindowsSystemIoCounterSampler& operator=(const WindowsSystemIoCounterSampler&) = delete;

    SystemIoCounters Capture();

private:
    bool EnsureDiskQuery(std::uint64_t capturedAt100ns);
    DiskIoCounters CaptureDisk(std::uint64_t capturedAt100ns);

    bool diskQueryInitializationAttempted_ = false;
    std::uint64_t nextDiskQueryRetryAt100ns_ = 0;
    PDH_HQUERY diskQuery_ = nullptr;
    PDH_HCOUNTER diskReadCounter_ = nullptr;
    PDH_HCOUNTER diskWriteCounter_ = nullptr;
};

}  // namespace codex_monitor
