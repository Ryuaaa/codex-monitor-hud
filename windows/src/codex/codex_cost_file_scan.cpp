#include "codex/codex_cost_file_scan.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace codex_monitor::codex {
namespace {

constexpr std::size_t kReadChunkBytes = 64 * 1024;
constexpr std::int64_t kNanosecondsPerSecond = 1'000'000'000LL;
constexpr std::int64_t kArchivedRetentionSeconds =
    static_cast<std::int64_t>(kCodexCostHistoryDays) * 24LL * 60LL * 60LL;

enum class PathObjectStatus {
    kOk,
    kMissing,
    kNotDirectory,
    kReparsePoint,
    kNotRegularFile,
    kIoError,
};

struct FileMetadata {
    std::string fileId;
    std::uint64_t sizeBytes = 0;
    std::int64_t modifiedUnixNanoseconds = 0;
};

struct CandidateFile {
    // This path is deliberately confined to the transient implementation and
    // is never copied into the public scan result or cursor.
    std::filesystem::path path;
    FileMetadata metadata;
};

std::error_code InvalidArgumentError() {
    return std::make_error_code(std::errc::invalid_argument);
}

std::int64_t SaturatingUnixNanoseconds(std::int64_t seconds,
                                       std::int64_t nanoseconds) noexcept {
    constexpr std::int64_t maximum =
        std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t minimum =
        std::numeric_limits<std::int64_t>::min();
    if (seconds > maximum / kNanosecondsPerSecond) return maximum;
    if (seconds < minimum / kNanosecondsPerSecond) return minimum;
    const std::int64_t base = seconds * kNanosecondsPerSecond;
    if (nanoseconds > 0 && base > maximum - nanoseconds) return maximum;
    if (nanoseconds < 0 && base < minimum - nanoseconds) return minimum;
    return base + nanoseconds;
}

std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](char value) {
        const unsigned char byte = static_cast<unsigned char>(value);
        return static_cast<char>(std::tolower(byte));
    });
    return value;
}

bool StartsWith(std::string_view value, std::string_view prefix) noexcept {
    return value.size() >= prefix.size() &&
           value.substr(0, prefix.size()) == prefix;
}

bool EndsWith(std::string_view value, std::string_view suffix) noexcept {
    return value.size() >= suffix.size() &&
           value.substr(value.size() - suffix.size()) == suffix;
}

enum class CandidateNameKind {
    kIgnored,
    kJsonLines,
    kCompressedJsonLines,
};

CandidateNameKind ClassifyCandidateName(
    const std::filesystem::path& filename,
    bool archived) {
    const std::string name = LowerAscii(filename.generic_u8string());
    if (!archived && !StartsWith(name, "rollout-")) {
        return CandidateNameKind::kIgnored;
    }
    if (EndsWith(name, ".jsonl.zst")) {
        return CandidateNameKind::kCompressedJsonLines;
    }
    return EndsWith(name, ".jsonl") ? CandidateNameKind::kJsonLines
                                     : CandidateNameKind::kIgnored;
}

bool IsLexicallyWithin(const std::filesystem::path& root,
                       const std::filesystem::path& candidate) {
    const std::filesystem::path relative =
        candidate.lexically_normal().lexically_relative(root.lexically_normal());
    if (relative.empty() || relative.is_absolute()) return false;
    for (const auto& component : relative) {
        if (component == std::filesystem::path("..")) return false;
    }
    return true;
}

#ifdef _WIN32

std::error_code LastWindowsError() {
    return std::error_code(static_cast<int>(GetLastError()),
                           std::system_category());
}

