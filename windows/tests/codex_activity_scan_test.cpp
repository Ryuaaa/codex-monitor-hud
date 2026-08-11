#include "codex/codex_activity_scan.h"
#include "codex/codex_cost_file_scan.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto tick = std::chrono::steady_clock::now()
                              .time_since_epoch()
                              .count();
        path_ = std::filesystem::temp_directory_path() /
                ("codex-activity-test-" + std::to_string(tick));
        std::error_code error;
        std::filesystem::create_directories(path_, error);
        Expect(!error, "the activity test root must be created");
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

std::string Timestamp(std::int64_t unixSeconds) {
    const std::time_t value = static_cast<std::time_t>(unixSeconds);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &value);
#else
    gmtime_r(&value, &utc);
#endif
    char output[32]{};
    const std::size_t count = std::strftime(
        output, sizeof(output), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return count == 20 ? std::string(output, count) : std::string{};
}

std::string StartLine(std::int64_t unixSeconds) {
    return "{\"timestamp\":\"" + Timestamp(unixSeconds) +
           "\",\"type\":\"response_item\",\"payload\":{"
           "\"type\":\"message\",\"role\":\"user\","
           "\"content\":[{\"text\":\"private prompt\"}]}}";
}

std::string FinishLine(std::int64_t unixSeconds) {
    return "{\"timestamp\":\"" + Timestamp(unixSeconds) +
           "\",\"type\":\"response_item\",\"payload\":{"
           "\"type\":\"message\",\"role\":\"assistant\","
           "\"phase\":\"final_answer\","
           "\"content\":[{\"text\":\"private answer\"}]}}";
}

void WriteLines(const std::filesystem::path& path,
                const std::vector<std::string>& lines) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    for (const std::string& line : lines) output << line << '\n';
    output.close();
    Expect(output.good(), "the activity fixture must be written");
}

