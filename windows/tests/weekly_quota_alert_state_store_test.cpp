#include "codex/weekly_quota_alert_state_store.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

using codex_monitor::codex::WeeklyQuotaAlertMode;
using codex_monitor::codex::WeeklyQuotaAlertState;
using codex_monitor::codex::WeeklyQuotaAlertStateLoadStatus;
using codex_monitor::codex::WeeklyQuotaAlertStateSaveStatus;
using codex_monitor::codex::WeeklyQuotaAlertStateStore;

void Require(bool condition, std::string_view message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto sequence = std::chrono::high_resolution_clock::now()
                                  .time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("codex-monitor-weekly-alert-" +
                 std::to_string(sequence));
        std::error_code error;
        Require(std::filesystem::create_directories(path_, error) && !error,
                "temporary state directory must be created");
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

void Write(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(contents.data(),
                 static_cast<std::streamsize>(contents.size()));
    output.flush();
    Require(static_cast<bool>(output), "state fixture must be written");
}

std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    Require(static_cast<bool>(input), "state fixture must be readable");
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

WeeklyQuotaAlertState NotifiedState() {
    WeeklyQuotaAlertState state;
    state.weeklyResetAtUnixSeconds = 2'000'400'000;
    state.mode = WeeklyQuotaAlertMode::kRolling24Hours;
    state.lastEvaluatedAtUnixSeconds = 2'000'000'000;
    state.lastNotifiedPeriodStartUnixSeconds =
        2'000'000'000 - 24 * 60 * 60;
    state.lastNotifiedAtUnixSeconds = 2'000'000'000;
    return state;
}

void TestRoundTripAndWhitelist() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "weekly-alert-state.txt";
    const WeeklyQuotaAlertState expected = NotifiedState();
    Require(WeeklyQuotaAlertStateStore(path).Save(expected).written(),
            "a valid anti-duplication state must save");
    const auto loaded = WeeklyQuotaAlertStateStore(path).Load();
    Require(loaded.status == WeeklyQuotaAlertStateLoadStatus::kOk &&
                loaded.state.weeklyResetAtUnixSeconds ==
                    expected.weeklyResetAtUnixSeconds &&
                loaded.state.mode == WeeklyQuotaAlertMode::kRolling24Hours &&
                loaded.state.lastEvaluatedAtUnixSeconds ==
                    expected.lastEvaluatedAtUnixSeconds &&
                loaded.state.lastNotifiedPeriodStartUnixSeconds ==
                    expected.lastNotifiedPeriodStartUnixSeconds &&
                loaded.state.lastNotifiedAtUnixSeconds ==
                    expected.lastNotifiedAtUnixSeconds,
            "all whitelisted anti-duplication fields must round-trip");

    const std::string contents = Read(path);
    Require(contents.rfind("version=1\n", 0) == 0 &&
                contents.find("token") == std::string::npos &&
                contents.find("account") == std::string::npos &&
                contents.find("cookie") == std::string::npos &&
                contents.find("remaining") == std::string::npos,
            "persistent alert state must not duplicate quota or account data");
}

void TestMissingAndEmptyState() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "weekly-alert-state.txt";
    Require(WeeklyQuotaAlertStateStore(path).Load().status ==
                WeeklyQuotaAlertStateLoadStatus::kNotFound,
            "a missing alert state is a normal first run");
    Require(WeeklyQuotaAlertStateStore(path).Save({}).written(),
            "the explicit empty state must save");
    const auto loaded = WeeklyQuotaAlertStateStore(path).Load();
    Require(loaded.status == WeeklyQuotaAlertStateLoadStatus::kOk &&
                !loaded.state.weeklyResetAtUnixSeconds &&
                !loaded.state.lastEvaluatedAtUnixSeconds &&
                loaded.state.mode ==
                    WeeklyQuotaAlertMode::kRolling24Hours,
            "the empty state must round-trip without inventing a cycle");
}

