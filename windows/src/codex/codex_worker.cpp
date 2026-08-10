#include "codex_worker.h"

#include "codex_executable.h"
#include "codex_cost_file_scan.h"
#include "codex_cost_history_state.h"
#include "quota_history_store.h"

#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <functional>
#include <limits>
#include <system_error>
#include <utility>
#include <vector>

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

const RateLimitWindow* SelectQuotaWindow(const RateLimitsData& limits,
                                         bool weekly) noexcept {
    const RateLimitWindow* primary = limits.primary ? &*limits.primary : nullptr;
    const RateLimitWindow* secondary =
        limits.secondary ? &*limits.secondary : nullptr;
    for (const RateLimitWindow* candidate : {primary, secondary}) {
        if (!candidate || !candidate->windowDurationMinutes) continue;
        const bool candidateIsWeekly = *candidate->windowDurationMinutes > 1440;
        if (candidateIsWeekly == weekly) return candidate;
    }
    const RateLimitWindow* fallback = weekly ? secondary : primary;
    return fallback && !fallback->windowDurationMinutes ? fallback : nullptr;
}

QuotaHistoryWindowSample MakeHistoryWindow(
    const RateLimitWindow* window) noexcept {
    QuotaHistoryWindowSample result;
    if (!window) return result;
    const int used = std::clamp(static_cast<int>(window->usedPercent), 0, 100);
    result.remainingPercent = static_cast<double>(100 - used);
    result.resetsAtUnixSeconds = window->resetsAtUnixSeconds;
    return result;
}

bool HasHistoryValue(const QuotaHistorySample& sample) noexcept {
    return sample.fiveHour.remainingPercent ||
           sample.fiveHour.resetsAtUnixSeconds ||
           sample.weekly.remainingPercent ||
           sample.weekly.resetsAtUnixSeconds;
}

std::vector<QuotaForecastSample> ForecastSamples(
    const std::vector<QuotaHistorySample>& history,
    bool weekly) {
    std::vector<QuotaForecastSample> result;
    result.reserve(history.size());
    for (const QuotaHistorySample& sample : history) {
        const QuotaHistoryWindowSample& window =
            weekly ? sample.weekly : sample.fiveHour;
        if (!window.remainingPercent || !window.resetsAtUnixSeconds) continue;
        result.push_back(QuotaForecastSample{
            static_cast<double>(sample.capturedAtUnixSeconds),
            *window.remainingPercent,
            static_cast<double>(*window.resetsAtUnixSeconds),
        });
    }
    return result;
}

QuotaWindowForecastRefresh CalculateWindowForecast(
    const RateLimitWindow* current,
    const std::vector<QuotaHistorySample>& history,
    bool weekly,
    std::int64_t nowUnixSeconds) {
    QuotaWindowForecastRefresh result;
    result.windowReturned = current != nullptr;
    if (!current || !current->resetsAtUnixSeconds) {
        result.forecast = CalculateQuotaForecast(
            {}, -1.0, -1.0, static_cast<double>(nowUnixSeconds));
        return result;
    }
    const int used = std::clamp(static_cast<int>(current->usedPercent), 0, 100);
    result.forecast = CalculateQuotaForecast(
        ForecastSamples(history, weekly), static_cast<double>(100 - used),
        static_cast<double>(*current->resetsAtUnixSeconds),
        static_cast<double>(nowUnixSeconds));
    return result;
}

QuotaForecastRefresh RefreshQuotaForecast(
    const RateLimitsData& limits,
    const std::filesystem::path& historyPath,
    QuotaHistoryAtomicReplace atomicReplace) {
    const auto nowDuration = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch());
    const std::int64_t nowUnixSeconds = nowDuration.count();
    const RateLimitWindow* fiveHour = SelectQuotaWindow(limits, false);
    const RateLimitWindow* weekly = SelectQuotaWindow(limits, true);
    const QuotaHistorySample current{
        nowUnixSeconds,
        MakeHistoryWindow(fiveHour),
        MakeHistoryWindow(weekly),
    };

    QuotaHistoryStore store(historyPath, std::move(atomicReplace));
    QuotaHistoryLoadResult loaded = store.Load(nowUnixSeconds);
    std::vector<QuotaHistorySample> history =
        loaded.ok() ? std::move(loaded.samples)
                    : std::vector<QuotaHistorySample>{};
    if (HasHistoryValue(current)) history.push_back(current);

    QuotaForecastRefresh result;
    result.fiveHour =
        CalculateWindowForecast(fiveHour, history, false, nowUnixSeconds);
    result.weekly =
        CalculateWindowForecast(weekly, history, true, nowUnixSeconds);

    if (historyPath.empty()) {
        result.historySaveFailed = HasHistoryValue(current);
        return result;
    }
    if (!HasHistoryValue(current)) {
        return result;
    }
    const QuotaHistoryUpdateResult updated = store.Update(current);
    result.historyStored =
        updated.status == QuotaHistoryUpdateStatus::kWritten ||
        updated.status == QuotaHistoryUpdateStatus::kSkippedTooSoon;
    result.historySaveFailed = !result.historyStored;
    return result;
}

