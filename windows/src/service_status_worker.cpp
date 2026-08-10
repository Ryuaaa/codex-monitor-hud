#include "service_status_worker.h"

#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <utility>

namespace codex_monitor {
namespace {

constexpr std::chrono::seconds kSuccessfulRefreshDelay{900};
constexpr std::array<std::chrono::seconds, 5> kFailedRefreshDelays{
    std::chrono::seconds{60},
    std::chrono::seconds{120},
    std::chrono::seconds{300},
    std::chrono::seconds{600},
    std::chrono::seconds{900},
};

std::chrono::seconds FailureRefreshDelay(
    std::size_t consecutiveFailureCount) noexcept {
    const std::size_t index = std::min(
        consecutiveFailureCount == 0 ? std::size_t{0}
                                     : consecutiveFailureCount - 1,
        kFailedRefreshDelays.size() - 1);
    return kFailedRefreshDelays[index];
}

struct ApartmentCleanup {
    bool initialized = false;
    ~ApartmentCleanup() {
        if (initialized) winrt::uninit_apartment();
    }
};

OpenAIServiceStatusFetchResult RuntimeFailure() {
    OpenAIServiceStatusFetchResult result;
    result.failure = OpenAIServiceStatusFailureKind::kInvalidResponse;
    result.error = L"Windows runtime initialization failed";
    return result;
}

}  // namespace

OpenAIServiceStatusWorker::OpenAIServiceStatusWorker(
    OpenAIServiceStatusFetcher fetcher)
    : fetcher_(std::move(fetcher)) {
    if (!fetcher_) fetcher_ = FetchOpenAIServiceStatus;
}

OpenAIServiceStatusWorker::~OpenAIServiceStatusWorker() {
    StopAndJoin();
}

bool OpenAIServiceStatusWorker::Start(HWND completionWindow,
                                      UINT completionMessage) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_ || thread_.joinable() || stopRequested_ ||
        !completionWindow || completionMessage < WM_APP || !fetcher_) {
        return false;
    }
    completionWindow_ = completionWindow;
    completionMessage_ = completionMessage;
    try {
        thread_ = std::thread(&OpenAIServiceStatusWorker::Run, this);
    } catch (...) {
        completionWindow_ = nullptr;
        completionMessage_ = 0;
        return false;
    }
    started_ = true;
    return true;
}

bool OpenAIServiceStatusWorker::ActivateAndRefresh() {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ || stopRequested_) return false;
        if (!active_) {
            active_ = true;
            changed = true;
            const auto now = std::chrono::steady_clock::now();
            if (resumeNotBefore_ && now < *resumeNotBefore_) {
                nextAutomaticRefresh_ = resumeNotBefore_;
            } else {
                resumeNotBefore_.reset();
                pending_ = true;
            }
        } else if (!pending_) {
            // A caller requesting again while already active is an explicit
            // refresh and may bypass the normal cadence.
            pending_ = true;
            nextAutomaticRefresh_.reset();
            resumeNotBefore_.reset();
            changed = true;
        }
    }
    if (changed) wake_.notify_one();
    return changed;
}

bool OpenAIServiceStatusWorker::RequestRefresh() {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ || stopRequested_ || !active_ || pending_) return false;
        pending_ = true;
        nextAutomaticRefresh_.reset();
        resumeNotBefore_.reset();
        changed = true;
    }
    wake_.notify_one();
    return changed;
}

void OpenAIServiceStatusWorker::PauseAndInvalidate() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ || stopRequested_) return;
        active_ = false;
        pending_ = false;
        nextAutomaticRefresh_.reset();
        latest_.reset();
        cancellationEpoch_.fetch_add(1, std::memory_order_acq_rel);
    }
    wake_.notify_one();
}

std::optional<CompletedServiceStatusRefresh>
OpenAIServiceStatusWorker::TakeLatest() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::optional<CompletedServiceStatusRefresh> result = std::move(latest_);
    latest_.reset();
    return result;
}

bool OpenAIServiceStatusWorker::IsBusy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return started_ && (pending_ || busy_);
}