bool IsMissingWindowsError(DWORD error) noexcept {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

std::int64_t FileTimeToUnixNanoseconds(const FILETIME& value) noexcept {
    constexpr std::uint64_t kWindowsToUnixEpochTicks =
        116444736000000000ULL;
    const std::uint64_t ticks =
        (static_cast<std::uint64_t>(value.dwHighDateTime) << 32U) |
        static_cast<std::uint64_t>(value.dwLowDateTime);
    if (ticks >= kWindowsToUnixEpochTicks) {
        const std::uint64_t delta = ticks - kWindowsToUnixEpochTicks;
        const std::uint64_t maximum =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        if (delta > maximum / 100ULL) {
            return std::numeric_limits<std::int64_t>::max();
        }
        return static_cast<std::int64_t>(delta * 100ULL);
    }
    const std::uint64_t delta = kWindowsToUnixEpochTicks - ticks;
    const std::uint64_t negativeLimit =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) +
        1ULL;
    if (delta > negativeLimit / 100ULL) {
        return std::numeric_limits<std::int64_t>::min();
    }
    const std::uint64_t magnitude = delta * 100ULL;
    if (magnitude == negativeLimit) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return -static_cast<std::int64_t>(magnitude);
}

std::string WindowsFileId(const BY_HANDLE_FILE_INFORMATION& information) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "win-" << std::hex << std::setfill('0') << std::setw(8)
           << static_cast<std::uint64_t>(information.dwVolumeSerialNumber)
           << '-' << std::setw(8)
           << static_cast<std::uint64_t>(information.nFileIndexHigh)
           << std::setw(8)
           << static_cast<std::uint64_t>(information.nFileIndexLow);
    return output.str();
}

PathObjectStatus CheckDirectory(const std::filesystem::path& path,
                                std::error_code& error) {
    error.clear();
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD nativeError = GetLastError();
        if (IsMissingWindowsError(nativeError)) return PathObjectStatus::kMissing;
        error = std::error_code(static_cast<int>(nativeError),
                                std::system_category());
        return PathObjectStatus::kIoError;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return PathObjectStatus::kReparsePoint;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return PathObjectStatus::kNotDirectory;
    }
    return PathObjectStatus::kOk;
}

bool IsNetworkRoot(const std::filesystem::path& root) noexcept {
    const std::wstring native = root.native();
    if (native.size() < 3) return true;
    std::array<wchar_t, 32768> volumeRoot{};
    if (GetVolumePathNameW(native.c_str(), volumeRoot.data(),
                           static_cast<DWORD>(volumeRoot.size())) &&
        GetDriveTypeW(volumeRoot.data()) == DRIVE_REMOTE) {
        return true;
    }
    std::array<wchar_t, 4> driveRoot = {
        native[0], L':', L'\\', L'\0'};
    return GetDriveTypeW(driveRoot.data()) == DRIVE_REMOTE;
}

class NativeFile {
public:
    NativeFile() = default;
    NativeFile(const NativeFile&) = delete;
    NativeFile& operator=(const NativeFile&) = delete;

    NativeFile(NativeFile&& other) noexcept
        : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)),
          metadata_(std::move(other.metadata_)) {}

    NativeFile& operator=(NativeFile&& other) noexcept {
        if (this == &other) return *this;
        Close();
        handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        metadata_ = std::move(other.metadata_);
        return *this;
    }

    ~NativeFile() { Close(); }

    PathObjectStatus Open(const std::filesystem::path& path,
                          std::error_code& error) {
        Close();
        error.clear();
        handle_ = CreateFileW(
            path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            const DWORD nativeError = GetLastError();
            if (IsMissingWindowsError(nativeError)) {
                return PathObjectStatus::kMissing;
            }
            error = std::error_code(static_cast<int>(nativeError),
                                    std::system_category());
            return PathObjectStatus::kIoError;
        }

        BY_HANDLE_FILE_INFORMATION information{};
        if (!GetFileInformationByHandle(handle_, &information)) {
            error = LastWindowsError();
            Close();
            return PathObjectStatus::kIoError;
        }
        if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            Close();
            return PathObjectStatus::kReparsePoint;
        }
        if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            Close();
            return PathObjectStatus::kNotRegularFile;
        }

        metadata_.fileId = WindowsFileId(information);
        metadata_.sizeBytes =
            (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32U) |
            static_cast<std::uint64_t>(information.nFileSizeLow);
        metadata_.modifiedUnixNanoseconds =
            FileTimeToUnixNanoseconds(information.ftLastWriteTime);
        return PathObjectStatus::kOk;
    }

    bool Seek(std::uint64_t offset, std::error_code& error) noexcept {
        error.clear();
        if (offset >
            static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max())) {
            error = InvalidArgumentError();
            return false;
        }
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN)) {
            error = LastWindowsError();
            return false;
        }
        return true;
    }

    bool Read(char* buffer,
              std::size_t requested,
              std::size_t& bytesRead,
              std::error_code& error) noexcept {
        bytesRead = 0;
        error.clear();
        DWORD nativeBytesRead = 0;
        if (!ReadFile(handle_, buffer, static_cast<DWORD>(requested),
                      &nativeBytesRead, nullptr)) {
            error = LastWindowsError();
            return false;
        }
        bytesRead = static_cast<std::size_t>(nativeBytesRead);
        return true;
    }

    [[nodiscard]] const FileMetadata& metadata() const noexcept {
        return metadata_;
    }

