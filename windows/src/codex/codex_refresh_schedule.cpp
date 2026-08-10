#include "codex_refresh_schedule.h"

namespace codex_monitor {
namespace {

constexpr std::chrono::seconds kSuccessDelay{300};
constexpr std::chrono::seconds kFailureDelay{60};

}  // namespace

bool CodexRefreshSchedule::Activate() {
    if (stopped_ || active_) return false;
    active_ = true;
    pending_ = true;
    recommendedDelay_.reset();
    return true;
}

bool CodexRefreshSchedule::Request() {
    if (stopped_ || !active_ || pending_) return false;
    pending_ = true;
    recommendedDelay_.reset();
    return true;
}

void CodexRefreshSchedule::PauseAndInvalidate() {
    if (stopped_) return;
    ++generation_;
    active_ = false;
    pending_ = false;
    recommendedDelay_.reset();
}

void CodexRefreshSchedule::Stop() {
    if (stopped_) return;
    ++generation_;
    stopped_ = true;
    active_ = false;
    pending_ = false;
    recommendedDelay_.reset();
}

std::optional<CodexRefreshWorkItem> CodexRefreshSchedule::TakeNext() {
    if (stopped_ || !active_ || running_ || !pending_) return std::nullopt;
    running_ = CodexRefreshWorkItem{generation_, nextSequence_++};
    pending_ = false;
    return running_;
}

bool CodexRefreshSchedule::Finish(const CodexRefreshWorkItem& item,
                                  CodexRefreshOutcome outcome) {
    if (!running_ || running_->generation != item.generation ||
        running_->sequence != item.sequence) {
        return false;
    }

    running_.reset();
    if (stopped_ || !active_ || item.generation != generation_) return false;

    if (pending_) {
        recommendedDelay_.reset();
    } else {
        recommendedDelay_ = outcome == CodexRefreshOutcome::kSuccess
                                ? kSuccessDelay
                                : kFailureDelay;
    }
    return true;
}

bool CodexRefreshSchedule::IsActive() const {
    return active_ && !stopped_;
}

bool CodexRefreshSchedule::IsBusy() const {
    return running_.has_value() || pending_;
}

bool CodexRefreshSchedule::HasPending() const {
    return pending_;
}

bool CodexRefreshSchedule::IsStopped() const {
    return stopped_;
}

std::optional<std::chrono::seconds> CodexRefreshSchedule::RecommendedDelay() const {
    return recommendedDelay_;
}

}  // namespace codex_monitor
