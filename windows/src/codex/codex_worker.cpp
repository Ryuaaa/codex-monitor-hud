#include "codex_worker.h"

#include "codex_executable.h"

#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <utility>

namespace codex_monitor::codex {
namespace {

constexpr std::chrono::seconds kSuccessfulRefreshDelay{300};
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

template <typename T>
void MarkMethodUnavailable(MethodState<T>& state,
                           const wchar_t* message) {
    ApplyMethodResult(
        state,
        MethodParseResult<T>::Failure(MethodFailure{
            MethodFailureKind::kMissingField, L"$transport", message}));
}

void MarkAllMethodsUnavailable(CodexDataState& data,
                               const wchar_t* message) {
    MarkMethodUnavailable(data.rateLimits, message);
    MarkMethodUnavailable(data.account, message);
    MarkMethodUnavailable(data.usage, message);
    MarkMethodUnavailable(data.threadList, message);
}

[[nodiscard]] bool RefreshSucceeded(const AppServerRefreshReport& report,
                                    const CodexDataState& data) noexcept {
    return !report.failure.has_value() && report.allMethodsCompleted() &&
           !data.rateLimits.lastFailure.has_value() &&
           !data.account.lastFailure.has_value() &&
           !data.usage.lastFailure.has_value() &&
           !data.threadList.lastFailure.has_value();
}

struct RefreshResult {
    CodexDataState data;
    AppServerRefreshReport report;
};

RefreshResult FailureResult(const CodexDataState& retainedData,
                            AppServerClientFailureKind failure,
                            const wchar_t* message) {
    RefreshResult result;
    result.data = retainedData;
    result.report.failure = failure;
    MarkAllMethodsUnavailable(result.data, message);
    return result;
}

}  // namespace

CodexWorker::~CodexWorker() {
    StopAndJoin();
}

bool CodexWorker::Start(HWND completionWindow,
                        UINT completionMessage,
                        std::string_view clientVersion) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_ || thread_.joinable() || schedule_.IsStopped() ||
        !completionWindow || completionMessage < WM_APP ||
        clientVersion.empty() || clientVersion.size() > 128) {
        return false;
    }

    completionWindow_ = completionWindow;
    completionMessage_ = completionMessage;
    try {
        clientVersion_.assign(clientVersion);
        thread_ = std::thread(&CodexWorker::Run, this);
    } catch (...) {
        completionWindow_ = nullptr;
        completionMessage_ = 0;
        clientVersion_.clear();
        return false;
    }
    started_ = true;
    return true;
}

bool CodexWorker::ActivateAndRefresh() {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) return false;
        changed = schedule_.IsActive() ? schedule_.Request()
                                       : schedule_.Activate();
        if (changed) nextAutomaticRefresh_.reset();
    }
    if (changed) wake_.notify_one();
    return changed;
}

bool CodexWorker::RequestRefresh() {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) return false;
        changed = schedule_.Request();
        if (changed) nextAutomaticRefresh_.reset();
    }
    if (changed) wake_.notify_one();
    return changed;
}

void CodexWorker::PauseAndInvalidate() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) return;
        schedule_.PauseAndInvalidate();
        nextAutomaticRefresh_.reset();
        latest_.reset();
        // Resume never resets this epoch. A pre-pause callback therefore stays
        // cancelled even after a new generation becomes active.
        cancellationEpoch_.fetch_add(1, std::memory_order_acq_rel);
    }
    wake_.notify_one();
}

std::optional<CompletedCodexRefresh> CodexWorker::TakeLatest() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::optional<CompletedCodexRefresh> result = std::move(latest_);
    latest_.reset();
    return result;
}

bool CodexWorker::IsBusy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return started_ && schedule_.IsBusy();
}

void CodexWorker::StopAndJoin() {
    std::thread threadToJoin;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ && !thread_.joinable()) {
            latest_.reset();
            return;
        }
        schedule_.Stop();
        nextAutomaticRefresh_.reset();
        latest_.reset();
        cancellationEpoch_.fetch_add(1, std::memory_order_acq_rel);
        completionWindow_ = nullptr;
        completionMessage_ = 0;
        clientVersion_.clear();
        started_ = false;
        if (thread_.joinable()) threadToJoin = std::move(thread_);
    }
    wake_.notify_one();
    if (threadToJoin.joinable()) threadToJoin.join();
}

