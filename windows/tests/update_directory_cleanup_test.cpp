#include "update/update_directory_cleanup_win32.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

using namespace codex_monitor::update;

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::wstring ManagedName(wchar_t digit) {
    return L"update-" + std::wstring(32U, digit);
}

WindowsUpdateDirectoryCleanupEntry Entry(
    wchar_t digit,
    std::uint64_t lastWrite,
    bool isDirectory = true,
    bool isReparsePoint = false) {
    return {ManagedName(digit), lastWrite, isDirectory, isReparsePoint};
}

void TestExactManagedNameContract() {
    Require(IsManagedWindowsUpdateDirectoryName(ManagedName(L'0')),
            "the exact lowercase random-directory shape is accepted");
    Require(IsManagedWindowsUpdateDirectoryName(
                L"update-0123456789abcdef0123456789abcdef"),
            "all lowercase hexadecimal digits are accepted");
    Require(!IsManagedWindowsUpdateDirectoryName(
                L"update-0123456789ABCDEF0123456789abcdef"),
            "uppercase hexadecimal is outside the managed namespace");
    Require(!IsManagedWindowsUpdateDirectoryName(
                L"update-0123456789abcdef0123456789abcdeg"),
            "non-hexadecimal names are rejected");
    Require(!IsManagedWindowsUpdateDirectoryName(
                L"update-0123456789abcdef0123456789abcde"),
            "short names are rejected");
    Require(!IsManagedWindowsUpdateDirectoryName(
                L"other--0123456789abcdef0123456789abcdef"),
            "other prefixes are rejected");
}

void TestSevenDayAndNewestTwoRetention() {
    constexpr std::uint64_t now =
        30ULL * 24ULL * 60ULL * 60ULL * 10'000'000ULL;
    constexpr std::uint64_t day =
        24ULL * 60ULL * 60ULL * 10'000'000ULL;
    const std::vector<WindowsUpdateDirectoryCleanupEntry> entries = {
        Entry(L'a', now - 1ULL * day),
        Entry(L'b', now - 2ULL * day),
        Entry(L'c', now - 6ULL * day),
        Entry(L'd', now - 7ULL * day),
        Entry(L'e', now - 20ULL * day),
    };
    const auto selected =
        SelectStaleWindowsUpdateDirectories(entries, now);
    Require(selected ==
                std::vector<std::wstring>({ManagedName(L'd'),
                                           ManagedName(L'e')}),
            "recent entries and the two newest are kept; seven-day-old entries may be removed");

    const std::vector<WindowsUpdateDirectoryCleanupEntry> onlyOld = {
        Entry(L'1', now - 8ULL * day),
        Entry(L'2', now - 9ULL * day),
    };
    Require(SelectStaleWindowsUpdateDirectories(onlyOld, now).empty(),
            "the two newest managed directories are retained even when old");
}

void TestFilesReparsePointsAndUnknownTimesAreNeverSelected() {
    constexpr std::uint64_t now =
        50ULL * kMinimumUpdateDirectoryAge100Nanoseconds;
    const std::uint64_t old =
        now - 2ULL * kMinimumUpdateDirectoryAge100Nanoseconds;
    std::vector<WindowsUpdateDirectoryCleanupEntry> entries = {
        Entry(L'1', now - 1U),
        Entry(L'2', now - 2U),
        Entry(L'3', old, false, false),
        Entry(L'4', old, true, true),
        Entry(L'5', 0U),
        {L"not-managed", old, true, false},
        Entry(L'6', old),
    };
    const auto selected =
        SelectStaleWindowsUpdateDirectories(entries, now);
    Require(selected == std::vector<std::wstring>({ManagedName(L'6')}),
            "only an ordinary, strictly named directory with a known old timestamp is selected");
}

void TestFutureTimesFailClosed() {
    constexpr std::uint64_t now =
        20ULL * kMinimumUpdateDirectoryAge100Nanoseconds;
    const std::vector<WindowsUpdateDirectoryCleanupEntry> entries = {
        Entry(L'a', now + 10U),
        Entry(L'b', now + 9U),
        Entry(L'c', now + 8U),
    };
    Require(SelectStaleWindowsUpdateDirectories(entries, now).empty(),
            "clock rollback or future timestamps cannot cause deletion");
}

