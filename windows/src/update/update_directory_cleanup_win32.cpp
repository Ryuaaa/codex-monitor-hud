#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "update/update_directory_cleanup_win32.h"

#include <algorithm>
#include <limits>
#include <optional>

#ifdef _WIN32
#include <windows.h>
#endif

namespace codex_monitor::update {
namespace {

struct IndexedCleanupEntry {
    std::wstring name;
    std::uint64_t lastWriteTime100Nanoseconds = 0U;
};

#ifdef _WIN32

constexpr std::size_t kMaximumRootEntriesToInspect = 1024U;
constexpr std::size_t kMaximumManagedDirectories = 256U;
constexpr std::size_t kMaximumFilesPerManagedDirectory = 16U;

class KernelHandle {
public:
    explicit KernelHandle(HANDLE value) noexcept : value_(value) {}
    ~KernelHandle() {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
    }
    KernelHandle(const KernelHandle&) = delete;
    KernelHandle& operator=(const KernelHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};

class FindHandle {
public:
    explicit FindHandle(HANDLE value) noexcept : value_(value) {}
    ~FindHandle() {
        if (value_ != INVALID_HANDLE_VALUE) FindClose(value_);
    }
    FindHandle(const FindHandle&) = delete;
    FindHandle& operator=(const FindHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};

std::uint64_t FileTimeTicks(const FILETIME& value) noexcept {
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32U) |
           static_cast<std::uint64_t>(value.dwLowDateTime);
}

bool SameWindowsPath(std::wstring_view lhs,
                     std::wstring_view rhs) noexcept {
    if (lhs.size() >
            static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        rhs.size() >
            static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    return CompareStringOrdinal(
               lhs.data(), static_cast<int>(lhs.size()), rhs.data(),
               static_cast<int>(rhs.size()), TRUE) == CSTR_EQUAL;
}

bool IsCanonicalFixedLocalDirectoryPath(
    const std::filesystem::path& path) {
    const std::wstring input = path.native();
    if (input.size() < 4U || input.size() >= 32760U ||
        !((input[0] >= L'a' && input[0] <= L'z') ||
          (input[0] >= L'A' && input[0] <= L'Z')) ||
        input[1] != L':' || input[2] != L'\\' ||
        input.back() == L'\\' ||
        input.find(L'/') != std::wstring::npos ||
        input.find(L'\0') != std::wstring::npos) {
        return false;
    }
    const std::wstring driveRoot = input.substr(0, 3U);
    if (GetDriveTypeW(driveRoot.c_str()) != DRIVE_FIXED) return false;

    std::wstring normalized(32768U, L'\0');
    const DWORD written = GetFullPathNameW(
        input.c_str(), static_cast<DWORD>(normalized.size()),
        normalized.data(), nullptr);
    if (written == 0U || written >= normalized.size()) return false;
    normalized.resize(written);
    return SameWindowsPath(input, normalized);
}

bool IsOrdinaryDirectoryHandle(HANDLE handle) noexcept {
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    return GetFileInformationByHandleEx(
               handle, FileAttributeTagInfo, &attributes,
               static_cast<DWORD>(sizeof(attributes))) &&
           (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U &&
           (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U;
}

bool IsOrdinaryFileHandle(HANDLE handle) noexcept {
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    return GetFileInformationByHandleEx(
               handle, FileAttributeTagInfo, &attributes,
               static_cast<DWORD>(sizeof(attributes))) &&
           (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U &&
           (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U;
}

std::optional<std::uint64_t> DirectoryLastWriteTime(HANDLE handle) noexcept {
    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandleEx(
            handle, FileBasicInfo, &basic,
            static_cast<DWORD>(sizeof(basic))) ||
        basic.LastWriteTime.QuadPart <= 0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(basic.LastWriteTime.QuadPart);
}

bool IsOldEnough(std::uint64_t lastWrite,
                 std::uint64_t now) noexcept {
    return lastWrite > 0U && now >= lastWrite &&
           now - lastWrite >=
               kMinimumUpdateDirectoryAge100Nanoseconds;
}

bool MarkHandleForDeletion(HANDLE handle) noexcept {
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    return SetFileInformationByHandle(
               handle, FileDispositionInfo, &disposition,
               static_cast<DWORD>(sizeof(disposition))) != FALSE;
}

bool DeleteFlatOrdinaryUpdateDirectory(
    const std::filesystem::path& directory,
    std::uint64_t now100Nanoseconds) noexcept {
    KernelHandle directoryHandle(CreateFileW(
        directory.c_str(),
        DELETE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!directoryHandle ||
        !IsOrdinaryDirectoryHandle(directoryHandle.get())) {
        return false;
    }
    const std::optional<std::uint64_t> lastWrite =
        DirectoryLastWriteTime(directoryHandle.get());
    if (!lastWrite.has_value() ||
        !IsOldEnough(*lastWrite, now100Nanoseconds)) {
        return false;
    }

    std::vector<std::wstring> fileNames;
    const std::filesystem::path wildcard = directory / L"*";
    {
        WIN32_FIND_DATAW data{};
        FindHandle find(FindFirstFileExW(
            wildcard.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
            nullptr, FIND_FIRST_EX_LARGE_FETCH));
        if (!find) {
            if (GetLastError() != ERROR_FILE_NOT_FOUND) return false;
        } else {
            do {
                const std::wstring_view name(data.cFileName);
                if (name == L"." || name == L"..") continue;
                if (fileNames.size() >=
                        kMaximumFilesPerManagedDirectory ||
                    (data.dwFileAttributes &
                     (FILE_ATTRIBUTE_DIRECTORY |
                      FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
                    return false;
                }
                fileNames.emplace_back(name);
            } while (FindNextFileW(find.get(), &data));
            if (GetLastError() != ERROR_NO_MORE_FILES) return false;
        }
    }

    for (const std::wstring& fileName : fileNames) {
        const std::filesystem::path filePath = directory / fileName;
        KernelHandle file(CreateFileW(
            filePath.c_str(), DELETE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!file || !IsOrdinaryFileHandle(file.get()) ||
            !MarkHandleForDeletion(file.get())) {
            return false;
        }
    }

    return MarkHandleForDeletion(directoryHandle.get());
}

#endif

}  // namespace

bool IsManagedWindowsUpdateDirectoryName(
    std::wstring_view name) noexcept {
    constexpr std::wstring_view kPrefix = L"update-";
    if (name.size() != kPrefix.size() + 32U ||
        name.substr(0U, kPrefix.size()) != kPrefix) {
        return false;
    }
    for (const wchar_t value : name.substr(kPrefix.size())) {
        if (!((value >= L'0' && value <= L'9') ||
              (value >= L'a' && value <= L'f'))) {
            return false;
        }
    }
    return true;
}

std::vector<std::wstring> SelectStaleWindowsUpdateDirectories(
    const std::vector<WindowsUpdateDirectoryCleanupEntry>& entries,
    std::uint64_t now100Nanoseconds) noexcept {
    try {
        std::vector<IndexedCleanupEntry> eligible;
        eligible.reserve(entries.size());
        for (const auto& entry : entries) {
            if (entry.isDirectory && !entry.isReparsePoint &&
                entry.lastWriteTime100Nanoseconds > 0U &&
                IsManagedWindowsUpdateDirectoryName(entry.name)) {
                eligible.push_back(
                    {entry.name, entry.lastWriteTime100Nanoseconds});
            }
        }
        std::sort(eligible.begin(), eligible.end(),
                  [](const auto& lhs, const auto& rhs) {
                      if (lhs.lastWriteTime100Nanoseconds !=
                          rhs.lastWriteTime100Nanoseconds) {
                          return lhs.lastWriteTime100Nanoseconds >
                                 rhs.lastWriteTime100Nanoseconds;
                      }
                      return lhs.name < rhs.name;
                  });

        std::vector<std::wstring> selected;
        for (std::size_t index = kMinimumRetainedUpdateDirectories;
             index < eligible.size(); ++index) {
            const auto& entry = eligible[index];
            if (now100Nanoseconds >=
                    entry.lastWriteTime100Nanoseconds &&
                now100Nanoseconds -
                        entry.lastWriteTime100Nanoseconds >=
                    kMinimumUpdateDirectoryAge100Nanoseconds) {
                selected.push_back(entry.name);
            }
        }
        return selected;
    } catch (...) {
        return {};
    }
}

void BestEffortCleanupWindowsUpdateDirectories(
    const std::filesystem::path& updatesRoot) noexcept {
#ifdef _WIN32
    try {
        if (!IsCanonicalFixedLocalDirectoryPath(updatesRoot)) return;
        KernelHandle root(CreateFileW(
            updatesRoot.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (!root || !IsOrdinaryDirectoryHandle(root.get())) return;

        FILETIME nowFileTime{};
        GetSystemTimeAsFileTime(&nowFileTime);
        const std::uint64_t now100Nanoseconds = FileTimeTicks(nowFileTime);
        if (now100Nanoseconds == 0U) return;

        std::vector<WindowsUpdateDirectoryCleanupEntry> entries;
        const std::filesystem::path wildcard = updatesRoot / L"*";
        {
            WIN32_FIND_DATAW data{};
            FindHandle find(FindFirstFileExW(
                wildcard.c_str(), FindExInfoBasic, &data,
                FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH));
            if (!find) return;

            std::size_t inspected = 0U;
            do {
                const std::wstring_view name(data.cFileName);
                if (name == L"." || name == L"..") continue;
                if (++inspected > kMaximumRootEntriesToInspect) return;
                if (!IsManagedWindowsUpdateDirectoryName(name)) continue;
                if (entries.size() >= kMaximumManagedDirectories) return;
                entries.push_back({
                    std::wstring(name), FileTimeTicks(data.ftLastWriteTime),
                    (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U,
                    (data.dwFileAttributes &
                     FILE_ATTRIBUTE_REPARSE_POINT) != 0U,
                });
            } while (FindNextFileW(find.get(), &data));
            if (GetLastError() != ERROR_NO_MORE_FILES) return;
        }

        const std::vector<std::wstring> selected =
            SelectStaleWindowsUpdateDirectories(entries,
                                                now100Nanoseconds);
        for (const std::wstring& name : selected) {
            (void)DeleteFlatOrdinaryUpdateDirectory(
                updatesRoot / name, now100Nanoseconds);
        }
    } catch (...) {
        // Cleanup is maintenance only. It must never block HUD startup or an
        // explicit update attempt.
    }
#else
    (void)updatesRoot;
#endif
}

}  // namespace codex_monitor::update
