#include "sampling_schedule.h"

namespace codex_monitor {
namespace {

bool Covers(SampleMode available, SampleMode requested) {
    return available == SampleMode::kFastAndSlow || available == requested;
}

}  // namespace

bool SamplingSchedule::ActivateAndRequestFullSample() {
    if (stopped_) return false;

    bool changed = false;
    if (!accepting_) {
        accepting_ = true;
        baselineResetPending_ = true;
        changed = true;
    }
    return MergePending(SampleMode::kFastAndSlow) || changed;
}

bool SamplingSchedule::Request(SampleMode mode) {
    if (stopped_ || !accepting_) return false;
    return MergePending(mode);
}

void SamplingSchedule::PauseAndInvalidate() {
    if (stopped_) return;
    ++generation_;
    accepting_ = false;
    pendingMode_.reset();
    baselineResetPending_ = true;
}

void SamplingSchedule::Stop() {
    if (stopped_) return;
    ++generation_;
    stopped_ = true;
    accepting_ = false;
    baselineResetPending_ = false;
    pendingMode_.reset();
}

bool SamplingSchedule::HasAction() const {
    return stopped_ || baselineResetPending_ || (!running_ && pendingMode_.has_value());
}

bool SamplingSchedule::IsBusy() const {
    return baselineResetPending_ || running_.has_value() || pendingMode_.has_value();
}

bool SamplingSchedule::IsStopped() const {
    return stopped_;
}

bool SamplingSchedule::TakeBaselineReset() {
    if (!baselineResetPending_) return false;
    baselineResetPending_ = false;
    return true;
}

std::optional<SamplingWorkItem> SamplingSchedule::TakeNext() {
    if (stopped_ || !accepting_ || baselineResetPending_ || running_ || !pendingMode_) {
        return std::nullopt;
    }

    running_ = SamplingWorkItem{*pendingMode_, generation_};
    pendingMode_.reset();
    return running_;
}

bool SamplingSchedule::Finish(const SamplingWorkItem& item) {
    const bool matchesRunning =
        running_ && running_->generation == item.generation && running_->mode == item.mode;
    running_.reset();
    return matchesRunning && !stopped_ && accepting_ && item.generation == generation_;
}

bool SamplingSchedule::MergePending(SampleMode mode) {
    if (running_ && running_->generation == generation_ &&
        Covers(running_->mode, mode)) {
        return false;
    }

    if (!pendingMode_) {
        pendingMode_ = mode;
        return true;
    }
    if (*pendingMode_ == SampleMode::kFast && mode == SampleMode::kFastAndSlow) {
        pendingMode_ = SampleMode::kFastAndSlow;
        return true;
    }
    return false;
}

}  // namespace codex_monitor