private:
    void Close() noexcept {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    FileMetadata metadata_;
};

#else

std::int64_t PosixModifiedUnixNanoseconds(const struct stat& information) {
#if defined(__APPLE__)
    return SaturatingUnixNanoseconds(
        static_cast<std::int64_t>(information.st_mtimespec.tv_sec),
        static_cast<std::int64_t>(information.st_mtimespec.tv_nsec));
#else
    return SaturatingUnixNanoseconds(
        static_cast<std::int64_t>(information.st_mtim.tv_sec),
        static_cast<std::int64_t>(information.st_mtim.tv_nsec));
#endif
}

std::string PosixFileId(const struct stat& information) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "posix-" << std::hex
           << static_cast<std::uintmax_t>(information.st_dev) << '-'
           << static_cast<std::uintmax_t>(information.st_ino);
    return output.str();
}

PathObjectStatus CheckDirectory(const std::filesystem::path& path,
                                std::error_code& error) {
    error.clear();
    struct stat information {};
    if (::lstat(path.c_str(), &information) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return PathObjectStatus::kMissing;
        }
        error = std::error_code(errno, std::generic_category());
        return PathObjectStatus::kIoError;
    }
    if (S_ISLNK(information.st_mode)) {
        return PathObjectStatus::kReparsePoint;
    }
    if (!S_ISDIR(information.st_mode)) {
        return PathObjectStatus::kNotDirectory;
    }
    return PathObjectStatus::kOk;
}

class NativeFile {
public:
    NativeFile() = default;
    NativeFile(const NativeFile&) = delete;
    NativeFile& operator=(const NativeFile&) = delete;