void TestPureInferenceMatchesMacSafetyRules() {
    using namespace codex_monitor::codex;
    constexpr std::int64_t now = 1767225720000LL;
    const std::vector<std::string> started = {
        R"json({"timestamp":"2026-01-01T00:01:00Z","type":"response_item","payload":{"type":"message","role":"user","content":[{"text":"never retain me"}]}})json"};
    const std::vector<std::string> completed = {
        started.front(),
        R"json({"timestamp":"2026-01-01T00:01:40Z","type":"response_item","payload":{"type":"message","role":"assistant","phase":"final_answer","content":[{"text":"also private"}]}})json"};

    const CodexActivityFileState active = InferCodexActivityFileState(
        started, now - 10'000, now);
    Expect(active.active && active.durationSeconds == 60,
           "a recent unfinished user turn must be active for 60 seconds");
    Expect(!InferCodexActivityFileState(completed, now - 10'000, now).active,
           "a later final answer must complete the turn");
    Expect(!InferCodexActivityFileState(started, now - 121'000, now).active,
           "a file outside the 120-second activity window must be idle");
    Expect(!InferCodexActivityFileState(started, now + 121'000, now).active,
           "a file modified far in the future must not be active");
    const std::vector<std::string> futureEvent = {
        R"json({"timestamp":"2099-01-01T00:01:00Z","type":"event_msg","payload":{"type":"turn_started"}})json"};
    Expect(!InferCodexActivityFileState(futureEvent, now - 10'000, now).active,
           "a far-future event timestamp must not invent activity");

    const CodexActivityLineResult eventStart = ParseCodexActivityJsonlLine(
        R"json({"timestamp":"2026-01-01T08:01:00+08:00","type":"event_msg","payload":{"type":"turn_started","message":"private"}})json");
    Expect(eventStart.disposition == CodexActivityLineDisposition::kStarted &&
               eventStart.timestampUnixMilliseconds == now - 60'000,
           "documented event messages and timezone offsets must parse");
    const CodexActivityLineResult eventFinish = ParseCodexActivityJsonlLine(
        R"json({"timestamp":"2026-01-01T00:01:30Z","type":"event_msg","payload":{"type":"turn_completed"}})json");
    Expect(eventFinish.disposition ==
               CodexActivityLineDisposition::kFinished,
           "turn_completed must finish a turn");
}

void TestPrivateTextCannotImpersonateMetadata() {
    using namespace codex_monitor::codex;
    const CodexActivityLineResult decoy = ParseCodexActivityJsonlLine(
        R"json({"timestamp":"2026-01-01T05:21:00Z","type":"response_item","payload":{"type":"message","role":"assistant","phase":"commentary","content":[{"text":"{\"type\":\"event_msg\",\"payload\":{\"type\":\"turn_started\"}}"}]}})json");
    Expect(decoy.disposition == CodexActivityLineDisposition::kIgnored,
           "metadata-looking prompt or response text must be ignored");
    Expect(ParseCodexActivityJsonlLine("{broken").disposition ==
               CodexActivityLineDisposition::kMalformed,
           "malformed JSON must be ignored without inventing activity");
    const std::string oversizedType(300, 'x');
    const std::string line =
        "{\"timestamp\":\"2026-01-01T05:21:00Z\",\"type\":\"" +
        oversizedType + "\",\"payload\":{\"type\":\"turn_started\"}}";
    Expect(ParseCodexActivityJsonlLine(line).disposition ==
               CodexActivityLineDisposition::kIgnored,
           "oversized retained metadata fields must be bounded and ignored");
}

void TestBoundedFilesystemScanAndHonestDegradation() {
    using namespace codex_monitor::codex;
    TemporaryDirectory temporary;
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    const std::vector<std::filesystem::path> dates =
        RecentLocalCodexSessionDatePaths(now, 2);
    Expect(dates.size() == 2, "two recent date folders must be selected");
    const std::filesystem::path today = temporary.path() / dates.front();
    std::error_code error;
    std::filesystem::create_directories(today, error);
    Expect(!error, "the current activity date folder must be created");

    const std::filesystem::path active = today / "rollout-active.jsonl";
    WriteLines(active, {StartLine(now - 60)});
    const std::filesystem::path completed = today / "rollout-done.jsonl";
    WriteLines(completed, {StartLine(now - 90), FinishLine(now - 30)});
    const std::filesystem::path compressed =
        today / "rollout-migrated.jsonl.zst";
    WriteLines(compressed, {"compressed bytes are deliberately not read"});

    CodexActivityScanRequest request{temporary.path(), now, {}};
    const CodexActivityScanResult partial = ScanRecentCodexActivity(request);
    Expect(partial.available() && partial.partial,
           "a readable set plus recent compressed file must be partial");
    Expect(partial.activeTaskCount == 1 &&
               partial.longestActiveTaskSeconds >= 59 &&
               partial.longestActiveTaskSeconds <= 62,
           "only the unfinished recent file must be counted as active");
    Expect(partial.readableRecentFileCount == 2 &&
               partial.unresolvedRecentFileCount == 1 &&
               partial.skippedCompressedFileCount == 1,
           "the scan must expose readable and unresolved coverage");
    Expect(partial.bytesRead < 16 * 1024,
           "small recent files must not consume the tail budget");

    std::filesystem::remove(active, error);
    std::filesystem::remove(completed, error);
    const CodexActivityScanResult unavailable =
        ScanRecentCodexActivity(request);
    Expect(unavailable.status ==
               CodexActivityScanStatus::kRecentFilesUnresolved &&
               !unavailable.available() &&
               unavailable.activeTaskCount == 0,
           "compressed-only recent state must be unavailable, not a zero");
}

void TestCandidateAndTailLimits() {
    using namespace codex_monitor::codex;
    TemporaryDirectory temporary;
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    const std::filesystem::path today =
        temporary.path() / RecentLocalCodexSessionDatePaths(now, 1).front();
    std::error_code error;
    std::filesystem::create_directories(today, error);
    Expect(!error, "the bounded-scan date folder must be created");
    for (std::size_t index = 0;
         index < kCodexActivityMaximumCandidateFiles + 1;
         ++index) {
        char name[64]{};
        std::snprintf(name, sizeof(name), "rollout-%03zu.jsonl", index);
        WriteLines(today / name, {StartLine(now - 30)});
    }
    CodexActivityScanRequest request{temporary.path(), now, {}};
    CodexActivityScanResult limited = ScanRecentCodexActivity(request);
    Expect(limited.available() && limited.partial &&
               limited.activeTaskCount ==
                   kCodexActivityMaximumCandidateFiles &&
               limited.unresolvedRecentFileCount == 1,
           "only 64 newest candidates may be read and overflow must be partial");

    std::filesystem::remove_all(today, error);
    std::filesystem::create_directories(today, error);
    const std::filesystem::path large = today / "rollout-large.jsonl";
    {
        std::ofstream output(large, std::ios::binary | std::ios::trunc);
        const std::string filler =
            R"json({"timestamp":"2020-01-01T00:00:00Z","type":"ignored","payload":{"content":")json" +
            std::string(4000, 'x') + "\"}}\n";
        while (static_cast<std::uint64_t>(output.tellp()) <=
               kCodexActivityMaximumTailBytes + 64 * 1024ULL) {
            output << filler;
        }
        output << StartLine(now - 20) << '\n';
    }
    limited = ScanRecentCodexActivity(request);
    Expect(limited.available() && limited.activeTaskCount == 1,
           "a start event in the bounded tail must still be detected");
    Expect(limited.bytesRead <= kCodexActivityMaximumTailBytes,
           "a file scan must never read more than its final 1 MiB");
}

void TestMissingAndUnsafeRootsDoNotReportZero() {
    using namespace codex_monitor::codex;
    TemporaryDirectory temporary;
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    CodexActivityScanResult missing = ScanRecentCodexActivity(
        {temporary.path() / "missing", now, {}});
    Expect(missing.status == CodexActivityScanStatus::kRootNotFound &&
               !missing.available(),
           "a missing sessions root must be unavailable");
    CodexActivityScanResult relative =
        ScanRecentCodexActivity({"relative/sessions", now, {}});
    Expect(relative.status == CodexActivityScanStatus::kUnsafeRoot &&
               !relative.available(),
           "a relative sessions root must be rejected");
}

}  // namespace

int main() {
    TestPureInferenceMatchesMacSafetyRules();
    TestPrivateTextCannotImpersonateMetadata();
    TestBoundedFilesystemScanAndHonestDegradation();
    TestCandidateAndTailLimits();
    TestMissingAndUnsafeRootsDoNotReportZero();
    if (failures != 0) return 1;
    std::cout << "codex_activity_scan_test: pass\n";
    return 0;
}