#ifdef _WIN32

bool PathExists(const std::filesystem::path& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::filesystem::path CreateTemporaryRoot() {
    wchar_t temporaryPath[MAX_PATH + 1]{};
    const DWORD length = GetTempPathW(MAX_PATH, temporaryPath);
    Require(length > 0U && length < MAX_PATH,
            "Windows temporary path is available");

    wchar_t uniquePath[MAX_PATH + 1]{};
    Require(GetTempFileNameW(temporaryPath, L"cmh", 0U, uniquePath) != 0U,
            "a unique cleanup test path is available");
    Require(DeleteFileW(uniquePath) != FALSE,
            "the temporary placeholder can be removed");
    Require(CreateDirectoryW(uniquePath, nullptr) != FALSE,
            "the temporary cleanup root can be created");
    return std::filesystem::path(uniquePath);
}

void CreateOldFlatDirectory(const std::filesystem::path& root,
                            const std::wstring& name,
                            std::uint64_t ageDays) {
    const std::filesystem::path directory = root / name;
    Require(CreateDirectoryW(directory.c_str(), nullptr) != FALSE,
            "managed fixture directory can be created");
    const std::filesystem::path file = directory / L"fixture.bin";
    HANDLE rawFile = CreateFileW(
        file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(rawFile != INVALID_HANDLE_VALUE,
            "managed fixture file can be created");
    CloseHandle(rawFile);

    HANDLE rawDirectory = CreateFileW(
        directory.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    Require(rawDirectory != INVALID_HANDLE_VALUE,
            "managed fixture timestamp can be opened");
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const std::uint64_t nowTicks =
        (static_cast<std::uint64_t>(now.dwHighDateTime) << 32U) |
        static_cast<std::uint64_t>(now.dwLowDateTime);
    const std::uint64_t oldTicks =
        nowTicks - ageDays * 24ULL * 60ULL * 60ULL * 10'000'000ULL;
    FILETIME old{};
    old.dwLowDateTime = static_cast<DWORD>(oldTicks);
    old.dwHighDateTime = static_cast<DWORD>(oldTicks >> 32U);
    Require(SetFileTime(rawDirectory, nullptr, nullptr, &old) != FALSE,
            "managed fixture timestamp can be aged");
    CloseHandle(rawDirectory);
}

void RemoveFlatFixture(const std::filesystem::path& directory) {
    DeleteFileW((directory / L"fixture.bin").c_str());
    RemoveDirectoryW(directory.c_str());
}

void TestWindowsFilesystemCleanup() {
    const std::filesystem::path root = CreateTemporaryRoot();
    const std::wstring newest = ManagedName(L'a');
    const std::wstring secondNewest = ManagedName(L'b');
    const std::wstring stale = ManagedName(L'c');
    const std::wstring unmanaged = L"update-" + std::wstring(32U, L'A');
    CreateOldFlatDirectory(root, newest, 8U);
    CreateOldFlatDirectory(root, secondNewest, 9U);
    CreateOldFlatDirectory(root, stale, 10U);
    CreateOldFlatDirectory(root, unmanaged, 20U);

    BestEffortCleanupWindowsUpdateDirectories(root);

    Require(PathExists(root / newest) && PathExists(root / secondNewest),
            "the two newest real directories are retained");
    Require(!PathExists(root / stale),
            "an old third real directory and its flat file are removed");
    Require(PathExists(root / unmanaged),
            "an unmanaged real directory is untouched");

    RemoveFlatFixture(root / newest);
    RemoveFlatFixture(root / secondNewest);
    RemoveFlatFixture(root / unmanaged);
    RemoveDirectoryW(root.c_str());
}

#endif

}  // namespace

int main() {
    TestExactManagedNameContract();
    TestSevenDayAndNewestTwoRetention();
    TestFilesReparsePointsAndUnknownTimesAreNeverSelected();
    TestFutureTimesFailClosed();
#ifdef _WIN32
    TestWindowsFilesystemCleanup();
#endif
    std::cout << "update_directory_cleanup_tests=pass\n";
    return 0;
}
