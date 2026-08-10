#include "service_status_model.h"

#include <cctype>

namespace codex_monitor {
namespace {

struct StatusText {
    OpenAIServiceHealth health;
    std::string_view componentHeadline;
    std::string_view overallHeadline;
    std::string_view overallDetail;
};

std::string Normalize(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    std::string normalized;
    normalized.reserve(end - begin);
    for (std::size_t index = begin; index < end; ++index) {
        const unsigned char character = static_cast<unsigned char>(value[index]);
        normalized.push_back(static_cast<char>(std::tolower(character)));
    }
    return normalized;
}

std::optional<StatusText> ComponentText(std::string_view rawStatus) {
    const std::string status = Normalize(rawStatus);
    if (status == "operational") {
        return StatusText{OpenAIServiceHealth::kOperational,
                          "Codex operational",
                          {},
                          {}};
    }
    if (status == "degraded_performance") {
        return StatusText{OpenAIServiceHealth::kDegraded,
                          "Codex performance degraded",
                          {},
                          {}};
    }
    if (status == "partial_outage") {
        return StatusText{OpenAIServiceHealth::kPartialOutage,
                          "Codex partial outage",
                          {},
                          {}};
    }
    if (status == "major_outage") {
        return StatusText{OpenAIServiceHealth::kMajorOutage,
                          "Codex major outage",
                          {},
                          {}};
    }
    if (status == "under_maintenance") {
        return StatusText{OpenAIServiceHealth::kMaintenance,
                          "Codex under maintenance",
                          {},
                          {}};
    }
    return std::nullopt;
}

std::optional<StatusText> OverallText(std::string_view rawIndicator) {
    const std::string indicator = Normalize(rawIndicator);
    if (indicator == "none") {
        return StatusText{OpenAIServiceHealth::kOperational,
                          {},
                          "OpenAI operational",
                          "Overall OpenAI status: Operational"};
    }
    if (indicator == "minor") {
        return StatusText{OpenAIServiceHealth::kDegraded,
                          {},
                          "OpenAI performance degraded",
                          "Overall OpenAI status: Degraded performance"};
    }
    if (indicator == "major") {
        return StatusText{OpenAIServiceHealth::kMajorOutage,
                          {},
                          "OpenAI service outage",
                          "Overall OpenAI status: Service outage"};
    }
    if (indicator == "critical") {
        return StatusText{OpenAIServiceHealth::kMajorOutage,
                          {},
                          "OpenAI critical service outage",
                          "Overall OpenAI status: Critical service outage"};
    }
    if (indicator == "maintenance") {
        return StatusText{OpenAIServiceHealth::kMaintenance,
                          {},
                          "OpenAI under maintenance",
                          "Overall OpenAI status: Maintenance"};
    }
    return std::nullopt;
}

}  // namespace

OpenAIServiceStatusModel MapOpenAIServiceStatus(
    std::string_view overallIndicator,
    std::optional<std::string_view> codexComponentStatus) {
    const std::optional<StatusText> overall = OverallText(overallIndicator);
    const std::optional<StatusText> component = codexComponentStatus
        ? ComponentText(*codexComponentStatus)
        : std::nullopt;

    if (component) {
        return OpenAIServiceStatusModel{
            component->health,
            std::string(component->componentHeadline),
            overall ? std::string(overall->overallDetail)
                    : "Overall OpenAI status: Unknown",
        };
    }
    if (overall) {
        return OpenAIServiceStatusModel{
            overall->health,
            std::string(overall->overallHeadline),
            "Codex-specific status unavailable; using overall OpenAI status.",
        };
    }
    return OpenAIServiceStatusModel{
        OpenAIServiceHealth::kUnknown,
        "Service status unavailable",
        "OpenAI did not return a recognized service status.",
    };
}

}  // namespace codex_monitor
