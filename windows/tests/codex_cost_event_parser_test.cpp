#include "codex/codex_cost_event_parser.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <utility>

namespace {

using codex_monitor::codex::CodexCostEventParserState;
using codex_monitor::codex::CodexCostLineDisposition;
using codex_monitor::codex::CodexCostLineParseResult;
using codex_monitor::codex::ParseCodexCostJsonlLine;
using codex_monitor::codex::ParsedCodexCostEvent;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

bool EndsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) ==
               0;
}

ParsedCodexCostEvent RequireEvent(CodexCostLineParseResult result,
                                  const char* message) {
    Expect(result.disposition == CodexCostLineDisposition::kEvent &&
               result.event.has_value(),
           message);
    return std::move(*result.event);
}

void TestTurnContextAndLastUsage() {
    CodexCostEventParserState state;
    const auto context = ParseCodexCostJsonlLine(
        R"json({"timestamp":"2026-08-11T00:00:00Z","type":"turn_context","payload":{"model":"openai/gpt-5.6","private":"must-not-survive"},"cwd":"C:\\private"})json",
        state);
    Expect(context.disposition == CodexCostLineDisposition::kStateUpdated &&
               state.currentModel == "gpt-5.6-sol",
           "turn context updates only the normalized model");

    const std::string line =
        R"json({"type":"event_msg","timestamp":"2026-08-11T01:02:03.45Z","payload":{"type":"token_count","info":{"last_token_usage":{"input_tokens":100,"cached_input_tokens":20,"cache_creation_input_tokens":5,"output_tokens":10}}},"message":"private"})json";
    const auto& event = RequireEvent(ParseCodexCostJsonlLine(line, state),
                                     "last usage produces an event");
    Expect(event.model == "gpt-5.6-sol", "context model is inherited");
    Expect(event.usage.inputTokens == 100 &&
               event.usage.cachedInputTokens == 20 &&
               event.usage.cacheWriteInputTokens == 5 &&
               event.usage.outputTokens == 10,
           "token tuple and cache-creation fallback are preserved");
    Expect(event.timestampUnixMilliseconds == 1786410123450LL,
           "fractional UTC timestamp is converted exactly");
    Expect(event.fingerprint == "6830bb6af2bdc309#1",
           "event fingerprint matches the frozen macOS FNV identity");
}

void TestCumulativeWatermarkAndReset() {
    CodexCostEventParserState state;
    const auto parseTotal = [&state](std::int64_t input,
                                     std::int64_t cached,
                                     std::int64_t output,
                                     int second) {
        const std::string line =
            std::string("{\"timestamp\":\"2026-08-11T00:00:") +
            (second < 10 ? "0" : "") + std::to_string(second) +
            "Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\",\"info\":{\"total_token_usage\":{\"input_tokens\":" +
            std::to_string(input) +
            ",\"cached_input_tokens\":" + std::to_string(cached) +
            ",\"output_tokens\":" + std::to_string(output) + "}}}}";
        return ParseCodexCostJsonlLine(line, state);
    };

    const auto& first = RequireEvent(parseTotal(100, 40, 10, 1),
                                     "first total counts in full");
    Expect(first.usage.inputTokens == 100 &&
               first.usage.cachedInputTokens == 40 &&
               first.usage.outputTokens == 10,
           "first total tuple");
    const auto repeated = parseTotal(100, 40, 10, 2);
    Expect(repeated.disposition == CodexCostLineDisposition::kStateUpdated &&
               !repeated.event,
           "repeated cumulative total is not counted twice");
    const auto& increment = RequireEvent(parseTotal(150, 55, 15, 3),
                                         "larger total emits only delta");
    Expect(increment.usage.inputTokens == 50 &&
               increment.usage.cachedInputTokens == 15 &&
               increment.usage.outputTokens == 5,
           "component-wise high-water delta");
    Expect(parseTotal(20, 5, 2, 4).disposition ==
               CodexCostLineDisposition::kStateUpdated,
           "counter reset below the watermark does not double-count");
    const auto& afterReset = RequireEvent(parseTotal(170, 50, 20, 5),
                                          "post-reset total above watermark emits");
    Expect(afterReset.usage.inputTokens == 20 &&
               afterReset.usage.cachedInputTokens == 0 &&
               afterReset.usage.outputTokens == 5,
           "watermark never moves backward during a source stream");
}

void TestUnknownModelsAndOccurrenceIdentity() {
    CodexCostEventParserState state;
    const std::string line =
        R"json({"timestamp":"2026-08-11T00:00:00Z","type":"event_msg","payload":{"type":"token_count","info":{"last_token_usage":{"input_tokens":1,"output_tokens":1}}}})json";
    const auto& first = RequireEvent(ParseCodexCostJsonlLine(line, state),
                                     "missing model event");
    const auto& second = RequireEvent(ParseCodexCostJsonlLine(line, state),
                                      "identical last usage is a separate event");
    Expect(first.model == "unknown" && second.model == "unknown",
           "missing model remains explicitly unknown");
    Expect(EndsWith(first.fingerprint, "#1") &&
               EndsWith(second.fingerprint, "#2") &&
               first.fingerprint != second.fingerprint,
           "identical last-usage events receive deterministic occurrences");

    const auto& futureModel = RequireEvent(
        ParseCodexCostJsonlLine(
            R"json({"timestamp":"2026-08-11T00:00:01Z","type":"event_msg","payload":{"type":"token_count","info":{"model":"future-model","last_token_usage":{"input_tokens":2,"output_tokens":0}}}})json",
            state),
        "unknown price-table model is still retained");
    Expect(futureModel.model == "future-model",
           "unpriced model identity remains available for coverage reporting");
}