void OpenAIServiceStatusWorker::StopAndJoin() {
    std::thread threadToJoin;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ && !thread_.joinable()) {
            latest_.reset();
            return;
        }
        stopRequested_ = true;
        active_ = false;
        pending_ = false;
        nextAutomaticRefresh_.reset();
        resumeNotBefore_.reset();
        latest_.reset();
        cancellationEpoch_.fetch_add(1, std::memory_order_acq_rel);
        completionWindow_ = nullptr;
        completionMessage_ = 0;
        started_ = false;
        if (thread_.joinable()) threadToJoin = std::move(thread_);
    }
    wake_.notify_one();
    if (threadToJoin.joinable()) threadToJoin.join();
}

void OpenAIServiceStatusWorker::Run() {
    bool apartmentInitialized = false;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartmentInitialized = true;
    } catch (...) {
        // The worker publishes an ordinary failure and retries at the normal
        // failure cadence instead of affecting the UI thread.
    }
    ApartmentCleanup apartmentCleanup{apartmentInitialized};

    std::size_t consecutiveFailureCount = 0;
    for (;;) {
        std::uint64_t refreshEpoch = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            for (;;) {
                if (stopRequested_) return;
                const auto now = std::chrono::steady_clock::now();
                if (active_ && nextAutomaticRefresh_ &&
                    now >= *nextAutomaticRefresh_) {
                    nextAutomaticRefresh_.reset();
                    resumeNotBefore_.reset();
                    pending_ = true;
                }
                if (active_ && pending_ && !busy_) {
                    pending_ = false;
                    busy_ = true;
                    refreshEpoch =
                        cancellationEpoch_.load(std::memory_order_acquire);
                    break;
                }
                if (active_ && nextAutomaticRefresh_) {
                    wake_.wait_until(lock, *nextAutomaticRefresh_);
                } else {
                    wake_.wait(lock);
                }
            }
        }

        OpenAIServiceStatusFetchResult fetched;
        if (!apartmentInitialized) {
            fetched = RuntimeFailure();
        } else {
            try {
                fetched = fetcher_([this, refreshEpoch] {
                    return cancellationEpoch_.load(
                               std::memory_order_acquire) != refreshEpoch;
                });
            } catch (...) {
                fetched.failure = OpenAIServiceStatusFailureKind::kInvalidResponse;
                fetched.error = L"Service status refresh failed unexpectedly";
            }
        }

        HWND notifyWindow = nullptr;
        UINT notifyMessage = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            busy_ = false;
            if (stopRequested_) return;
            const bool acceptResult = active_ &&
                cancellationEpoch_.load(std::memory_order_acquire) ==
                    refreshEpoch;
            if (!acceptResult) {
                wake_.notify_one();
                continue;
            }

            const bool succeeded = fetched.succeeded;
            if (succeeded) {
                consecutiveFailureCount = 0;
                retainedStatus_ = std::move(fetched.status);
                lastSuccessfulRefresh_ = std::chrono::system_clock::now();
            } else if (consecutiveFailureCount < kFailedRefreshDelays.size()) {
                ++consecutiveFailureCount;
            }
            const auto delay = succeeded
                ? kSuccessfulRefreshDelay
                : FailureRefreshDelay(consecutiveFailureCount);
            latest_ = CompletedServiceStatusRefresh{
                retainedStatus_, lastSuccessfulRefresh_, succeeded,
                !succeeded && retainedStatus_.has_value(), delay};
            if (pending_) {
                nextAutomaticRefresh_.reset();
            } else {
                const auto next = std::chrono::steady_clock::now() + delay;
                nextAutomaticRefresh_ = next;
                resumeNotBefore_ = next;
            }
            notifyWindow = completionWindow_;
            notifyMessage = completionMessage_;
        }
        if (notifyWindow && notifyMessage != 0) {
            PostMessageW(notifyWindow, notifyMessage, 0, 0);
        }
        wake_.notify_one();
    }
}

}  // namespace codex_monitor
