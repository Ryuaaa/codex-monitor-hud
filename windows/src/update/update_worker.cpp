#include "update/update_worker.h"

#include "update/update_state_store.h"

#include <winrt/base.h>

#include <algorithm>
#include <system_error>
#include <utility>

namespace codex_monitor::update {
namespace {

std::int64_t CurrentUnixSeconds() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

struct ApartmentCleanup {
    bool initialized = false;
    ~ApartmentCleanup() {
        if (initialized) winrt::uninit_apartment();
    }
};

WindowsUpdateCheckResult RuntimeFailure() {
    WindowsUpdateCheckResult result;
    result.status = WindowsUpdateCheckStatus::kInvalidResponse;
    result.error = L"Windows runtime initialization failed";
    return result;
}

}  // namespace

WindowsUpdateWorker::WindowsUpdateWorker(
    WindowsUpdateChecker checker,
    WindowsUpdateWorkerTiming timing)
    : checker_(std::move(checker)), timing_(timing) {
    if (!checker_) checker_ = CheckForWindowsUpdate;
    if (timing_.startupDelay < std::chrono::seconds::zero()) {
        timing_.startupDelay = std::chrono::seconds::zero();
    }
    if (timing_.automaticCheckInterval < std::chrono::seconds{60}) {
        timing_.automaticCheckInterval = std::chrono::seconds{60};
    }
}

WindowsUpdateWorker::~WindowsUpdateWorker() {
    StopAndJoin();
}

bool WindowsUpdateWorker::Start(
    HWND completionWindow,
    UINT completionMessage,
    std::string_view currentVersion,
    std::filesystem::path statePath) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_ || thread_.joinable() || stopRequested_ ||
        !completionWindow || completionMessage < WM_APP ||
        currentVersion.empty() || statePath.empty() || !checker_) {
        return false;
    }
    completionWindow_ = completionWindow;
    completionMessage_ = completionMessage;
    currentVersion_.assign(currentVersion);
    statePath_ = std::move(statePath);
    try {
        thread_ = std::thread(&WindowsUpdateWorker::Run, this);
    } catch (...) {
        completionWindow_ = nullptr;
        completionMessage_ = 0;
        currentVersion_.clear();
        statePath_.clear();
        return false;
    }
    started_ = true;
    return true;
}

bool WindowsUpdateWorker::RequestManualCheck() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ || stopRequested_ || manualPending_ || busy_) return false;
        manualPending_ = true;
        nextAutomaticCheck_.reset();
    }
    wake_.notify_one();
    return true;
}

std::optional<CompletedWindowsUpdateCheck>
WindowsUpdateWorker::TakeLatest() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::optional<CompletedWindowsUpdateCheck> result = std::move(latest_);
    latest_.reset();
    return result;
}

bool WindowsUpdateWorker::IsBusy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return started_ && (manualPending_ || busy_);
}

void WindowsUpdateWorker::StopAndJoin() {
    RequestStop();
    std::thread threadToJoin;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!thread_.joinable()) {
            latest_.reset();
            return;
        }
        threadToJoin = std::move(thread_);
    }
    if (threadToJoin.joinable()) threadToJoin.join();
}

void WindowsUpdateWorker::RequestStop() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopRequested_ && !started_) return;
        stopRequested_ = true;
        manualPending_ = false;
        busy_ = false;
        nextAutomaticCheck_.reset();
        latest_.reset();
        cancellationEpoch_.fetch_add(1, std::memory_order_acq_rel);
        completionWindow_ = nullptr;
        completionMessage_ = 0;
        started_ = false;
    }
    wake_.notify_one();
}

