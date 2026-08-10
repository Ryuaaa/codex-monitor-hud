#pragma once

#include "windows_sampler.h"

#include <cstdint>
#include <optional>

namespace codex_monitor {

struct SamplingWorkItem {
    SampleMode mode = SampleMode::kFast;
    std::uint64_t generation = 0;
};

// Portable state machine used under the sampling worker's mutex.
class SamplingSchedule {
public:
    bool ActivateAndRequestFullSample();
    bool Request(SampleMode mode);
    void PauseAndInvalidate();
    void Stop();

    bool HasAction() const;
    bool IsBusy() const;
    bool IsStopped() const;
    bool TakeBaselineReset();
    std::optional<SamplingWorkItem> TakeNext();
    bool Finish(const SamplingWorkItem& item);

private:
    bool MergePending(SampleMode mode);

    bool accepting_ = false;
    bool stopped_ = false;
    bool baselineResetPending_ = false;
    std::uint64_t generation_ = 1;
    std::optional<SamplingWorkItem> running_;
    std::optional<SampleMode> pendingMode_;
};

}  // namespace codex_monitor