std::optional<std::string> LocalDateFromUnixMilliseconds(
    std::int64_t milliseconds) noexcept {
    if (milliseconds < 0) return std::nullopt;
    const std::int64_t seconds = milliseconds / 1000;
    if (seconds > static_cast<std::int64_t>(
                      std::numeric_limits<std::time_t>::max())) {
        return std::nullopt;
    }
    const std::time_t value = static_cast<std::time_t>(seconds);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &value) != 0) return std::nullopt;
#else
    if (!localtime_r(&value, &local)) return std::nullopt;
#endif
    char buffer[11]{};
    if (std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d",
                      local.tm_year + 1900, local.tm_mon + 1,
                      local.tm_mday) != 10) {
        return std::nullopt;
    }
    return std::string(buffer);
}

std::optional<std::string> CurrentLocalDateString() noexcept {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch());
    return LocalDateFromUnixMilliseconds(now.count());
}

CodexCostRefresh RefreshCostHistory(
    const std::optional<std::filesystem::path>& codexHome,
    CodexCostHistoryState& historyState,
    const std::function<bool()>& shouldCancel) {
    CodexCostRefresh output;
    CodexCostFileScanResult scan;
    if (codexHome) {
        CodexCostFileScanRequest request;
        request.codexHome = *codexHome;
        request.nowUnixSeconds =
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        request.previousFiles = historyState.Cursors();
        request.shouldCancel = shouldCancel;
        scan = ScanCodexCostRolloutFiles(request);
        output.status = scan.ok() ? CodexCostRefreshStatus::kNoTokenEvents
                                  : CodexCostRefreshStatus::kScanFailed;
        output.coverageIncomplete = scan.coverageIncomplete;
        output.skippedCompressedFiles = scan.skippedCompressedFiles;
        output.skippedOversizedLines = scan.skippedOversizedLines;
        output.rejectedUnsafeEntries = scan.rejectedUnsafeEntries;
    } else {
        scan.status = CodexCostFileScanStatus::kInvalidArgument;
        output.status = CodexCostRefreshStatus::kCodexHomeUnavailable;
    }

    CodexCostHistoryApplyResult applied = historyState.Apply(
        scan, LocalDateFromUnixMilliseconds);
    output.malformedLineCount = applied.malformedLineCount;
    const std::optional<std::string> currentDate = CurrentLocalDateString();
    if (currentDate) {
        output.localSummary =
            CalculateCodexCostSummary(applied.events, *currentDate);
        if (output.localSummary) {
            output.localSummary->saturated =
                output.localSummary->saturated || applied.saturated;
        }
    }
    const bool hasEvents = output.localSummary && output.localSummary->available;
    if (scan.ok() && hasEvents) {
        output.status = CodexCostRefreshStatus::kAvailable;
    } else if (!scan.ok() && hasEvents) {
        output.showingLastKnown = true;
    }
    return output;
}

struct ApartmentCleanup {
    bool initialized = false;

    ~ApartmentCleanup() {
        if (initialized) winrt::uninit_apartment();
    }
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
                        std::string_view clientVersion,
                        std::filesystem::path quotaHistoryPath) {
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
        quotaHistoryPath_ = std::move(quotaHistoryPath);
        thread_ = std::thread(&CodexWorker::Run, this);
    } catch (...) {
        completionWindow_ = nullptr;
        completionMessage_ = 0;
        clientVersion_.clear();
        quotaHistoryPath_.clear();
        return false;
    }
    started_ = true;
    return true;
}

bool CodexWorker::SetQuotaForecastEnabled(bool enabled) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ || quotaForecastEnabled_ == enabled) return false;
        quotaForecastEnabled_ = enabled;
        quotaForecastEpoch_.fetch_add(1, std::memory_order_acq_rel);
    }
    // Enabling while other Codex cards already keep the worker active reuses
    // the next normal refresh instead of creating an extra app-server read.
    // A paused worker is activated immediately by the shared demand gate.
    wake_.notify_one();
    return true;
}

