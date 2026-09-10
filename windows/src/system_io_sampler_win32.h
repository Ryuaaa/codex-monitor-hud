#pragma once

#include "system_io_rate.h"

#ifdef _WIN32
#include <windows.h>
#include <pdh.h>
#else
// Only opaque members are needed by portable scheduling tests.
using PDH_HQUERY = void*;
using PDH_HCOUNTER = void*;
#endif

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
    void ResetDiskQueryAfterFailure(std::uint64_t capturedAt100ns) noexcept;
    DiskIoCounters CaptureDisk(std::uint64_t capturedAt100ns);

    bool diskQueryInitializationAttempted_ = false;
    std::uint64_t nextDiskQueryRetryAt100ns_ = 0;
    PDH_HQUERY diskQuery_ = nullptr;
    PDH_HCOUNTER diskReadCounter_ = nullptr;
    PDH_HCOUNTER diskWriteCounter_ = nullptr;
};

}  // namespace codex_monitor
