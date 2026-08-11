#include "performance_trend.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace codex_monitor {
namespace {

std::optional<double> ValidCpuPercent(std::optional<double> value) noexcept {
    if (!value || !std::isfinite(*value) || *value < 0.0 || *value > 100.0) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

void PerformanceTrend::Add(
    std::uint64_t capturedAtUnbiased100ns,
    std::optional<double> systemCpuPercent) {
    systemCpuPercent = ValidCpuPercent(systemCpuPercent);
    if (!points_.empty()) {
        const std::uint64_t previous = points_.back().capturedAtUnbiased100ns;
        if (capturedAtUnbiased100ns < previous ||
            capturedAtUnbiased100ns - previous >
                kPerformanceTrendMaximumGap100ns) {
            points_.clear();
        } else if (capturedAtUnbiased100ns == previous) {
            points_.back().systemCpuPercent = systemCpuPercent;
            return;
        }
    }

    points_.push_back(
        PerformanceTrendPoint{capturedAtUnbiased100ns, systemCpuPercent});
    const std::uint64_t cutoff =
        capturedAtUnbiased100ns > kPerformanceTrendWindow100ns
            ? capturedAtUnbiased100ns - kPerformanceTrendWindow100ns
            : 0;
    const auto firstRetained = std::lower_bound(
        points_.begin(), points_.end(), cutoff,
        [](const PerformanceTrendPoint& point, std::uint64_t threshold) {
            return point.capturedAtUnbiased100ns < threshold;
        });
    if (firstRetained != points_.begin()) {
        points_.erase(points_.begin(), firstRetained);
    }
}

void PerformanceTrend::Reset() noexcept {
    points_.clear();
}

PerformanceTrendSummary PerformanceTrend::Summarize() const {
    PerformanceTrendSummary summary;
    summary.sampleCount = points_.size();
    if (points_.empty()) return summary;

    summary.coverage100ns = points_.back().capturedAtUnbiased100ns -
                           points_.front().capturedAtUnbiased100ns;
    double sum = 0.0;
    double peak = 0.0;
    for (const PerformanceTrendPoint& point : points_) {
        if (!point.systemCpuPercent) continue;
        sum += *point.systemCpuPercent;
        peak = std::max(peak, *point.systemCpuPercent);
        ++summary.validSampleCount;
    }
    if (summary.validSampleCount > 0) {
        summary.averagePercent =
            sum / static_cast<double>(summary.validSampleCount);
        summary.peakPercent = peak;
    }

    if (points_.size() <= kPerformanceTrendBucketCount) {
        summary.buckets.reserve(points_.size());
        for (const PerformanceTrendPoint& point : points_) {
            summary.buckets.push_back(point.systemCpuPercent);
        }
        return summary;
    }

    std::array<double, kPerformanceTrendBucketCount> bucketSums{};
    std::array<std::size_t, kPerformanceTrendBucketCount> bucketCounts{};
    const std::uint64_t firstTimestamp =
        points_.front().capturedAtUnbiased100ns;
    const std::uint64_t bucketDenominator = summary.coverage100ns + 1;
    for (const PerformanceTrendPoint& point : points_) {
        const std::uint64_t elapsed =
            point.capturedAtUnbiased100ns - firstTimestamp;
        const std::size_t bucket = std::min<std::size_t>(
            kPerformanceTrendBucketCount - 1,
            static_cast<std::size_t>(
                elapsed * kPerformanceTrendBucketCount / bucketDenominator));
        if (!point.systemCpuPercent) continue;
        bucketSums[bucket] += *point.systemCpuPercent;
        ++bucketCounts[bucket];
    }

    summary.buckets.reserve(kPerformanceTrendBucketCount);
    for (std::size_t bucket = 0; bucket < kPerformanceTrendBucketCount;
         ++bucket) {
        summary.buckets.push_back(
            bucketCounts[bucket] > 0
                ? std::optional<double>(
                      bucketSums[bucket] /
                      static_cast<double>(bucketCounts[bucket]))
                : std::nullopt);
    }
    return summary;
}

}  // namespace codex_monitor
