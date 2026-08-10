#include "service_status_json_win32.h"

#include <winrt/base.h>

#include <iostream>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

void TestOperationalSummary() {
    const auto result = codex_monitor::ParseOpenAIServiceStatusSummaryJson(LR"json(
        {
          "status": {"indicator": "none", "description": "All Systems Operational"},
          "components": [
            {"name": "API", "status": "operational"},
            {"name": "Codex in ChatGPT Desktop", "status": "operational"}
          ]
        }
    )json");
    Expect(result && result->overallIndicator == "none" &&
               result->codexComponentStatus == "operational",
           "a normal summary must retain both raw status values");
}

void TestDegradedSummaryAndTolerantExactName() {
    const auto result = codex_monitor::ParseOpenAIServiceStatusSummaryJson(LR"json(
        {
          "status": {"indicator": "MINOR"},
          "components": [
            {"name": "Codex in ChatGPT Desktop Preview", "status": "major_outage"},
            {"name": "  cOdEx In ChAtGpT dEsKtOp\t", "status": "degraded_performance"}
          ]
        }
    )json");
    Expect(result && result->overallIndicator == "MINOR" &&
               result->codexComponentStatus == "degraded_performance",
           "component matching must trim and ignore ASCII case but stay exact");
}

void TestMissingComponentIsValid() {
    const auto noMatch = codex_monitor::ParseOpenAIServiceStatusSummaryJson(LR"json(
        {
          "status": {"indicator": "major"},
          "components": [
            {"name": "My Codex in ChatGPT Desktop", "status": "operational"},
            {"name": "Codex in ChatGPT Desktop Legacy", "status": "operational"}
          ]
        }
    )json");
    Expect(noMatch && noMatch->overallIndicator == "major" &&
               !noMatch->codexComponentStatus,
           "similar unrelated component names must not be selected");

    const auto noComponents = codex_monitor::ParseOpenAIServiceStatusSummaryJson(
        LR"json({"status":{"indicator":"none"}})json");
    Expect(noComponents && !noComponents->codexComponentStatus,
           "a missing component array must retain the overall fallback");
}

void TestMalformedJsonFails() {
    const auto result = codex_monitor::ParseOpenAIServiceStatusSummaryJson(
        LR"json({"status":{"indicator":"none"},"components":[})json");
    Expect(!result, "malformed summary JSON must fail parsing");
}

void TestKnownFieldTypeErrorsFail() {
    const auto wrongStatus = codex_monitor::ParseOpenAIServiceStatusSummaryJson(
        LR"json({"status":[],"components":[]})json");
    Expect(!wrongStatus, "the root status field must be an object");

    const auto wrongIndicator = codex_monitor::ParseOpenAIServiceStatusSummaryJson(
        LR"json({"status":{"indicator":1},"components":[]})json");
    Expect(!wrongIndicator, "the overall indicator must be a string");

    const auto wrongComponents = codex_monitor::ParseOpenAIServiceStatusSummaryJson(
        LR"json({"status":{"indicator":"none"},"components":{}})json");
    Expect(!wrongComponents, "components must be an array when present");

    const auto wrongCodexStatus = codex_monitor::ParseOpenAIServiceStatusSummaryJson(
        LR"json({
          "status":{"indicator":"none"},
          "components":[
            {"name":"Codex in ChatGPT Desktop","status":false}
          ]
        })json");
    Expect(!wrongCodexStatus, "a matched Codex component status must be a string");
}

}  // namespace

int main() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    TestOperationalSummary();
    TestDegradedSummaryAndTolerantExactName();
    TestMissingComponentIsValid();
    TestMalformedJsonFails();
    TestKnownFieldTypeErrorsFail();
    if (failures != 0) return 1;
    std::cout << "service_status_json_tests=pass\n";
    return 0;
}