    NativeFile(NativeFile&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)),
          metadata_(std::move(other.metadata_)) {}

    NativeFile& operator=(NativeFile&& other) noexcept {
        if (this == &other) return *this;
        Close();
        descriptor_ = std::exchange(other.descriptor_, -1);
        metadata_ = std::move(other.metadata_);
        return *this;
    }

    ~NativeFile() { Close(); }

    PathObjectStatus Open(const std::filesystem::path& path,
                          std::error_code& error) {
        Close();
        error.clear();
        int flags = O_RDONLY;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        descriptor_ = ::open(path.c_str(), flags);
        if (descriptor_ < 0) {
            if (errno == ENOENT || errno == ENOTDIR) {
                return PathObjectStatus::kMissing;
            }
            if (errno == ELOOP) return PathObjectStatus::kReparsePoint;
            error = std::error_code(errno, std::generic_category());
            return PathObjectStatus::kIoError;
        }

        struct stat information {};
        if (::fstat(descriptor_, &information) != 0) {
            error = std::error_code(errno, std::generic_category());
            Close();
            return PathObjectStatus::kIoError;
        }
        if (!S_ISREG(information.st_mode)) {
            Close();
            return PathObjectStatus::kNotRegularFile;
        }
        if (information.st_size < 0) {
            error = std::make_error_code(std::errc::value_too_large);
            Close();
            return PathObjectStatus::kIoError;
        }

        metadata_.fileId = PosixFileId(information);
        metadata_.sizeBytes = static_cast<std::uint64_t>(information.st_size);
        metadata_.modifiedUnixNanoseconds =
            PosixModifiedUnixNanoseconds(information);
        return PathObjectStatus::kOk;
    }

    bool Seek(std::uint64_t offset, std::error_code& error) noexcept {
        error.clear();
        if (offset >
            static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
            error = InvalidArgumentError();
            return false;
        }
        if (::lseek(descriptor_, static_cast<off_t>(offset), SEEK_SET) < 0) {
            error = std::error_code(errno, std::generic_category());
            return false;
        }
        return true;
    }

    bool Read(char* buffer,
              std::size_t requested,
              std::size_t& bytesRead,
              std::error_code& error) noexcept {
        bytesRead = 0;
        error.clear();
        const ssize_t result = ::read(descriptor_, buffer, requested);
        if (result < 0) {
            error = std::error_code(errno, std::generic_category());
            return false;
        }
        bytesRead = static_cast<std::size_t>(result);
        return true;
    }

    [[nodiscard]] const FileMetadata& metadata() const noexcept {
        return metadata_;
    }

private:
    void Close() noexcept {
        if (descriptor_ >= 0) ::close(descriptor_);
        descriptor_ = -1;
    }

    int descriptor_ = -1;
    FileMetadata metadata_;
};

#endif

struct CachedDirectoryStatus {
    PathObjectStatus status = PathObjectStatus::kIoError;
    std::error_code error;
};

using DirectoryStatusCache =
    std::map<std::filesystem::path, CachedDirectoryStatus>;

CachedDirectoryStatus DirectoryStatus(
    const std::filesystem::path& path,
    DirectoryStatusCache& cache) {
    const auto existing = cache.find(path);
    if (existing != cache.end()) return existing->second;
    CachedDirectoryStatus value;
    value.status = CheckDirectory(path, value.error);
    cache.emplace(path, value);
    return value;
}

bool IsSafeOptionalDirectory(const std::filesystem::path& path,
                             DirectoryStatusCache& cache,
                             CodexCostFileScanResult& result) {
    const bool firstCheck = cache.find(path) == cache.end();
    const CachedDirectoryStatus value = DirectoryStatus(path, cache);
    if (value.status == PathObjectStatus::kOk) return true;
    if (value.status == PathObjectStatus::kMissing) return false;
    if (firstCheck) {
        result.coverageIncomplete = true;
        if (value.status == PathObjectStatus::kIoError) {
            ++result.ioErrorCount;
            if (!result.error) result.error = value.error;
        } else {
            ++result.rejectedUnsafeEntries;
        }
    }
    return false;
}

void RecordCandidateOpenFailure(PathObjectStatus status,
                                const std::error_code& error,
                                CodexCostFileScanResult& result) {
    if (status == PathObjectStatus::kMissing) return;
    result.coverageIncomplete = true;
    if (status == PathObjectStatus::kIoError) {
        ++result.ioErrorCount;
        if (!result.error) result.error = error;
    } else {
        ++result.rejectedUnsafeEntries;
    }
}

