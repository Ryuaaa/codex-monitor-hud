#include "codex/quota_history_store.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

namespace {

using codex_monitor::codex::QuotaHistoryLoadStatus;
using codex_monitor::codex::QuotaHistorySample;
using codex_monitor::codex::QuotaHistoryStore;
using codex_monitor::codex::QuotaHistoryUpdateStatus;

void Require(bool condition, std::string_view message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto sequence = std::chrono::high_resolution_clock::now()
                                  .time_since_epoch()
                                  .count();
        path_ = std::filesystem::temp_directory_path() /
                ("codex-monitor-quota-history-" + std::to_string(sequence));
        std::error_code error;
        Require(std::filesystem::create_directories(path_, error) && !error,
                "temporary directory must be created");
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    Require(static_cast<bool>(input), "test file must be readable");
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

void WriteFile(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    Require(static_cast<bool>(output), "test file must be writable");
    output << contents;
    output.flush();
    Require(static_cast<bool>(output), "test file write must finish");
}

std::string SampleLine(std::int64_t capturedAt,
                       std::string_view fiveRemaining = "-",
                       std::string_view fiveReset = "-",
                       std::string_view weeklyRemaining = "50",
                       std::string_view weeklyReset = "-") {
    std::ostringstream output;
    output << "sample\tcaptured_at=" << capturedAt
           << "\tfive_hour_remaining=" << fiveRemaining
           << "\tfive_hour_reset_at=" << fiveReset
           << "\tweekly_remaining=" << weeklyRemaining
           << "\tweekly_reset_at=" << weeklyReset << '\n';
    return output.str();
}

QuotaHistorySample WeeklySample(std::int64_t capturedAt, double remaining) {
    QuotaHistorySample sample;
    sample.capturedAtUnixSeconds = capturedAt;
    sample.weekly.remainingPercent = remaining;
    return sample;
}

void TestRoundTripAndWhitelistedFormat() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "quota-history.txt";
    const std::int64_t now = 2'000'000'000;

    QuotaHistorySample sample;
    sample.capturedAtUnixSeconds = now;
    sample.fiveHour.remainingPercent = 87.25;
    sample.fiveHour.resetsAtUnixSeconds = now + 3600;
    sample.weekly.remainingPercent = 42.5;
    sample.weekly.resetsAtUnixSeconds = now + 86400;

    QuotaHistoryStore store(path);
    const auto update = store.Update(sample);
    Require(update.status == QuotaHistoryUpdateStatus::kWritten &&
                update.storedSampleCount == 1,
            "first valid sample must be written");

    const auto loaded = store.Load(now);
    Require(loaded.status == QuotaHistoryLoadStatus::kOk &&
                loaded.samples.size() == 1,
            "written sample must load");
    const auto& roundTrip = loaded.samples.front();
    Require(roundTrip.capturedAtUnixSeconds == now,
            "capture time must round-trip");
    Require(roundTrip.fiveHour.remainingPercent == 87.25 &&
                roundTrip.fiveHour.resetsAtUnixSeconds == now + 3600 &&
                roundTrip.weekly.remainingPercent == 42.5 &&
                roundTrip.weekly.resetsAtUnixSeconds == now + 86400,
            "optional quota fields must round-trip");

    const std::string contents = ReadFile(path);
    Require(contents.rfind("version=1\n", 0) == 0,
            "history must use the version 1 header");
    Require(contents.find("account") == std::string::npos &&
                contents.find("task") == std::string::npos &&
                contents.find("token") == std::string::npos,
            "history must not contain account, task, or token data");
}

void TestMalformedLinesAreSkipped() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "quota-history.txt";
    const std::int64_t now = 2'000'000'000;
    std::string contents = "version=1\r\n";
    contents += SampleLine(now - 10, "25.5", std::to_string(now + 100),
                           "75", std::to_string(now + 200));
    contents += "not-a-sample\n";
    contents += SampleLine(now - 9, "nan");
    contents += SampleLine(now - 8, "inf");
    contents += SampleLine(now - 7, "100.001");
    contents += SampleLine(now - 6, "-0.001");
    contents += SampleLine(-1);
    contents += SampleLine(253402300800LL);
    contents += SampleLine(now - 5, "50", "bad-reset");
    contents += "sample\tcaptured_at=" + std::to_string(now - 4) +
                "\tfive_hour_remaining=-\tfive_hour_reset_at=-"
                "\tweekly_remaining=50\tunknown=-\n";
    std::string extraField = SampleLine(now - 3);
    extraField.pop_back();
    contents += extraField + "\textra-field\n";
    contents += SampleLine(now - 2, "-", "-", "-", "-");
    contents += SampleLine(now - 1, "-", "-", "33.125",
                           std::to_string(now + 300));
    WriteFile(path, contents);

    const auto loaded = QuotaHistoryStore(path).Load(now);
    Require(loaded.status == QuotaHistoryLoadStatus::kOk,
            "a versioned file with damage must still load");
    Require(loaded.samples.size() == 2,
            "only valid whitelist lines must survive parsing");
    Require(loaded.samples.front().capturedAtUnixSeconds == now - 10 &&
                loaded.samples.back().capturedAtUnixSeconds == now - 1,
            "valid lines around damage must be retained");
    Require(loaded.skippedMalformedLines == 11,
            "every malformed line must be counted and skipped");
}