void TestNegativeAndInvalidTokenFields() {
    CodexCostEventParserState state;
    const auto& clipped = RequireEvent(
        ParseCodexCostJsonlLine(
            R"json({"timestamp":"2026-08-11T00:00:00Z","type":"event_msg","payload":{"type":"token_count","info":{"last_token_usage":{"input_tokens":-8,"cached_input_tokens":-3,"cache_write_input_tokens":-2,"output_tokens":4}}}})json",
            state),
        "positive output survives negative companion fields");
    Expect(clipped.usage.inputTokens == 0 &&
               clipped.usage.cachedInputTokens == 0 &&
               clipped.usage.cacheWriteInputTokens == 0 &&
               clipped.usage.outputTokens == 4,
           "negative values are clipped without wrapping");

    const auto allNegative = ParseCodexCostJsonlLine(
        R"json({"timestamp":"2026-08-11T00:00:01Z","type":"event_msg","payload":{"type":"token_count","info":{"last_token_usage":{"input_tokens":-1,"output_tokens":-1}}}})json",
        state);
    Expect(allNegative.disposition == CodexCostLineDisposition::kIgnored,
           "all-negative usage is ignored");
    const auto fractional = ParseCodexCostJsonlLine(
        R"json({"timestamp":"2026-08-11T00:00:02Z","type":"event_msg","payload":{"type":"token_count","info":{"last_token_usage":{"input_tokens":1.5,"output_tokens":2}}}})json",
        state);
    Expect(fractional.disposition == CodexCostLineDisposition::kIgnored,
           "non-integral token objects are rejected rather than truncated");

    const auto& fallback = RequireEvent(
        ParseCodexCostJsonlLine(
            R"json({"timestamp":"2026-08-11T00:00:03Z","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":-1,"output_tokens":0},"last_token_usage":{"input_tokens":6,"output_tokens":1}}}})json",
            state),
        "unusable total falls back to last usage");
    Expect(fallback.usage.inputTokens == 6 &&
               fallback.usage.outputTokens == 1 &&
               !state.hasRawTotalsWatermark,
           "all-nonpositive total does not create or advance a watermark");
}

void TestExtremeIntegers() {
    CodexCostEventParserState state;
    const auto& maximum = RequireEvent(
        ParseCodexCostJsonlLine(
            R"json({"timestamp":"2026-08-11T00:00:00Z","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":9223372036854775807,"cached_input_tokens":9223372036854775807,"output_tokens":9223372036854775807}}}})json",
            state),
        "int64 maximum is accepted without summing overflow");
    Expect(maximum.usage.inputTokens ==
                   std::numeric_limits<std::int64_t>::max() &&
               maximum.usage.outputTokens ==
                   std::numeric_limits<std::int64_t>::max(),
           "extreme token fields remain exact");

    const auto overflow = ParseCodexCostJsonlLine(
        R"json({"timestamp":"2026-08-11T00:00:01Z","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":9223372036854775808,"output_tokens":1}}}})json",
        state);
    Expect(overflow.disposition == CodexCostLineDisposition::kIgnored,
           "out-of-range usage object is rejected safely");
    Expect(state.rawTotalsWatermark.inputTokens ==
               std::numeric_limits<std::int64_t>::max(),
           "rejected extreme value cannot corrupt the watermark");
}

void TestMalformedAndTimestampValidation() {
    CodexCostEventParserState state;
    const auto malformed = ParseCodexCostJsonlLine(
        R"json({"type":"turn_context","payload":{"model":"gpt-5.6-terra"})json",
        state);
    Expect(malformed.disposition == CodexCostLineDisposition::kMalformed &&
               state.currentModel == "unknown",
           "malformed JSON never mutates state");
    Expect(ParseCodexCostJsonlLine("[]", state).disposition ==
               CodexCostLineDisposition::kMalformed,
           "root must be one complete JSON object");
    Expect(ParseCodexCostJsonlLine(
               R"json({"type":"event_msg","timestamp":"not-a-date","payload":{"type":"token_count","info":{"last_token_usage":{"input_tokens":1}}}})json",
               state)
               .disposition == CodexCostLineDisposition::kIgnored,
           "invalid timestamps cannot emit summary events");

    const auto& offset = RequireEvent(
        ParseCodexCostJsonlLine(
            R"json({"type":"event_msg","timestamp":"2026-08-11T00:30:00+08:00","payload":{"type":"token_count","info":{"last_token_usage":{"input_tokens":1}}}})json",
            state),
        "offset timestamp event");
    Expect(offset.timestampUnixMilliseconds == 1786379400000LL,
           "ISO offset is converted to an absolute timestamp");
}

}  // namespace

int main() {
    TestTurnContextAndLastUsage();
    TestCumulativeWatermarkAndReset();
    TestUnknownModelsAndOccurrenceIdentity();
    TestNegativeAndInvalidTokenFields();
    TestExtremeIntegers();
    TestMalformedAndTimestampValidation();
    std::cout << "codex_cost_event_parser_test: pass\n";
    return 0;
}