void InspectDirectDirectory(const std::filesystem::path& root,
                            const std::filesystem::path& directory,
                            bool archived,
                            std::int64_t archivedCutoffNanoseconds,
                            std::vector<CandidateFile>& candidates,
                            std::unordered_set<std::string>& seenFileIds,
                            CodexCostFileScanResult& result) {
    std::error_code iterationError;
    std::filesystem::directory_iterator iterator(
        directory, std::filesystem::directory_options::none, iterationError);
    if (iterationError) {
        result.coverageIncomplete = true;
        ++result.ioErrorCount;
        if (!result.error) result.error = iterationError;
        return;
    }
    const std::filesystem::directory_iterator end;
    while (iterator != end) {
        const std::filesystem::path filename = iterator->path().filename();
        const CandidateNameKind kind = ClassifyCandidateName(filename, archived);
        if (kind != CandidateNameKind::kIgnored) {
            const std::filesystem::path candidatePath = directory / filename;
            if (!IsLexicallyWithin(root, candidatePath)) {
                result.coverageIncomplete = true;
                ++result.rejectedUnsafeEntries;
            } else {
                NativeFile file;
                std::error_code openError;
                const PathObjectStatus openStatus =
                    file.Open(candidatePath, openError);
                if (openStatus != PathObjectStatus::kOk) {
                    RecordCandidateOpenFailure(openStatus, openError, result);
                } else if (kind == CandidateNameKind::kCompressedJsonLines) {
                    ++result.skippedCompressedFiles;
                    result.coverageIncomplete = true;
                } else {
                    const FileMetadata metadata = file.metadata();
                    if (archived && metadata.modifiedUnixNanoseconds <
                                        archivedCutoffNanoseconds) {
                        ++result.ignoredExpiredArchivedFiles;
                    } else if (seenFileIds.insert(metadata.fileId).second) {
                        candidates.push_back({candidatePath, metadata});
                    }
                }
            }
        }

        iterator.increment(iterationError);
        if (iterationError) {
            result.coverageIncomplete = true;
            ++result.ioErrorCount;
            if (!result.error) result.error = iterationError;
            return;
        }
    }
}

void DiscoverCandidates(const std::filesystem::path& root,
                        const std::vector<std::filesystem::path>& datePaths,
                        std::int64_t archivedCutoffNanoseconds,
                        std::vector<CandidateFile>& candidates,
                        CodexCostFileScanResult& result) {
    DirectoryStatusCache directories;
    std::unordered_set<std::string> seenFileIds;

    const std::filesystem::path sessions = root / "sessions";
    if (IsSafeOptionalDirectory(sessions, directories, result)) {
        for (const auto& relativeDate : datePaths) {
            std::filesystem::path current = sessions;
            bool safe = true;
            for (const auto& component : relativeDate) {
                current /= component;
                if (!IsSafeOptionalDirectory(current, directories, result)) {
                    safe = false;
                    break;
                }
            }
            if (safe) {
                InspectDirectDirectory(root, current, false,
                                       archivedCutoffNanoseconds, candidates,
                                       seenFileIds, result);
            }
        }
    }

    const std::filesystem::path archived = root / "archived_sessions";
    if (IsSafeOptionalDirectory(archived, directories, result)) {
        InspectDirectDirectory(root, archived, true,
                               archivedCutoffNanoseconds, candidates,
                               seenFileIds, result);
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const CandidateFile& left, const CandidateFile& right) {
                  if (left.metadata.modifiedUnixNanoseconds !=
                      right.metadata.modifiedUnixNanoseconds) {
                      return left.metadata.modifiedUnixNanoseconds >
                             right.metadata.modifiedUnixNanoseconds;
                  }
                  return left.metadata.fileId < right.metadata.fileId;
              });
}

const CodexCostFileCursor* FindPreviousCursor(
    const std::unordered_map<std::string, const CodexCostFileCursor*>& previous,
    const std::string& fileId) {
    const auto found = previous.find(fileId);
    return found == previous.end() ? nullptr : found->second;
}

