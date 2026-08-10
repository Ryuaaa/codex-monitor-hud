#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace codex_monitor {

enum class CodexRefreshOutcome {
    kSuccess,
    kFailure,
};

struct CodexRefreshWorkItem {
    std::uint64_t generation = 0;
    std::uint64_t sequence = 0;
};

class CodexRefreshSchedule {
public:
    // Activating a previously paused schedule immediately queues one refresh.
    bool Activate();
    bool Request();
    void PauseAndInvalidate();
    void Stop();

    std::optional<CodexRefreshWorkItem> TakeNext();
    bool Finish(const CodexRefreshWorkItem& item, CodexRefreshOutcome outcome);

    bool IsActive() const;
    bool IsBusy() const;
    bool HasPending() const;
    bool IsStopped() const;
    std::optional<std::chrono::seconds> RecommendedDelay() const;

private:
    bool active_ = false;
    bool stopped_ = false;
    bool pending_ = false;
    std::uint64_t generation_ = 1;
    std::uint64_t nextSequence_ = 1;
    std::optional<CodexRefreshWorkItem> running_;
    std::optional<std::chrono::seconds> recommendedDelay_;
};

}  // namespace codex_monitor
