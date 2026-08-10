#pragma once

#include <windows.h>

#include "performance_snapshot.h"
#include "sampling_schedule.h"
#include "windows_sampler.h"

#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace codex_monitor {

struct CompletedSample {
    SampleMode mode = SampleMode::kFast;
    PerformanceSnapshot snapshot;
};

class PerformanceWorker {
public:
    PerformanceWorker() = default;
    ~PerformanceWorker();

    PerformanceWorker(const PerformanceWorker&) = delete;
    PerformanceWorker& operator=(const PerformanceWorker&) = delete;

    bool Start(HWND completionWindow, UINT completionMessage);
    bool ActivateAndRequestFullSample();
    bool Request(SampleMode mode);
    void PauseAndInvalidate();
    bool IsBusy() const;
    std::optional<CompletedSample> TakeLatest();
    void StopAndJoin();

private:
    void Run();

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    SamplingSchedule schedule_;
    std::optional<CompletedSample> latest_;
    HWND completionWindow_ = nullptr;
    UINT completionMessage_ = 0;
    bool started_ = false;
    std::thread thread_;
    WindowsSampler sampler_;
};

}  // namespace codex_monitor
