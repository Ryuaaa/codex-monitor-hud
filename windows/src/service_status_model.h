#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace codex_monitor {

// Stable application-level values. Raw Statuspage strings stay outside the UI
// so a future provider change does not leak into rendering or settings code.
enum class OpenAIServiceHealth {
    kUnknown,
    kOperational,
    kDegraded,
    kPartialOutage,
    kMajorOutage,
    kMaintenance,
};

struct OpenAIServiceStatusModel {
    OpenAIServiceHealth health = OpenAIServiceHealth::kUnknown;
    std::string headline;
    std::string detail;
};

// Maps the public OpenAI Statuspage values to stable English UI text. A known
// Codex component status takes precedence; otherwise the overall indicator is
// used as a fallback. This function performs no I/O.
OpenAIServiceStatusModel MapOpenAIServiceStatus(
    std::string_view overallIndicator,
    std::optional<std::string_view> codexComponentStatus = std::nullopt);

}  // namespace codex_monitor