bool CodexWorker::SetCostHistoryEnabled(bool enabled) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ || costHistoryEnabled_ == enabled) return false;
        costHistoryEnabled_ = enabled;
        costHistoryEpoch_.fetch_add(1, std::memory_order_acq_rel);
    }
    // This feature shares the normal five-minute Codex refresh. If the worker
    // is paused, the page-level demand gate activates it immediately.
    wake_.notify_one();
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
        quotaForecastEpoch_.fetch_add(1, std::memory_order_acq_rel);
        costHistoryEpoch_.fetch_add(1, std::memory_order_acq_rel);
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
        quotaForecastEpoch_.fetch_add(1, std::memory_order_acq_rel);
        costHistoryEpoch_.fetch_add(1, std::memory_order_acq_rel);
        completionWindow_ = nullptr;
        completionMessage_ = 0;
        clientVersion_.clear();
        quotaHistoryPath_.clear();
        quotaForecastEnabled_ = false;
        costHistoryEnabled_ = false;
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

    // Declared before the client so COM remains initialized until every
    // thread-owned WinRT object has been destroyed, including on early return.
    ApartmentCleanup apartmentCleanup{apartmentInitialized};

    // The client and its retained parsed state never leave this thread.
    CodexAppServerClient client;
    CodexCostHistoryState costHistoryState;
    std::size_t consecutiveFailureCount = 0;
    for (;;) {
        std::optional<::codex_monitor::CodexRefreshWorkItem> item;
        std::uint64_t refreshEpoch = 0;
        std::uint64_t forecastEpoch = 0;
        std::uint64_t costEpoch = 0;
        bool forecastEnabled = false;
        bool costEnabled = false;
        std::string clientVersion;
        std::filesystem::path quotaHistoryPath;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            for (;;) {
                if (schedule_.IsStopped()) {
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
                    forecastEnabled = quotaForecastEnabled_;
                    forecastEpoch =
                        quotaForecastEpoch_.load(std::memory_order_acquire);
                    costEnabled = costHistoryEnabled_;
                    costEpoch =
                        costHistoryEpoch_.load(std::memory_order_acquire);
                    quotaHistoryPath = quotaHistoryPath_;
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
        std::optional<std::filesystem::path> refreshedCodexHome;
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
                    if (costEnabled) refreshedCodexHome = client.codexHome();
                }
            } catch (...) {
                refresh = FailureResult(
                    client.data(), AppServerClientFailureKind::kTransportFailed,
                    L"Codex refresh failed unexpectedly");
            }
        }

        std::optional<QuotaForecastRefresh> quotaForecastUpdate;
        if (forecastEnabled && refresh.report.rateLimitsResponseReceived &&
            !refresh.data.rateLimits.lastFailure &&
            refresh.data.rateLimits.lastValue &&
            cancellationEpoch_.load(std::memory_order_acquire) == refreshEpoch &&
            quotaForecastEpoch_.load(std::memory_order_acquire) == forecastEpoch) {
            const QuotaHistoryAtomicReplace replace =
                [this, forecastEpoch](const std::filesystem::path& temporary,
                                      const std::filesystem::path& destination) {
                    // Keep the visibility check and the atomic replacement in
                    // one critical section. Once disabling returns, no pending
                    // history commit can still land on disk.
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!started_ || !quotaForecastEnabled_ ||
                        quotaForecastEpoch_.load(std::memory_order_acquire) !=
                            forecastEpoch) {
                        return std::make_error_code(
                            std::errc::operation_canceled);
                    }
                    if (MoveFileExW(temporary.c_str(), destination.c_str(),
                                    MOVEFILE_REPLACE_EXISTING |
                                        MOVEFILE_WRITE_THROUGH)) {
                        return std::error_code{};
                    }
                    return std::error_code(
                        static_cast<int>(GetLastError()),
                        std::system_category());
                };
            quotaForecastUpdate = RefreshQuotaForecast(
                *refresh.data.rateLimits.lastValue, quotaHistoryPath, replace);
        }

        std::optional<CodexCostRefresh> costHistoryUpdate;
        if (costEnabled &&
            cancellationEpoch_.load(std::memory_order_acquire) == refreshEpoch &&
            costHistoryEpoch_.load(std::memory_order_acquire) == costEpoch) {
            costHistoryUpdate =
                RefreshCostHistory(
                    refreshedCodexHome, costHistoryState,
                    [this, refreshEpoch, costEpoch] {
                        return cancellationEpoch_.load(
                                   std::memory_order_acquire) != refreshEpoch ||
                               costHistoryEpoch_.load(
                                   std::memory_order_acquire) != costEpoch;
                    });
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
                if (!quotaForecastEnabled_ ||
                    quotaForecastEpoch_.load(std::memory_order_acquire) !=
                        forecastEpoch) {
                    quotaForecastUpdate.reset();
                }
                if (!costHistoryEnabled_ ||
                    costHistoryEpoch_.load(std::memory_order_acquire) !=
                        costEpoch) {
                    costHistoryUpdate.reset();
                }
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
                    std::move(quotaForecastUpdate),
                    std::move(costHistoryUpdate), succeeded, delay};
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