void CodexWorker::Run() {
    bool apartmentInitialized = false;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartmentInitialized = true;
    } catch (...) {
        // Work items below become ordinary failure snapshots. The UI thread
        // remains alive and the worker retains its normal retry cadence.
    }

    // The client and its retained parsed state never leave this thread.
    CodexAppServerClient client;
    std::size_t consecutiveFailureCount = 0;
    for (;;) {
        std::optional<::codex_monitor::CodexRefreshWorkItem> item;
        std::uint64_t refreshEpoch = 0;
        std::string clientVersion;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            for (;;) {
                if (schedule_.IsStopped()) {
                    if (apartmentInitialized) winrt::uninit_apartment();
                    return;
                }

                const auto now = std::chrono::steady_clock::now();
                if (nextAutomaticRefresh_ && now >= *nextAutomaticRefresh_) {
                    nextAutomaticRefresh_.reset();
                    schedule_.Request();
                }

                item = schedule_.TakeNext();
                if (item) {
                    refreshEpoch =
                        cancellationEpoch_.load(std::memory_order_acquire);
                    clientVersion = clientVersion_;
                    break;
                }

                if (nextAutomaticRefresh_) {
                    wake_.wait_until(lock, *nextAutomaticRefresh_);
                } else {
                    wake_.wait(lock);
                }
            }
        }

        RefreshResult refresh;
        if (!apartmentInitialized) {
            refresh = FailureResult(
                client.data(), AppServerClientFailureKind::kTransportFailed,
                L"Windows runtime initialization failed");
        } else {
            try {
                const std::optional<std::filesystem::path> executable =
                    FindCodexExecutable();
                if (!executable) {
                    refresh = FailureResult(
                        client.data(), AppServerClientFailureKind::kStartFailed,
                        L"Codex executable was unavailable");
                } else {
                    refresh.report = client.Refresh(
                        *executable, clientVersion,
                        [this, refreshEpoch] {
                            return cancellationEpoch_.load(
                                       std::memory_order_acquire) != refreshEpoch;
                        });
                    refresh.data = client.data();
                }
            } catch (...) {
                refresh = FailureResult(
                    client.data(), AppServerClientFailureKind::kTransportFailed,
                    L"Codex refresh failed unexpectedly");
            }
        }

        const bool succeeded = RefreshSucceeded(refresh.report, refresh.data);
        HWND notifyWindow = nullptr;
        UINT notifyMessage = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const bool acceptResult = schedule_.Finish(
                *item, succeeded ? ::codex_monitor::CodexRefreshOutcome::kSuccess
                                 : ::codex_monitor::CodexRefreshOutcome::kFailure);
            if (acceptResult) {
                if (succeeded) {
                    consecutiveFailureCount = 0;
                } else if (consecutiveFailureCount < kFailedRefreshDelays.size()) {
                    ++consecutiveFailureCount;
                }
                const auto delay = succeeded
                    ? kSuccessfulRefreshDelay
                    : FailureRefreshDelay(consecutiveFailureCount);
                latest_ = CompletedCodexRefresh{
                    std::move(refresh.data), std::move(refresh.report),
                    succeeded, delay};
                if (schedule_.RecommendedDelay()) {
                    nextAutomaticRefresh_ =
                        std::chrono::steady_clock::now() + delay;
                } else {
                    // A merged explicit request is already pending and runs
                    // immediately instead of waiting for the normal interval.
                    nextAutomaticRefresh_.reset();
                }
                notifyWindow = completionWindow_;
                notifyMessage = completionMessage_;
            }
        }
        if (notifyWindow && notifyMessage != 0) {
            PostMessageW(notifyWindow, notifyMessage, 0, 0);
        }
    }
}

}  // namespace codex_monitor::codex