void ReadCandidate(
    const CandidateFile& candidate,
    const std::unordered_map<std::string, const CodexCostFileCursor*>& previous,
    std::unordered_set<std::string>& processed,
    std::uint64_t& remainingBudget,
    std::size_t maximumLineBytes,
    CodexCostFileScanResult& result) {
    NativeFile file;
    std::error_code openError;
    const PathObjectStatus openStatus = file.Open(candidate.path, openError);
    if (openStatus != PathObjectStatus::kOk) {
        RecordCandidateOpenFailure(openStatus, openError, result);
        return;
    }

    const FileMetadata metadata = file.metadata();
    if (!processed.insert(metadata.fileId).second) return;
    const CodexCostFileCursor* old = FindPreviousCursor(previous, metadata.fileId);
    CodexCostFileCursor cursor;
    cursor.fileId = metadata.fileId;
    cursor.observedSizeBytes = metadata.sizeBytes;
    cursor.modifiedUnixNanoseconds = metadata.modifiedUnixNanoseconds;

    bool resetForChange = false;
    if (old) {
        const bool truncated =
            metadata.sizeBytes < old->observedSizeBytes ||
            old->parsedOffsetBytes > metadata.sizeBytes;
        const bool sameSizeRewrite =
            metadata.sizeBytes == old->observedSizeBytes &&
            metadata.modifiedUnixNanoseconds != old->modifiedUnixNanoseconds;
        resetForChange = truncated || sameSizeRewrite;
        // Both truncation and an in-place same-size rewrite invalidate the
        // parser watermark and compacted events retained by the caller.
        cursor.resetAfterTruncation = resetForChange;
        if (!resetForChange) {
            cursor.parsedOffsetBytes = old->parsedOffsetBytes;
            cursor.discardingOversizedLine =
                old->discardingOversizedLine;
            cursor.hasSkippedOversizedLine = old->hasSkippedOversizedLine;
        }
    }

    if (cursor.hasSkippedOversizedLine) result.coverageIncomplete = true;

    if (cursor.parsedOffsetBytes == metadata.sizeBytes) {
        cursor.complete = true;
        result.files.push_back(std::move(cursor));
        return;
    }

    if (remainingBudget == 0) {
        cursor.complete = false;
        result.coverageIncomplete = true;
        result.budgetExhausted = true;
        result.files.push_back(std::move(cursor));
        return;
    }

    std::error_code seekError;
    if (!file.Seek(cursor.parsedOffsetBytes, seekError)) {
        result.coverageIncomplete = true;
        ++result.ioErrorCount;
        if (!result.error) result.error = seekError;
        result.files.push_back(std::move(cursor));
        return;
    }

    const std::uint64_t startOffset = cursor.parsedOffsetBytes;
    std::uint64_t readPosition = startOffset;
    std::uint64_t committedOffset = startOffset;
    std::uint64_t lineStartOffset = startOffset;
    bool discarding = cursor.discardingOversizedLine;
    std::string lineBuffer;
    lineBuffer.reserve(std::min<std::size_t>(maximumLineBytes, 64 * 1024));
    std::array<char, kReadChunkBytes> buffer{};
    bool readFailed = false;

    while (remainingBudget > 0 && readPosition < metadata.sizeBytes) {
        const std::uint64_t fileRemaining = metadata.sizeBytes - readPosition;
        const std::size_t requestBytes = static_cast<std::size_t>(
            std::min<std::uint64_t>(
                {static_cast<std::uint64_t>(buffer.size()), remainingBudget,
                 fileRemaining}));
        std::size_t nativeBytesRead = 0;
        std::error_code readError;
        if (!file.Read(buffer.data(), requestBytes, nativeBytesRead, readError)) {
            result.coverageIncomplete = true;
            ++result.ioErrorCount;
            if (!result.error) result.error = readError;
            readFailed = true;
            break;
        }
        if (nativeBytesRead == 0) {
            result.coverageIncomplete = true;
            ++result.ioErrorCount;
            if (!result.error) {
                result.error = std::make_error_code(std::errc::io_error);
            }
            readFailed = true;
            break;
        }

        result.bytesRead += nativeBytesRead;
        remainingBudget -= nativeBytesRead;
        for (std::size_t index = 0; index < nativeBytesRead; ++index) {
            const char value = buffer[index];
            const std::uint64_t absoluteOffset =
                readPosition + static_cast<std::uint64_t>(index);
            if (value == '\n') {
                const std::uint64_t endOffset = absoluteOffset + 1;
                if (!discarding) {
                    if (!lineBuffer.empty() && lineBuffer.back() == '\r') {
                        lineBuffer.pop_back();
                    }
                    result.lines.push_back({metadata.fileId, lineStartOffset,
                                            endOffset,
                                            std::move(lineBuffer)});
                    lineBuffer.clear();
                }
                committedOffset = endOffset;
                lineStartOffset = endOffset;
                discarding = false;
                continue;
            }

            if (!discarding) {
                if (lineBuffer.size() < maximumLineBytes) {
                    lineBuffer.push_back(value);
                } else {
                    lineBuffer.clear();
                    discarding = true;
                    cursor.hasSkippedOversizedLine = true;
                    ++result.skippedOversizedLines;
                    result.coverageIncomplete = true;
                }
            }
        }
        readPosition += nativeBytesRead;
    }

    if (discarding) {
        // Progress through a malicious or damaged long line without retaining
        // its content. The flag ensures the next pass keeps discarding until a
        // newline is reached.
        cursor.parsedOffsetBytes = readPosition;
        cursor.discardingOversizedLine = true;
    } else {
        // A normal partial line is deliberately re-read next time. Its maximum
        // re-read cost is bounded by maximumLineBytes.
        cursor.parsedOffsetBytes = committedOffset;
        cursor.discardingOversizedLine = false;
    }

    cursor.complete = cursor.parsedOffsetBytes == metadata.sizeBytes;
    if (!cursor.complete || readFailed || cursor.hasSkippedOversizedLine) {
        result.coverageIncomplete = true;
    }
    if (remainingBudget == 0 && !cursor.complete) {
        result.budgetExhausted = true;
    }
    result.files.push_back(std::move(cursor));
}

}  // namespace