void TestThirtyDayAndTenThousandPointTrimming() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "quota-history.txt";
    const std::int64_t now = 2'000'000'000;
    std::ostringstream contents;
    contents << "version=1\n";
    contents << SampleLine(now - 30LL * 24LL * 60LL * 60LL - 1);
    for (std::int64_t offset = 10004; offset >= 0; --offset) {
        contents << SampleLine(now - offset);
    }
    WriteFile(path, contents.str());

    const auto loaded = QuotaHistoryStore(path).Load(now);
    Require(loaded.status == QuotaHistoryLoadStatus::kOk,
            "large valid history must load");
    Require(loaded.samples.size() == 10000,
            "history must retain at most ten thousand samples");
    Require(loaded.samples.front().capturedAtUnixSeconds == now - 9999 &&
                loaded.samples.back().capturedAtUnixSeconds == now,
            "history must retain the newest samples in chronological order");
    Require(loaded.discardedSamples == 6,
            "one expired and five excess samples must be discarded");
}

void TestRetentionBoundaryAndFutureSample() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "quota-history.txt";
    const std::int64_t now = 2'000'000'000;
    const std::int64_t retention = 30LL * 24LL * 60LL * 60LL;
    std::ostringstream contents;
    contents << "version=1\n"
             << SampleLine(now - retention - 1)
             << SampleLine(now - retention)
             << SampleLine(now)
             << SampleLine(now + 1);
    WriteFile(path, contents.str());

    const auto loaded = QuotaHistoryStore(path).Load(now);
    Require(loaded.samples.size() == 2 &&
                loaded.samples.front().capturedAtUnixSeconds == now - retention &&
                loaded.samples.back().capturedAtUnixSeconds == now,
            "the thirty-day boundary must be inclusive and future data excluded");
    Require(loaded.discardedSamples == 2,
            "expired and future samples must be reported as discarded");
}

void TestMinimumAppendInterval() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "quota-history.txt";
    const std::int64_t start = 2'000'000'000;
    QuotaHistoryStore store(path);

    Require(store.Update(WeeklySample(start, 90)).written(),
            "initial interval sample must write");
    const auto tooSoon = store.Update(WeeklySample(start + 59, 80));
    Require(tooSoon.status == QuotaHistoryUpdateStatus::kSkippedTooSoon,
            "a sample within sixty seconds must be skipped");
    Require(store.Update(WeeklySample(start + 60, 70)).written(),
            "a sample at sixty seconds must write");
    const auto outOfOrder = store.Update(WeeklySample(start + 1, 60));
    Require(outOfOrder.status == QuotaHistoryUpdateStatus::kInvalidSample,
            "an out-of-order sample must not rewrite history");

    const auto loaded = store.Load(start + 60);
    Require(loaded.samples.size() == 2 &&
                loaded.samples.front().weekly.remainingPercent == 90 &&
                loaded.samples.back().weekly.remainingPercent == 70,
            "skipped and out-of-order samples must not be persisted");
}

void TestAtomicReplacementFailurePreservesOldFile() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "quota-history.txt";
    const std::int64_t start = 2'000'000'000;
    Require(QuotaHistoryStore(path).Update(WeeklySample(start, 90)).written(),
            "atomic failure fixture must write");
    const std::string original = ReadFile(path);

    bool callbackRan = false;
    std::filesystem::path observedTemporary;
    QuotaHistoryStore failingStore(
        path,
        [&](const std::filesystem::path& temporaryPath,
            const std::filesystem::path& destinationPath) {
            callbackRan = true;
            observedTemporary = temporaryPath;
            Require(temporaryPath.parent_path() == destinationPath.parent_path(),
                    "replacement temporary file must share the destination directory");
            Require(std::filesystem::exists(temporaryPath),
                    "temporary file must be complete before replacement");
            Require(ReadFile(destinationPath) == original,
                    "old file must remain present before replacement");
            return std::make_error_code(std::errc::permission_denied);
        });
    const auto failed = failingStore.Update(WeeklySample(start + 60, 80));
    Require(callbackRan && failed.status == QuotaHistoryUpdateStatus::kIoError,
            "replacement failure must be reported");
    Require(ReadFile(path) == original,
            "replacement failure must preserve the old file byte-for-byte");
    Require(!observedTemporary.empty() &&
                !std::filesystem::exists(observedTemporary),
            "failed replacement temporary file must be cleaned up");
}

void TestInvalidInputAndUnknownVersionAreNonDestructive() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "quota-history.txt";
    WriteFile(path, "version=2\nfuture-format-data\n");
    const std::string original = ReadFile(path);

    const auto unsupported =
        QuotaHistoryStore(path).Update(WeeklySample(2'000'000'000, 50));
    Require(unsupported.status == QuotaHistoryUpdateStatus::kUnsupportedVersion &&
                ReadFile(path) == original,
            "unknown versions must never be overwritten");

    const auto otherPath = temporary.path() / "invalid-history.txt";
    QuotaHistorySample invalid = WeeklySample(2'000'000'000, 50);
    invalid.weekly.remainingPercent =
        std::numeric_limits<double>::quiet_NaN();
    const auto invalidResult = QuotaHistoryStore(otherPath).Update(invalid);
    Require(invalidResult.status == QuotaHistoryUpdateStatus::kInvalidSample &&
                !std::filesystem::exists(otherPath),
            "invalid in-memory samples must not create a file");
}

}  // namespace

int main() {
    TestRoundTripAndWhitelistedFormat();
    TestMalformedLinesAreSkipped();
    TestThirtyDayAndTenThousandPointTrimming();
    TestRetentionBoundaryAndFutureSample();
    TestMinimumAppendInterval();
    TestAtomicReplacementFailurePreservesOldFile();
    TestInvalidInputAndUnknownVersionAreNonDestructive();
    std::cout << "quota_history_store_tests=pass\n";
    return 0;
}
