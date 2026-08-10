#include "update/update_state_store.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

using codex_monitor::update::UpdateCheckState;
using codex_monitor::update::UpdateStateLoadStatus;
using codex_monitor::update::UpdateStateSaveStatus;
using codex_monitor::update::UpdateStateStore;

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path() /
                ("codex-update-state-" +
                 std::to_string(static_cast<unsigned long long>(
                     std::filesystem::file_time_type::clock::now()
                         .time_since_epoch().count())));
        std::error_code error;
        std::filesystem::create_directories(path_, error);
        Require(!error, "temporary update-state directory must be created");
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
    output << contents;
    Require(static_cast<bool>(output), "fixture must be written");
}

std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

void TestRoundTripAndPrivacyWhitelist() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "update-state.ini";
    const UpdateCheckState expected{
        2'000'000'000, "1.2.3", "1.2.2", "1.0.0"};
    Require(UpdateStateStore(path).Save(expected).written(),
            "valid update state must save");
    const auto loaded = UpdateStateStore(path).Load();
    Require(loaded.status == UpdateStateLoadStatus::kOk &&
                loaded.state.lastCheckUnixSeconds == 2'000'000'000 &&
                loaded.state.availableVersion == "1.2.3" &&
                loaded.state.lastNotifiedVersion == "1.2.2" &&
                loaded.state.checkedVersion == "1.0.0",
            "all whitelisted update-state fields must round-trip");
    const std::string contents = Read(path);
    Require(contents.rfind("version=2\n", 0) == 0 &&
                contents.find("token") == std::string::npos &&
                contents.find("cookie") == std::string::npos &&
                contents.find("url") == std::string::npos,
            "the current state version must contain no credential or URL field");
}

void TestMissingMalformedAndUnknownVersion() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "update-state.ini";
    Require(UpdateStateStore(path).Load().status ==
                UpdateStateLoadStatus::kNotFound,
            "a missing state is a normal first run");

    Write(path,
          "version=2\nlast_check=-1\navailable_version=1.2.3\n"
          "last_notified_version=\nchecked_version=1.0.0\n");
    Require(UpdateStateStore(path).Load().status ==
                UpdateStateLoadStatus::kMalformed,
            "negative timestamps must reject the whole state");

    Write(path,
          "version=3\nlast_check=10\navailable_version=1.2.3\n"
          "last_notified_version=\nchecked_version=1.0.0\n");
    const std::string before = Read(path);
    const auto save = UpdateStateStore(path).Save(
        {20, "1.2.4", "", "1.0.0"});
    Require(save.status == UpdateStateSaveStatus::kUnsupportedVersion &&
                Read(path) == before,
            "a newer unknown format must never be overwritten");
}

void TestAtomicReplaceFailurePreservesOldState() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "update-state.ini";
    Require(UpdateStateStore(path).Save(
                {10, "1.2.3", "", "1.0.0"}).written(),
            "the baseline state must save");
    const std::string before = Read(path);
    bool callbackRan = false;
    UpdateStateStore failing(
        path, [&callbackRan](const auto&, const auto&) {
            callbackRan = true;
            return std::make_error_code(std::errc::permission_denied);
        });
    const auto failed = failing.Save(
        {20, "1.2.4", "1.2.3", "1.0.0"});
    Require(callbackRan && failed.status == UpdateStateSaveStatus::kIoError &&
                Read(path) == before &&
                !std::filesystem::exists(path.string() + ".tmp"),
            "a failed atomic replacement must preserve the old state and clean up");
}

void TestStrictKeysAndVersions() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "update-state.ini";
    Write(path,
          "version=2\nlast_check=10\navailable_version=1.2.3\n"
          "last_notified_version=\nchecked_version=1.0.0\nextra=1\n");
    Require(UpdateStateStore(path).Load().status ==
                UpdateStateLoadStatus::kMalformed,
            "unknown fields must reject the state");
    Require(UpdateStateStore(path).Save(
                {10, "v1.2.3", "", "1.0.0"}).status ==
                UpdateStateSaveStatus::kInvalidState,
            "persisted versions must use normalized three-part form");
}

void TestVersionOneMigratesByForcingARefresh() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "update-state.ini";
    Write(path,
          "version=1\nlast_check=10\navailable_version=1.2.3\n"
          "last_notified_version=\n");
    const auto loaded = UpdateStateStore(path).Load();
    Require(loaded.status == UpdateStateLoadStatus::kOk &&
                loaded.state.checkedVersion.empty(),
            "version one state must load without pretending to know the checked app version");
    Require(UpdateStateStore(path).Save(
                {20, "1.2.4", "", "1.0.0"}).written() &&
                Read(path).rfind("version=2\n", 0) == 0,
            "saving a version one state must migrate it atomically to version two");
}

}  // namespace

int main() {
    TestRoundTripAndPrivacyWhitelist();
    TestMissingMalformedAndUnknownVersion();
    TestAtomicReplaceFailurePreservesOldState();
    TestStrictKeysAndVersions();
    TestVersionOneMigratesByForcingARefresh();
    std::cout << "update_state_store_test: pass\n";
    return 0;
}