void TestDamageFallsBackToEmptyAndCanBeRepaired() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "weekly-alert-state.txt";
    Write(path,
          "version=1\nweekly_reset_at=2000400000\nmode=rolling24h\n"
          "last_evaluated_at=2000000000\n"
          "last_notified_period_start=1999913600\n"
          "last_notified_at=bad\n");
    const auto malformed = WeeklyQuotaAlertStateStore(path).Load();
    Require(malformed.status ==
                WeeklyQuotaAlertStateLoadStatus::kMalformed &&
                !malformed.state.weeklyResetAtUnixSeconds &&
                !malformed.state.lastEvaluatedAtUnixSeconds &&
                !malformed.state.lastNotifiedAtUnixSeconds,
            "a damaged file must expose only a safe empty fallback state");
    Require(WeeklyQuotaAlertStateStore(path).Save(NotifiedState()).written() &&
                WeeklyQuotaAlertStateStore(path).Load().status ==
                    WeeklyQuotaAlertStateLoadStatus::kOk,
            "a malformed known version must be atomically repairable");
}

void TestSizeLineKeysAndRelationshipBounds() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "weekly-alert-state.txt";
    Write(path, std::string(1025, 'x'));
    Require(WeeklyQuotaAlertStateStore(path).Load().status ==
                WeeklyQuotaAlertStateLoadStatus::kMalformed,
            "an oversized state must be rejected before parsing");

    Write(path,
          "version=1\nweekly_reset_at=-\nmode=" +
              std::string(129, 'x') +
              "\nlast_evaluated_at=-\nlast_notified_period_start=-\n"
              "last_notified_at=-\n");
    Require(WeeklyQuotaAlertStateStore(path).Load().status ==
                WeeklyQuotaAlertStateLoadStatus::kMalformed,
            "an overlong line must be rejected");

    Write(path,
          "version=1\nweekly_reset_at=-\nmode=naturalDay\n"
          "last_evaluated_at=-\nlast_notified_period_start=-\n"
          "last_notified_at=-\nextra=1\n");
    Require(WeeklyQuotaAlertStateStore(path).Load().status ==
                WeeklyQuotaAlertStateLoadStatus::kMalformed,
            "unknown keys must reject the whole whitelist state");

    WeeklyQuotaAlertState invalid = NotifiedState();
    invalid.lastNotifiedAtUnixSeconds =
        *invalid.lastEvaluatedAtUnixSeconds + 1;
    Require(WeeklyQuotaAlertStateStore(path).Save(invalid).status ==
                WeeklyQuotaAlertStateSaveStatus::kInvalidState,
            "impossible notification and evaluation ordering must not save");
}

void TestUnknownVersionIsNeverOverwritten() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "weekly-alert-state.txt";
    Write(path,
          "version=2\nweekly_reset_at=-\nmode=naturalDay\n"
          "last_evaluated_at=-\nlast_notified_period_start=-\n"
          "last_notified_at=-\n");
    const std::string before = Read(path);
    const auto save = WeeklyQuotaAlertStateStore(path).Save(NotifiedState());
    Require(save.status ==
                WeeklyQuotaAlertStateSaveStatus::kUnsupportedVersion &&
                Read(path) == before,
            "a newer unknown state format must never be overwritten");
}

void TestAtomicFailurePreservesThePreviousState() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "weekly-alert-state.txt";
    Require(WeeklyQuotaAlertStateStore(path).Save({}).written(),
            "atomic failure fixture must create the old state");
    const std::string before = Read(path);
    bool callbackRan = false;
    std::filesystem::path observedTemporary;
    WeeklyQuotaAlertStateStore failing(
        path,
        [&](const std::filesystem::path& temporaryPath,
            const std::filesystem::path& destinationPath) {
            callbackRan = true;
            observedTemporary = temporaryPath;
            Require(temporaryPath.parent_path() ==
                        destinationPath.parent_path() &&
                        std::filesystem::exists(temporaryPath),
                    "atomic replacement must use a complete same-directory temporary file");
            return std::make_error_code(std::errc::permission_denied);
        });
    const auto failed = failing.Save(NotifiedState());
    Require(callbackRan &&
                failed.status == WeeklyQuotaAlertStateSaveStatus::kIoError &&
                Read(path) == before && !observedTemporary.empty() &&
                !std::filesystem::exists(observedTemporary),
            "replacement failure must preserve old bytes and remove the temporary file");
}

}  // namespace

int main() {
    TestRoundTripAndWhitelist();
    TestMissingAndEmptyState();
    TestDamageFallsBackToEmptyAndCanBeRepaired();
    TestSizeLineKeysAndRelationshipBounds();
    TestUnknownVersionIsNeverOverwritten();
    TestAtomicFailurePreservesThePreviousState();
    std::cout << "weekly_quota_alert_state_store_test: pass\n";
    return 0;
}