bool IsSafeAbsoluteWindowsLocalPath(std::wstring_view path) noexcept {
    if (path.size() < 3 || path.find(L'\0') != std::wstring_view::npos) {
        return false;
    }
    if ((path[0] == L'\\' || path[0] == L'/') &&
        (path[1] == L'\\' || path[1] == L'/')) {
        return false;
    }
    const wchar_t drive = path[0];
    const bool asciiLetter =
        (drive >= L'A' && drive <= L'Z') ||
        (drive >= L'a' && drive <= L'z');
    if (!asciiLetter || path[1] != L':' ||
        (path[2] != L'\\' && path[2] != L'/')) {
        return false;
    }
    return path.find(L':', 2) == std::wstring_view::npos;
}

std::vector<std::filesystem::path> RecentLocalCodexSessionDatePaths(
    std::int64_t nowUnixSeconds,
    std::size_t dayCount) noexcept {
    try {
        std::vector<std::filesystem::path> result;
        dayCount = std::min(dayCount, kCodexCostHistoryDays);
        if (dayCount == 0) return result;

        const std::time_t rawTime = static_cast<std::time_t>(nowUnixSeconds);
        if (static_cast<std::int64_t>(rawTime) != nowUnixSeconds) return result;
        std::tm local{};
#ifdef _WIN32
        if (localtime_s(&local, &rawTime) != 0) return result;
#else
        if (localtime_r(&rawTime, &local) == nullptr) return result;
#endif

        result.reserve(dayCount);
        for (std::size_t offset = 0; offset < dayCount; ++offset) {
            std::tm day = local;
            day.tm_isdst = -1;
            day.tm_mday -= static_cast<int>(offset);
            if (std::mktime(&day) == static_cast<std::time_t>(-1)) break;
            std::array<char, 11> formatted{};
            const int length = std::snprintf(
                formatted.data(), formatted.size(), "%04d/%02d/%02d",
                day.tm_year + 1900, day.tm_mon + 1, day.tm_mday);
            if (length != 10) break;
            result.emplace_back(formatted.data());
        }
        return result;
    } catch (...) {
        return {};
    }
}

