#include "codex/codex_cost_history_state.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

namespace {

using namespace codex_monitor::codex;

void Expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

CodexCostFileLine Line(std::string fileId,
                       std::uint64_t begin,
                       std::string text) {
    CodexCostFileLine line;
    line.fileId = std::move(fileId);
    line.beginOffsetBytes = begin;
    line.endOffsetBytes = begin + text.size() + 1;
    line.text = std::move(text);
    return line;
}

CodexCostFileCursor Cursor(std::string fileId,
                           std::uint64_t offset,
                           bool reset = false) {
    CodexCostFileCursor cursor;
    cursor.fileId = std::move(fileId);
    cursor.parsedOffsetBytes = offset;
    cursor.resetAfterTruncation = reset;
    return cursor;
}

std::optional<std::string> FixedDate(std::int64_t milliseconds) {
    if (milliseconds <= 0) return std::nullopt;
    return milliseconds < 1770000000000LL ? "2026-02-02" : "2026-02-03";
}

void TestIncrementalCompactionAndFileNamespace() {
    CodexCostHistoryState state;
    CodexCostFileScanResult first;
    first.status = CodexCostFileScanStatus::kOk;
    first.files = {Cursor("file-a", 200), Cursor("file-b", 200)};
    const std::string model =
        R"({"timestamp":"2026-02-02T02:00:00Z","type":"turn_context","payload":{"model":"gpt-5.6-terra"}})";
    const std::string token =
        R"({"timestamp":"2026-02-02T02:01:00Z","type":"event_msg","payload":{"type":"token_count","info":{"last_token_usage":{"input_tokens":100,"cached_input_tokens":20,"output_tokens":10}}}})";
    first.lines = {Line("file-a", 0, model), Line("file-a", 100, token),
                   Line("file-b", 0, model), Line("file-b", 100, token)};
    CodexCostHistoryApplyResult applied = state.Apply(first, FixedDate);
    Expect(applied.events.size() == 2,
           "the same day and model in separate files stay namespaced");
    Expect(applied.events[0].usage.inputTokens == 100,
           "the first batch is counted");

    CodexCostFileScanResult second;
    second.status = CodexCostFileScanStatus::kOk;
    second.files = {Cursor("file-a", 300), Cursor("file-b", 200)};
    second.lines = {Line("file-a", 200, token)};
    applied = state.Apply(second, FixedDate);
    Expect(applied.events.size() == 2,
           "incremental events compact into existing file/day/model rows");
    std::int64_t totalInput = 0;
    for (const CodexCostEvent& event : applied.events) {
        totalInput += event.usage.inputTokens;
    }
    Expect(totalInput == 300,
           "only newly delivered records are added across scans");
    Expect(state.Cursors().size() == 2,
           "scanner cursors are retained without source paths");
}

void TestTruncationAndRemovedFiles() {
    CodexCostHistoryState state;
    CodexCostFileScanResult first;
    first.status = CodexCostFileScanStatus::kOk;
    first.files = {Cursor("old", 100), Cursor("keep", 100)};
    const std::string token =
        R"({"timestamp":"2026-02-02T02:01:00Z","type":"event_msg","payload":{"type":"token_count","info":{"model":"gpt-5.6-luna","last_token_usage":{"input_tokens":100,"output_tokens":10}}}})";
    first.lines = {Line("old", 0, token), Line("keep", 0, token)};
    const CodexCostHistoryApplyResult initial = state.Apply(first, FixedDate);
    Expect(initial.events.size() == 2, "the initial files are retained");

    CodexCostFileScanResult next;
    next.status = CodexCostFileScanStatus::kOk;
    next.files = {Cursor("keep", 50, true)};
    const std::string replacement =
        R"({"timestamp":"2026-02-02T03:01:00Z","type":"event_msg","payload":{"type":"token_count","info":{"model":"gpt-5.6-luna","last_token_usage":{"input_tokens":7,"output_tokens":3}}}})";
    next.lines = {Line("keep", 0, replacement)};
    const CodexCostHistoryApplyResult applied = state.Apply(next, FixedDate);
    Expect(applied.events.size() == 1 &&
               applied.events.front().usage.inputTokens == 7,
           "truncation resets one file and absent candidates are evicted");
}

void TestFailureRetainsLastGoodAggregate() {
    CodexCostHistoryState state;
    CodexCostFileScanResult good;
    good.status = CodexCostFileScanStatus::kOk;
    good.files = {Cursor("file", 100)};
    good.lines = {Line(
        "file", 0,
        R"({"timestamp":"2026-02-02T02:01:00Z","type":"event_msg","payload":{"type":"token_count","info":{"model":"future-model","last_token_usage":{"input_tokens":10,"output_tokens":2}}}})")};
    const CodexCostHistoryApplyResult initial = state.Apply(good, FixedDate);
    Expect(initial.events.size() == 1, "the initial good scan is retained");
    CodexCostFileScanResult failed;
    failed.status = CodexCostFileScanStatus::kIoError;
    const CodexCostHistoryApplyResult retained = state.Apply(failed, FixedDate);
    Expect(retained.events.size() == 1 &&
               retained.events.front().cachedPricedTokens == 0,
           "a failed scan retains the last good unknown-model aggregate");
}

void TestInstallTimestampRejectsOlderDeliveredLines() {
    CodexCostHistoryState state;
    CodexCostFileScanResult scan;
    scan.status = CodexCostFileScanStatus::kOk;
    scan.files = {Cursor("copied-after-install", 300)};
    scan.lines = {
        Line("copied-after-install", 0,
             R"({"timestamp":"2026-02-02T02:00:00Z","type":"event_msg","payload":{"type":"token_count","info":{"model":"gpt-5.6-luna","last_token_usage":{"input_tokens":100,"output_tokens":10}}}})"),
        Line("copied-after-install", 150,
             R"({"timestamp":"2026-02-02T03:00:00Z","type":"event_msg","payload":{"type":"token_count","info":{"model":"gpt-5.6-luna","last_token_usage":{"input_tokens":200,"output_tokens":20}}}})"),
    };
    const auto applied = state.Apply(scan, FixedDate, 1'770'000'600'000LL);
    Expect(applied.events.size() == 1 &&
               applied.events.front().usage.inputTokens == 200,
           "a file discovered later must still exclude events older than installation");
}

void TestInstallBaselineSeedsModelWithoutOldEvents() {
    CodexCostHistoryState state;
    CodexCostFileScanResult baseline;
    baseline.status = CodexCostFileScanStatus::kOk;
    CodexCostFileCursor cursor = Cursor("active-at-install", 200);
    cursor.establishBaseline = true;
    cursor.baselineModel = "openai/gpt-5.6-terra";
    baseline.files = {cursor};
    const auto installed = state.Apply(baseline, FixedDate);
    Expect(installed.events.empty(),
           "the install baseline must not create historical Token events");

    CodexCostFileScanResult appended;
    appended.status = CodexCostFileScanStatus::kOk;
    appended.files = {Cursor("active-at-install", 300)};
    appended.lines = {Line(
        "active-at-install", 200,
        R"({"timestamp":"2026-02-03T02:01:00Z","type":"event_msg","payload":{"type":"token_count","info":{"last_token_usage":{"input_tokens":10,"output_tokens":2}}}})")};
    const auto applied = state.Apply(appended, FixedDate, 1770000000000LL);
    Expect(applied.events.size() == 1 &&
               applied.events.front().model == "gpt-5.6-terra" &&
               applied.events.front().usage.inputTokens == 10,
           "post-install Token events must inherit the bounded baseline model");
}

void TestPartialDiscoveryDoesNotEvictLastGoodFile() {
    CodexCostHistoryState state;
    CodexCostFileScanResult first;
    first.status = CodexCostFileScanStatus::kOk;
    first.files = {Cursor("temporarily-missing", 100)};
    first.lines = {Line(
        "temporarily-missing", 0,
        R"({"timestamp":"2026-02-02T02:01:00Z","type":"event_msg","payload":{"type":"token_count","info":{"model":"gpt-5.6-luna","last_token_usage":{"input_tokens":10,"output_tokens":2}}}})")};
    const auto initial = state.Apply(first, FixedDate);
    Expect(initial.events.size() == 1, "the baseline file must be retained");

    CodexCostFileScanResult partial;
    partial.status = CodexCostFileScanStatus::kOk;
    partial.discoveryIncomplete = true;
    partial.coverageIncomplete = true;
    const auto retained = state.Apply(partial, FixedDate);
    Expect(retained.events.size() == 1 && state.Cursors().size() == 1,
           "partial discovery must not interpret a missing file as deletion");

    CodexCostFileScanResult complete;
    complete.status = CodexCostFileScanStatus::kOk;
    const auto evicted = state.Apply(complete, FixedDate);
    Expect(evicted.events.empty() && state.Cursors().empty(),
           "complete discovery may evict a file that is truly absent");
}

void TestContentGapDoesNotPreventDeletedFileEviction() {
    CodexCostHistoryState state;
    CodexCostFileScanResult first;
    first.status = CodexCostFileScanStatus::kOk;
    first.files = {Cursor("deleted-before-content-gap", 100)};
    first.lines = {Line(
        "deleted-before-content-gap", 0,
        R"({"timestamp":"2026-02-02T02:01:00Z","type":"event_msg","payload":{"type":"token_count","info":{"model":"gpt-5.6-luna","last_token_usage":{"input_tokens":10,"output_tokens":2}}}})")};
    Expect(state.Apply(first, FixedDate).events.size() == 1,
           "the content-gap fixture must retain one baseline event");

    CodexCostFileScanResult contentGap;
    contentGap.status = CodexCostFileScanStatus::kOk;
    contentGap.coverageIncomplete = true;
    const auto evicted = state.Apply(contentGap, FixedDate);
    Expect(evicted.events.empty() && state.Cursors().empty(),
           "compressed or oversized content gaps must not retain a deleted file forever");
}

}  // namespace

int main() {
    TestIncrementalCompactionAndFileNamespace();
    TestTruncationAndRemovedFiles();
    TestFailureRetainsLastGoodAggregate();
    TestInstallTimestampRejectsOlderDeliveredLines();
    TestInstallBaselineSeedsModelWithoutOldEvents();
    TestPartialDiscoveryDoesNotEvictLastGoodFile();
    TestContentGapDoesNotPreventDeletedFileEviction();
    std::cout << "codex_cost_history_state_test: pass\n";
    return 0;
}
