#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace codex_monitor {

struct ParsedOpenAIServiceStatus {
    std::string overallIndicator;
    std::optional<std::string> codexComponentStatus;
};

// Parses a complete Statuspage summary payload. The returned strings preserve
// the raw API values; presentation mapping belongs to service_status_model.
// This Windows-only implementation performs no network I/O.
std::optional<ParsedOpenAIServiceStatus> ParseOpenAIServiceStatusSummaryJson(
    std::wstring_view json) noexcept;

}  // namespace codex_monitor
