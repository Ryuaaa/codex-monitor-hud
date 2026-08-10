#include "service_status_model.h"

#include <iostream>
#include <optional>
#include <string_view>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

struct ComponentCase {
    std::string_view rawStatus;
    codex_monitor::OpenAIServiceHealth expectedHealth;
    std::string_view expectedHeadline;
};

void TestCodexComponentMappings() {
    constexpr ComponentCase cases[] = {
        {"operational",
         codex_monitor::OpenAIServiceHealth::kOperational,
         "Codex operational"},
        {"degraded_performance",
         codex_monitor::OpenAIServiceHealth::kDegraded,
         "Codex performance degraded"},
        {"partial_outage",
         codex_monitor::OpenAIServiceHealth::kPartialOutage,
         "Codex partial outage"},
        {"major_outage",
         codex_monitor::OpenAIServiceHealth::kMajorOutage,
         "Codex major outage"},
        {"under_maintenance",
         codex_monitor::OpenAIServiceHealth::kMaintenance,
         "Codex under maintenance"},
    };

    for (const ComponentCase& test : cases) {
        const auto result = codex_monitor::MapOpenAIServiceStatus(
            "none", test.rawStatus);
        Expect(result.health == test.expectedHealth,
               "component status must map to the stable health enum");
        Expect(result.headline == test.expectedHeadline,
               "component status must map to stable Codex UI text");
        Expect(result.detail == "Overall OpenAI status: Operational",
               "component detail must still report the overall service state");
    }
}

struct OverallCase {
    std::string_view rawIndicator;
    codex_monitor::OpenAIServiceHealth expectedHealth;
    std::string_view expectedHeadline;
};

void TestOverallFallbackMappings() {
    constexpr OverallCase cases[] = {
        {"none",
         codex_monitor::OpenAIServiceHealth::kOperational,
         "OpenAI operational"},
        {"minor",
         codex_monitor::OpenAIServiceHealth::kDegraded,
         "OpenAI performance degraded"},
        {"major",
         codex_monitor::OpenAIServiceHealth::kMajorOutage,
         "OpenAI service outage"},
        {"critical",
         codex_monitor::OpenAIServiceHealth::kMajorOutage,
         "OpenAI critical service outage"},
        {"maintenance",
         codex_monitor::OpenAIServiceHealth::kMaintenance,
         "OpenAI under maintenance"},
    };

    for (const OverallCase& test : cases) {
        const auto missingComponent = codex_monitor::MapOpenAIServiceStatus(
            test.rawIndicator, std::nullopt);
        Expect(missingComponent.health == test.expectedHealth,
               "missing Codex status must fall back to the overall enum");
        Expect(missingComponent.headline == test.expectedHeadline,
               "missing Codex status must fall back to the overall headline");
        Expect(missingComponent.detail ==
                   "Codex-specific status unavailable; using overall OpenAI status.",
               "overall fallback must be explicit in the UI detail");

        const auto unknownComponent = codex_monitor::MapOpenAIServiceStatus(
            test.rawIndicator, "future_component_state");
        Expect(unknownComponent.health == test.expectedHealth &&
                   unknownComponent.headline == test.expectedHeadline,
               "unknown Codex status must safely fall back to overall status");
    }
}

void TestComponentTakesPrecedence() {
    const auto result = codex_monitor::MapOpenAIServiceStatus(
        "critical", "operational");
    Expect(result.health == codex_monitor::OpenAIServiceHealth::kOperational &&
               result.headline == "Codex operational",
           "a recognized Codex component must drive the headline");
    Expect(result.detail == "Overall OpenAI status: Critical service outage",
           "the overall status must remain visible when Codex is operational");
}

void TestUnknownAndMissingValuesStayUnknown() {
    const auto unknown = codex_monitor::MapOpenAIServiceStatus(
        "future_indicator", "future_component_state");
    Expect(unknown.health == codex_monitor::OpenAIServiceHealth::kUnknown &&
               unknown.headline == "Service status unavailable" &&
               unknown.detail ==
                   "OpenAI did not return a recognized service status.",
           "unknown values must not be presented as healthy");

    const auto missing = codex_monitor::MapOpenAIServiceStatus("", std::nullopt);
    Expect(missing.health == codex_monitor::OpenAIServiceHealth::kUnknown,
           "missing values must remain unknown");
}

void TestMappingIsCaseInsensitiveAndTrimmed() {
    const auto result = codex_monitor::MapOpenAIServiceStatus(
        "  MINOR\t", "  PARTIAL_OUTAGE ");
    Expect(result.health == codex_monitor::OpenAIServiceHealth::kPartialOutage &&
               result.headline == "Codex partial outage" &&
               result.detail ==
                   "Overall OpenAI status: Degraded performance",
           "status mapping must tolerate surrounding whitespace and ASCII case");
}

}  // namespace

int main() {
    TestCodexComponentMappings();
    TestOverallFallbackMappings();
    TestComponentTakesPrecedence();
    TestUnknownAndMissingValuesStayUnknown();
    TestMappingIsCaseInsensitiveAndTrimmed();
    if (failures != 0) return 1;
    std::cout << "service_status_model_tests=pass\n";
    return 0;
}
