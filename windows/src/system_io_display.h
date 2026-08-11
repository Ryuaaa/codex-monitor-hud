#pragma once

#include "system_io_rate.h"

#include <optional>
#include <string>

namespace codex_monitor {

// Formats a binary byte rate for compact HUD display. Missing values remain
// distinguishable from a valid zero rate, and a reset/first sample is exposed
// as a baseline transition rather than fabricated as zero.
std::wstring FormatSystemIoByteRate(
    const std::optional<double>& bytesPerSecond,
    bool needsBaseline);

// Builds the complete Computer-page card from the already sampled rates. This
// function is platform-independent and performs no sampling or network I/O.
std::wstring BuildSystemIoThroughputCardText(const SystemIoRates& rates);

}  // namespace codex_monitor
