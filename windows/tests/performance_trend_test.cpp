#include "performance_trend.h"

#include <cmath>
#include <iostream>
#include <optional>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

bool Near(const std::optional<double>& value, double expected) {
    return value && std::fabs(*value - expected) < 0.0001;
}

constexpr std::uint64_t Seconds(std::uint64_t seconds) {
    return seconds *
           codex_monitor::kPerformanceTrendHundredNanosecondsPerSecond;
}

void TestEmptyAndMissingValuesAreNotZero() {
    codex_monitor::PerformanceTrend trend;
    auto summary = trend.Summarize();
    Expect(summary.sampleCount == 0 && !summary.averagePercent &&
               !summary.peakPercent && summary.buckets.empty(),
           "an empty trend must not fabricate summary values");

    trend.Add(Seconds(1), std::nullopt);
    trend.Add(Seconds(6), -1.0);
    trend.Add(Seconds(11), 101.0);
    summary = trend.Summarize();
    Expect(summary.sampleCount == 3 && summary.validSampleCount == 0,
           "missing and out-of-range samples must remain unavailable");
    Expect(!summary.averagePercent && !summary.peakPercent,
           "unavailable CPU samples must not be averaged as zero");
    Expect(summary.buckets.size() == 3 && !summary.buckets[0] &&
               !summary.buckets[1] && !summary.buckets[2],
           "unavailable buckets must stay optional");
}

void TestAveragePeakCoverageAndDuplicateReplacement() {
    codex_monitor::PerformanceTrend trend;
    trend.Add(Seconds(10), 10.0);
    trend.Add(Seconds(15), 30.0);
    trend.Add(Seconds(20), 50.0);
    trend.Add(Seconds(20), 70.0);
    const auto summary = trend.Summarize();
    Expect(summary.sampleCount == 3 && summary.validSampleCount == 3,
           "an equal timestamp must replace rather than duplicate a sample");
    Expect(summary.coverage100ns == Seconds(10),
           "coverage must use unbiased sample timestamps");
    Expect(Near(summary.averagePercent, 110.0 / 3.0) &&
               Near(summary.peakPercent, 70.0),
           "the trend must report valid-sample average and peak");
}

void TestTenMinuteWindowAndEighteenBuckets() {
    codex_monitor::PerformanceTrend trend;
    for (std::uint64_t index = 0; index <= 120; ++index) {
        trend.Add(Seconds(index * 5), static_cast<double>(index % 101));
    }
    auto summary = trend.Summarize();
    Expect(summary.coverage100ns == Seconds(600),
           "the complete ten-minute boundary must be retained");
    Expect(summary.sampleCount == 121 &&
               summary.buckets.size() ==
                   codex_monitor::kPerformanceTrendBucketCount,
           "a full trend must downsample to eighteen buckets");

    trend.Add(Seconds(605), 20.0);
    summary = trend.Summarize();
    Expect(summary.coverage100ns == Seconds(600) &&
               summary.sampleCount == 121,
           "samples older than the rolling ten-minute window must be pruned");
}

void TestBucketsFollowElapsedTimeRatherThanSampleCount() {
    codex_monitor::PerformanceTrend trend;
    for (std::uint64_t second = 0; second <= 45; second += 5) {
        trend.Add(Seconds(second), 20.0);
    }
    for (std::uint64_t second = 70; second <= 120; second += 5) {
        trend.Add(Seconds(second), 40.0);
    }
    const auto summary = trend.Summarize();
    bool hasUnavailableTimeBucket = false;
    for (const std::optional<double>& bucket : summary.buckets) {
        if (!bucket) hasUnavailableTimeBucket = true;
    }
    Expect(summary.sampleCount == 21 &&
               summary.buckets.size() ==
                   codex_monitor::kPerformanceTrendBucketCount &&
               hasUnavailableTimeBucket,
           "irregular samples must preserve elapsed-time gaps in the chart");
}

void TestDiscontinuitiesClearTheSegment() {
    codex_monitor::PerformanceTrend trend;
    trend.Add(Seconds(100), 20.0);
    trend.Add(Seconds(105), 30.0);
    trend.Add(Seconds(136), 40.0);
    auto summary = trend.Summarize();
    Expect(summary.sampleCount == 1 && summary.coverage100ns == 0 &&
               Near(summary.averagePercent, 40.0),
           "a gap over thirty seconds must begin a new segment");

    trend.Add(Seconds(130), 50.0);
    summary = trend.Summarize();
    Expect(summary.sampleCount == 1 && Near(summary.averagePercent, 50.0),
           "a backwards timestamp must begin a new segment");

    trend.Reset();
    Expect(trend.empty() && trend.Summarize().sampleCount == 0,
           "an explicit pause reset must clear the segment");
}

}  // namespace

int main() {
    TestEmptyAndMissingValuesAreNotZero();
    TestAveragePeakCoverageAndDuplicateReplacement();
    TestTenMinuteWindowAndEighteenBuckets();
    TestBucketsFollowElapsedTimeRatherThanSampleCount();
    TestDiscontinuitiesClearTheSegment();
    if (failures != 0) return 1;
    std::cout << "performance_trend_tests=pass\n";
    return 0;
}
