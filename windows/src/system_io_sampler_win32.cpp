#include <sdkddkver.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>

#include "system_io_sampler_win32.h"

#include <pdhmsg.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace codex_monitor {
namespace {

constexpr wchar_t kDiskReadCounterPath[] =
    L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec";
constexpr wchar_t kDiskWriteCounterPath[] =
    L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec";
constexpr std::uint64_t kDiskSourceIdentity = 1;
constexpr std::uint64_t kDiskQueryRetryInterval100ns =
    60ULL * kSystemIoHundredNanosecondsPerSecond;

bool CheckedAdd(std::uint64_t value, std::uint64_t* total) noexcept {
    if (!total || value > std::numeric_limits<std::uint64_t>::max() - *total) return false;
    *total += value;
    return true;
}

std::uint64_t HashInterfaceIdentities(std::vector<std::uint64_t> identities) {
    std::sort(identities.begin(), identities.end());
    std::uint64_t hash = 1469598103934665603ULL;
    for (const std::uint64_t identity : identities) {
        for (unsigned int shift = 0; shift < 64; shift += 8) {
            hash ^= (identity >> shift) & 0xffULL;
            hash *= 1099511628211ULL;
        }
    }
    hash ^= static_cast<std::uint64_t>(identities.size());
    hash *= 1099511628211ULL;
    return hash;
}

NetworkIoCounters CaptureNetwork(std::uint64_t capturedAt100ns) {
    NetworkIoCounters result{};
    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || !table) return result;

    bool valid = true;
    std::vector<std::uint64_t> identities;
    identities.reserve(table->NumEntries);
    for (ULONG index = 0; index < table->NumEntries; ++index) {
        const MIB_IF_ROW2& row = table->Table[index];
        if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK ||
            row.OperStatus != IfOperStatusUp ||
            !row.InterfaceAndOperStatusFlags.HardwareInterface ||
            row.InterfaceAndOperStatusFlags.FilterInterface) {
            continue;
        }
        identities.push_back(row.InterfaceLuid.Value);
        valid = CheckedAdd(row.InOctets, &result.receivedBytes) &&
                CheckedAdd(row.OutOctets, &result.sentBytes) && valid;
    }
    FreeMibTable(table);

    if (!valid) return NetworkIoCounters{};
    result.available = true;
    result.capturedAt100ns = capturedAt100ns;
    result.sourceIdentity = HashInterfaceIdentities(std::move(identities));
    return result;
}

bool IsValidRawCounter(const PDH_RAW_COUNTER& counter) noexcept {
    return (counter.CStatus == PDH_CSTATUS_VALID_DATA ||
            counter.CStatus == PDH_CSTATUS_NEW_DATA) &&
           counter.FirstValue >= 0;
}

}  // namespace

WindowsSystemIoCounterSampler::~WindowsSystemIoCounterSampler() {
    if (diskQuery_) PdhCloseQuery(diskQuery_);
}

bool WindowsSystemIoCounterSampler::EnsureDiskQuery(std::uint64_t capturedAt100ns) {
    if (diskQuery_) return true;
    if (diskQueryInitializationAttempted_ &&
        capturedAt100ns < nextDiskQueryRetryAt100ns_) {
        return false;
    }
    diskQueryInitializationAttempted_ = true;
    nextDiskQueryRetryAt100ns_ =
        capturedAt100ns + kDiskQueryRetryInterval100ns;

    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER readCounter = nullptr;
    PDH_HCOUNTER writeCounter = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS || !query) return false;
    if (PdhAddEnglishCounterW(query, kDiskReadCounterPath, 0, &readCounter) != ERROR_SUCCESS ||
        PdhAddEnglishCounterW(query, kDiskWriteCounterPath, 0, &writeCounter) != ERROR_SUCCESS) {
        PdhCloseQuery(query);
        return false;
    }

    diskQuery_ = query;
    diskReadCounter_ = readCounter;
    diskWriteCounter_ = writeCounter;
    nextDiskQueryRetryAt100ns_ = 0;
    return true;
}

DiskIoCounters WindowsSystemIoCounterSampler::CaptureDisk(std::uint64_t capturedAt100ns) {
    DiskIoCounters result{};
    if (!EnsureDiskQuery(capturedAt100ns) ||
        PdhCollectQueryData(diskQuery_) != ERROR_SUCCESS) {
        return result;
    }

    DWORD readType = 0;
    DWORD writeType = 0;
    PDH_RAW_COUNTER read{};
    PDH_RAW_COUNTER write{};
    // These display counters are PERF_COUNTER_BULK_COUNT. Their raw FirstValue is the
    // cumulative byte count; it is not an already-formatted rate. Keep the raw values
    // and derive bytes/second from two monotonic samples in ComputeSystemIoRates.
    if (PdhGetRawCounterValue(diskReadCounter_, &readType, &read) != ERROR_SUCCESS ||
        PdhGetRawCounterValue(diskWriteCounter_, &writeType, &write) != ERROR_SUCCESS ||
        readType != PERF_COUNTER_BULK_COUNT || writeType != PERF_COUNTER_BULK_COUNT ||
        !IsValidRawCounter(read) || !IsValidRawCounter(write)) {
        return result;
    }

    result.available = true;
    result.readBytes = static_cast<std::uint64_t>(read.FirstValue);
    result.writtenBytes = static_cast<std::uint64_t>(write.FirstValue);
    result.capturedAt100ns = capturedAt100ns;
    result.sourceIdentity = kDiskSourceIdentity;
    return result;
}

SystemIoCounters WindowsSystemIoCounterSampler::Capture() {
    ULONGLONG capturedAt100ns = 0;
    QueryUnbiasedInterruptTimePrecise(&capturedAt100ns);

    SystemIoCounters result{};
    result.network = CaptureNetwork(static_cast<std::uint64_t>(capturedAt100ns));
    result.disk = CaptureDisk(static_cast<std::uint64_t>(capturedAt100ns));
    return result;
}

}  // namespace codex_monitor