void WindowsUpdateWorker::Run() {
    bool apartmentInitialized = false;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartmentInitialized = true;
    } catch (...) {
        // An ordinary checked failure is published below.
    }
    ApartmentCleanup apartmentCleanup{apartmentInitialized};

    UpdateCheckState retainedState;
    const UpdateStateLoadResult loaded = UpdateStateStore(statePath_).Load();
    if (loaded.ok()) retainedState = loaded.state;

    const std::optional<SemanticVersion> currentVersion =
        ParseSemVerTag(currentVersion_);
    const std::optional<SemanticVersion> cachedVersion =
        ParseSemVerTag(retainedState.availableVersion);
    const std::string normalizedCurrentVersion =
        currentVersion ? currentVersion->canonical : currentVersion_;
    const bool applicationVersionChanged =
        retainedState.checkedVersion.empty() ||
        retainedState.checkedVersion != normalizedCurrentVersion;
    const bool cachedUpdateStillNewer =
        currentVersion && cachedVersion && cachedVersion->IsStable() &&
        cachedVersion->buildMetadata.empty() &&
        CompareSemVerPrecedence(*cachedVersion, *currentVersion) > 0;
    if (!cachedUpdateStillNewer) retainedState.availableVersion.clear();
    if (applicationVersionChanged) retainedState.lastCheckUnixSeconds = 0;

    const std::int64_t initialNow = CurrentUnixSeconds();
    const std::int64_t interval = timing_.automaticCheckInterval.count();
    std::chrono::seconds initialDelay = timing_.startupDelay;
    if (retainedState.lastCheckUnixSeconds > 0 && interval > 0 &&
        retainedState.lastCheckUnixSeconds <= initialNow + 300) {
        const std::int64_t elapsed =
            std::max<std::int64_t>(0,
                initialNow - retainedState.lastCheckUnixSeconds);
        if (elapsed < interval) {
            initialDelay = std::chrono::seconds{interval - elapsed};
        }
    }

    HWND cachedNotifyWindow = nullptr;
    UINT cachedNotifyMessage = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopRequested_) return;
        nextAutomaticCheck_ =
            std::chrono::steady_clock::now() + initialDelay;
        if (!retainedState.availableVersion.empty()) {
            WindowsUpdateCheckResult cached;
            cached.status = WindowsUpdateCheckStatus::kUpdateAvailable;
            latest_ = CompletedWindowsUpdateCheck{
                std::move(cached), retainedState.availableVersion,
                retainedState.lastCheckUnixSeconds, false, true, false};
            cachedNotifyWindow = completionWindow_;
            cachedNotifyMessage = completionMessage_;
        }
    }
    if (cachedNotifyWindow && cachedNotifyMessage != 0) {
        PostMessageW(cachedNotifyWindow, cachedNotifyMessage, 0, 0);
    }

    for (;;) {
        bool manual = false;
        std::uint64_t refreshEpoch = 0;
        HWND busyNotifyWindow = nullptr;
        UINT busyNotifyMessage = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            for (;;) {
                if (stopRequested_) return;
                const auto now = std::chrono::steady_clock::now();
                const bool automaticDue =
                    nextAutomaticCheck_ && now >= *nextAutomaticCheck_;
                if (!busy_ && (manualPending_ || automaticDue)) {
                    manual = manualPending_;
                    manualPending_ = false;
                    busy_ = true;
                    nextAutomaticCheck_.reset();
                    refreshEpoch = cancellationEpoch_.load(
                        std::memory_order_acquire);
                    busyNotifyWindow = completionWindow_;
                    busyNotifyMessage = completionMessage_;
                    break;
                }
                if (nextAutomaticCheck_) {
                    wake_.wait_until(lock, *nextAutomaticCheck_);
                } else {
                    wake_.wait(lock);
                }
            }
        }
        if (busyNotifyWindow && busyNotifyMessage != 0) {
            PostMessageW(busyNotifyWindow, busyNotifyMessage, 0, 0);
        }

        WindowsUpdateCheckResult checked;
        if (!apartmentInitialized) {
            checked = RuntimeFailure();
        } else {
            try {
                checked = checker_(currentVersion_, [this, refreshEpoch] {
                    return cancellationEpoch_.load(
                               std::memory_order_acquire) != refreshEpoch;
                });
            } catch (...) {
                checked.status = WindowsUpdateCheckStatus::kInvalidResponse;
                checked.error = L"Update check failed unexpectedly";
            }
        }
        const std::int64_t checkedAt = CurrentUnixSeconds();

        if (checked.succeeded()) {
            retainedState.availableVersion =
                checked.release ? checked.release->version.canonical : "";
        }
        retainedState.checkedVersion = normalizedCurrentVersion;
        retainedState.lastCheckUnixSeconds = std::max<std::int64_t>(0, checkedAt);

        bool stateSaveFailed = false;
        const UpdateStateAtomicReplace replace =
            [this, refreshEpoch](const std::filesystem::path& temporary,
                                 const std::filesystem::path& destination) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (stopRequested_ ||
                        cancellationEpoch_.load(std::memory_order_acquire) !=
                            refreshEpoch) {
                        return std::make_error_code(
                            std::errc::operation_canceled);
                    }
                }
                if (MoveFileExW(temporary.c_str(), destination.c_str(),
                                MOVEFILE_REPLACE_EXISTING |
                                    MOVEFILE_WRITE_THROUGH)) {
                    return std::error_code{};
                }
                return std::error_code(static_cast<int>(GetLastError()),
                                       std::system_category());
            };
        const UpdateStateSaveResult saved =
            UpdateStateStore(statePath_, replace).Save(retainedState);
        stateSaveFailed = !saved.written();

        HWND notifyWindow = nullptr;
        UINT notifyMessage = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            busy_ = false;
            if (stopRequested_) return;
            if (cancellationEpoch_.load(std::memory_order_acquire) !=
                refreshEpoch) {
                wake_.notify_one();
                continue;
            }
            latest_ = CompletedWindowsUpdateCheck{
                std::move(checked), retainedState.availableVersion,
                checkedAt, manual, false, stateSaveFailed};
            if (!manualPending_) {
                nextAutomaticCheck_ = std::chrono::steady_clock::now() +
                                      timing_.automaticCheckInterval;
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

}  // namespace codex_monitor::update