CodexCostFileScanResult ScanCodexCostRolloutFiles(
    const CodexCostFileScanRequest& request) noexcept {
    CodexCostFileScanResult result;
    try {
        if (request.codexHome.empty() || request.nowUnixSeconds < 0 ||
            request.maximumLineBytes == 0) {
            result.status = CodexCostFileScanStatus::kInvalidArgument;
            result.error = InvalidArgumentError();
            return result;
        }

        std::filesystem::path root = request.codexHome.lexically_normal();
#ifdef _WIN32
        if (!IsSafeAbsoluteWindowsLocalPath(root.native()) ||
            IsNetworkRoot(root)) {
            result.status = CodexCostFileScanStatus::kUnsafeRoot;
            result.error = std::make_error_code(std::errc::permission_denied);
            return result;
        }
#else
        const std::string nativeRoot = root.native();
        if (!root.is_absolute() || StartsWith(nativeRoot, "//") ||
            nativeRoot.find('\0') != std::string::npos) {
            result.status = CodexCostFileScanStatus::kUnsafeRoot;
            result.error = std::make_error_code(std::errc::permission_denied);
            return result;
        }
#endif

        std::error_code rootError;
        const PathObjectStatus rootStatus = CheckDirectory(root, rootError);
        if (rootStatus != PathObjectStatus::kOk) {
            result.error = rootError;
            switch (rootStatus) {
                case PathObjectStatus::kMissing:
                    result.status = CodexCostFileScanStatus::kRootNotFound;
                    break;
                case PathObjectStatus::kNotDirectory:
                case PathObjectStatus::kNotRegularFile:
                    result.status = CodexCostFileScanStatus::kRootNotDirectory;
                    break;
                case PathObjectStatus::kReparsePoint:
                    result.status = CodexCostFileScanStatus::kUnsafeRoot;
                    result.error =
                        std::make_error_code(std::errc::permission_denied);
                    break;
                case PathObjectStatus::kIoError:
                    result.status = CodexCostFileScanStatus::kIoError;
                    break;
                case PathObjectStatus::kOk:
                    break;
            }
            return result;
        }

        const auto datePaths = RecentLocalCodexSessionDatePaths(
            request.nowUnixSeconds, kCodexCostHistoryDays);
        if (datePaths.size() != kCodexCostHistoryDays) {
            result.status = CodexCostFileScanStatus::kInvalidArgument;
            result.error = InvalidArgumentError();
            return result;
        }

        const std::int64_t cutoffSeconds =
            request.nowUnixSeconds >= kArchivedRetentionSeconds
                ? request.nowUnixSeconds - kArchivedRetentionSeconds
                : 0;
        const std::int64_t cutoffNanoseconds =
            SaturatingUnixNanoseconds(cutoffSeconds, 0);
        std::vector<CandidateFile> candidates;
        DiscoverCandidates(root, datePaths, cutoffNanoseconds, candidates,
                           result);

        std::unordered_map<std::string, const CodexCostFileCursor*> previous;
        previous.reserve(request.previousFiles.size());
        for (const auto& file : request.previousFiles) {
            if (!file.fileId.empty()) previous.emplace(file.fileId, &file);
        }

        std::uint64_t remainingBudget = std::min(
            request.byteBudgetBytes, kCodexCostMaximumScanBytes);
        const std::size_t maximumLineBytes = std::min(
            request.maximumLineBytes, kCodexCostMaximumLineBytes);
        std::unordered_set<std::string> processed;
        for (const auto& candidate : candidates) {
            ReadCandidate(candidate, previous, processed, remainingBudget,
                          maximumLineBytes, result);
        }

        for (const auto& file : result.files) {
            if (!file.complete || file.hasSkippedOversizedLine) {
                result.coverageIncomplete = true;
            }
        }
        result.status = CodexCostFileScanStatus::kOk;
        return result;
    } catch (const std::filesystem::filesystem_error& error) {
        result.status = CodexCostFileScanStatus::kIoError;
        result.error = error.code();
        return result;
    } catch (...) {
        result.status = CodexCostFileScanStatus::kIoError;
        result.error = std::make_error_code(std::errc::io_error);
        return result;
    }
}

}  // namespace codex_monitor::codex
