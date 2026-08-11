#include "codex/codex_activity_scan.h"

#include "codex/codex_cost_file_scan.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <limits>
#include <optional>
#include <string>
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

constexpr std::size_t kMaximumJsonDepth = 64;
constexpr std::size_t kMaximumRetainedJsonStringBytes = 128;
constexpr std::size_t kMaximumDirectoryEntries = 4096;

struct JsonCursor {
    std::string_view input;
    std::size_t position = 0;

    void SkipWhitespace() noexcept {
        while (position < input.size()) {
            const char value = input[position];
            if (value != ' ' && value != '\t' && value != '\r' &&
                value != '\n') {
                break;
            }
            ++position;
        }
    }

    [[nodiscard]] bool Consume(char expected) noexcept {
        SkipWhitespace();
        if (position >= input.size() || input[position] != expected) {
            return false;
        }
        ++position;
        return true;
    }
};

int HexDigit(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool ParseHexCodeUnit(JsonCursor& cursor, std::uint32_t& output) noexcept {
    if (cursor.position + 4 > cursor.input.size()) return false;
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index) {
        const int digit = HexDigit(cursor.input[cursor.position++]);
        if (digit < 0) return false;
        value = value * 16U + static_cast<std::uint32_t>(digit);
    }
    output = value;
    return true;
}

void AppendUtf8(std::uint32_t value,
                std::string& output,
                bool& overflow) {
    std::array<char, 4> bytes{};
    std::size_t count = 0;
    if (value <= 0x7fU) {
        bytes[count++] = static_cast<char>(value);
    } else if (value <= 0x7ffU) {
        bytes[count++] = static_cast<char>(0xc0U | (value >> 6U));
        bytes[count++] = static_cast<char>(0x80U | (value & 0x3fU));
    } else if (value <= 0xffffU) {
        bytes[count++] = static_cast<char>(0xe0U | (value >> 12U));
        bytes[count++] =
            static_cast<char>(0x80U | ((value >> 6U) & 0x3fU));
        bytes[count++] = static_cast<char>(0x80U | (value & 0x3fU));
    } else {
        bytes[count++] = static_cast<char>(0xf0U | (value >> 18U));
        bytes[count++] =
            static_cast<char>(0x80U | ((value >> 12U) & 0x3fU));
        bytes[count++] =
            static_cast<char>(0x80U | ((value >> 6U) & 0x3fU));
        bytes[count++] = static_cast<char>(0x80U | (value & 0x3fU));
    }
    if (output.size() + count > kMaximumRetainedJsonStringBytes) {
        overflow = true;
        return;
    }
    output.append(bytes.data(), count);
}

bool ParseJsonString(JsonCursor& cursor,
                     std::string* output,
                     bool* outputOverflow = nullptr) {
    cursor.SkipWhitespace();
    if (cursor.position >= cursor.input.size() ||
        cursor.input[cursor.position] != '"') {
        return false;
    }
    ++cursor.position;
    if (output) output->clear();
    bool overflow = false;

    const auto appendByte = [&](char value) {
        if (!output) return;
        if (output->size() < kMaximumRetainedJsonStringBytes) {
            output->push_back(value);
        } else {
            overflow = true;
        }
    };
    while (cursor.position < cursor.input.size()) {
        const unsigned char value =
            static_cast<unsigned char>(cursor.input[cursor.position++]);
        if (value == '"') {
            if (outputOverflow) *outputOverflow = overflow;
            return true;
        }
        if (value < 0x20U) return false;
        if (value != '\\') {
            appendByte(static_cast<char>(value));
            continue;
        }
        if (cursor.position >= cursor.input.size()) return false;
        const char escape = cursor.input[cursor.position++];
        switch (escape) {
            case '"':
            case '\\':
            case '/':
                appendByte(escape);
                break;
            case 'b':
                appendByte('\b');
                break;
            case 'f':
                appendByte('\f');
                break;
            case 'n':
                appendByte('\n');
                break;
            case 'r':
                appendByte('\r');
                break;
            case 't':
                appendByte('\t');
                break;
            case 'u': {
                std::uint32_t first = 0;
                if (!ParseHexCodeUnit(cursor, first)) return false;
                std::uint32_t scalar = first;
                if (first >= 0xd800U && first <= 0xdbffU) {
                    if (cursor.position + 2 > cursor.input.size() ||
                        cursor.input[cursor.position] != '\\' ||
                        cursor.input[cursor.position + 1] != 'u') {
                        return false;
                    }
                    cursor.position += 2;
                    std::uint32_t second = 0;
                    if (!ParseHexCodeUnit(cursor, second) ||
                        second < 0xdc00U || second > 0xdfffU) {
                        return false;
                    }
                    scalar = 0x10000U + ((first - 0xd800U) << 10U) +
                             (second - 0xdc00U);
                } else if (first >= 0xdc00U && first <= 0xdfffU) {
                    return false;
                }
                if (output) AppendUtf8(scalar, *output, overflow);
                break;
            }
            default:
                return false;
        }
    }
    return false;
}

bool SkipJsonValue(JsonCursor& cursor, std::size_t depth);

bool ConsumeLiteral(JsonCursor& cursor, std::string_view literal) noexcept {
    if (cursor.input.substr(cursor.position, literal.size()) != literal) {
        return false;
    }
    cursor.position += literal.size();
    return true;
}

bool SkipJsonNumber(JsonCursor& cursor) noexcept {
    cursor.SkipWhitespace();
    if (cursor.position < cursor.input.size() &&
        cursor.input[cursor.position] == '-') {
        ++cursor.position;
    }
    if (cursor.position >= cursor.input.size()) return false;
    if (cursor.input[cursor.position] == '0') {
        ++cursor.position;
        if (cursor.position < cursor.input.size() &&
            std::isdigit(static_cast<unsigned char>(
                cursor.input[cursor.position]))) {
            return false;
        }
    } else if (cursor.input[cursor.position] >= '1' &&
               cursor.input[cursor.position] <= '9') {
        while (cursor.position < cursor.input.size() &&
               std::isdigit(static_cast<unsigned char>(
                   cursor.input[cursor.position]))) {
            ++cursor.position;
        }
    } else {
        return false;
    }
    if (cursor.position < cursor.input.size() &&
        cursor.input[cursor.position] == '.') {
        ++cursor.position;
        const std::size_t begin = cursor.position;
        while (cursor.position < cursor.input.size() &&
               std::isdigit(static_cast<unsigned char>(
                   cursor.input[cursor.position]))) {
            ++cursor.position;
        }
        if (cursor.position == begin) return false;
    }
    if (cursor.position < cursor.input.size() &&
        (cursor.input[cursor.position] == 'e' ||
         cursor.input[cursor.position] == 'E')) {
        ++cursor.position;
        if (cursor.position < cursor.input.size() &&
            (cursor.input[cursor.position] == '+' ||
             cursor.input[cursor.position] == '-')) {
            ++cursor.position;
        }
        const std::size_t begin = cursor.position;
        while (cursor.position < cursor.input.size() &&
               std::isdigit(static_cast<unsigned char>(
                   cursor.input[cursor.position]))) {
            ++cursor.position;
        }
        if (cursor.position == begin) return false;
    }
    return true;
}

bool SkipJsonObject(JsonCursor& cursor, std::size_t depth) {
    if (depth > kMaximumJsonDepth || !cursor.Consume('{')) return false;
    cursor.SkipWhitespace();
    if (cursor.position < cursor.input.size() &&
        cursor.input[cursor.position] == '}') {
        ++cursor.position;
        return true;
    }
    while (true) {
        if (!ParseJsonString(cursor, nullptr) || !cursor.Consume(':') ||
            !SkipJsonValue(cursor, depth + 1)) {
            return false;
        }
        cursor.SkipWhitespace();
        if (cursor.position >= cursor.input.size()) return false;
        const char separator = cursor.input[cursor.position++];
        if (separator == '}') return true;
        if (separator != ',') return false;
    }
}

bool SkipJsonArray(JsonCursor& cursor, std::size_t depth) {
    if (depth > kMaximumJsonDepth || !cursor.Consume('[')) return false;
    cursor.SkipWhitespace();
    if (cursor.position < cursor.input.size() &&
        cursor.input[cursor.position] == ']') {
        ++cursor.position;
        return true;
    }
    while (true) {
        if (!SkipJsonValue(cursor, depth + 1)) return false;
        cursor.SkipWhitespace();
        if (cursor.position >= cursor.input.size()) return false;
        const char separator = cursor.input[cursor.position++];
        if (separator == ']') return true;
        if (separator != ',') return false;
    }
}

bool SkipJsonValue(JsonCursor& cursor, std::size_t depth) {
    if (depth > kMaximumJsonDepth) return false;
    cursor.SkipWhitespace();
    if (cursor.position >= cursor.input.size()) return false;
    switch (cursor.input[cursor.position]) {
        case '{':
            return SkipJsonObject(cursor, depth);
        case '[':
            return SkipJsonArray(cursor, depth);
        case '"':
            return ParseJsonString(cursor, nullptr);
        case 't':
            return ConsumeLiteral(cursor, "true");
        case 'f':
            return ConsumeLiteral(cursor, "false");
        case 'n':
            return ConsumeLiteral(cursor, "null");
        default:
            return SkipJsonNumber(cursor);
    }
}

struct ActivityPayloadFields {
    std::optional<std::string> type;
    std::optional<std::string> role;
    std::optional<std::string> phase;
};

struct ActivityRootFields {
    std::optional<std::string> timestamp;
    std::optional<std::string> type;
    std::optional<ActivityPayloadFields> payload;
};

bool ParseOptionalBoundedString(JsonCursor& cursor,
                                std::optional<std::string>& output) {
    output.reset();
    cursor.SkipWhitespace();
    if (cursor.position < cursor.input.size() &&
        cursor.input[cursor.position] == '"') {
        std::string value;
        bool overflow = false;
        if (!ParseJsonString(cursor, &value, &overflow)) return false;
        if (!overflow) output = std::move(value);
        return true;
    }
    return SkipJsonValue(cursor, 1);
}

bool ParsePayloadObject(JsonCursor& cursor, ActivityPayloadFields& output) {
    if (!cursor.Consume('{')) return false;
    cursor.SkipWhitespace();
    if (cursor.position < cursor.input.size() &&
        cursor.input[cursor.position] == '}') {
        ++cursor.position;
        return true;
    }
    while (true) {
        std::string key;
        bool keyOverflow = false;
        if (!ParseJsonString(cursor, &key, &keyOverflow) ||
            !cursor.Consume(':')) {
            return false;
        }
        bool parsed = true;
        if (!keyOverflow && key == "type") {
            parsed = ParseOptionalBoundedString(cursor, output.type);
        } else if (!keyOverflow && key == "role") {
            parsed = ParseOptionalBoundedString(cursor, output.role);
        } else if (!keyOverflow && key == "phase") {
            parsed = ParseOptionalBoundedString(cursor, output.phase);
        } else {
            parsed = SkipJsonValue(cursor, 1);
        }
        if (!parsed) return false;
        cursor.SkipWhitespace();
        if (cursor.position >= cursor.input.size()) return false;
        const char separator = cursor.input[cursor.position++];
        if (separator == '}') return true;
        if (separator != ',') return false;
    }
}

bool ParseOptionalPayload(JsonCursor& cursor,
                          std::optional<ActivityPayloadFields>& output) {
    output.reset();
    cursor.SkipWhitespace();
    if (cursor.position < cursor.input.size() &&
        cursor.input[cursor.position] == '{') {
        ActivityPayloadFields value;
        if (!ParsePayloadObject(cursor, value)) return false;
        output = std::move(value);
        return true;
    }
    return SkipJsonValue(cursor, 0);
}

bool ParseActivityRoot(std::string_view line, ActivityRootFields& output) {
    JsonCursor cursor{line};
    if (!cursor.Consume('{')) return false;
    cursor.SkipWhitespace();
    if (cursor.position < cursor.input.size() &&
        cursor.input[cursor.position] == '}') {
        ++cursor.position;
        cursor.SkipWhitespace();
        return cursor.position == cursor.input.size();
    }
    while (true) {
        std::string key;
        bool keyOverflow = false;
        if (!ParseJsonString(cursor, &key, &keyOverflow) ||
            !cursor.Consume(':')) {
            return false;
        }
        bool parsed = true;
        if (!keyOverflow && key == "timestamp") {
            parsed = ParseOptionalBoundedString(cursor, output.timestamp);
        } else if (!keyOverflow && key == "type") {
            parsed = ParseOptionalBoundedString(cursor, output.type);
        } else if (!keyOverflow && key == "payload") {
            parsed = ParseOptionalPayload(cursor, output.payload);
        } else {
            parsed = SkipJsonValue(cursor, 0);
        }
        if (!parsed) return false;
        cursor.SkipWhitespace();
        if (cursor.position >= cursor.input.size()) return false;
        const char separator = cursor.input[cursor.position++];
        if (separator == '}') break;
        if (separator != ',') return false;
    }
    cursor.SkipWhitespace();
    return cursor.position == cursor.input.size();
}

bool IsLeapYear(int year) noexcept {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int DaysInMonth(int year, int month) noexcept {
    static constexpr std::array<int, 13> kDays = {
        0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    return month == 2 && IsLeapYear(year)
               ? 29
               : kDays[static_cast<std::size_t>(month)];
}

bool ParseDigits(std::string_view text,
                 std::size_t& position,
                 std::size_t count,
                 int& output) noexcept {
    if (position + count > text.size()) return false;
    int value = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const char digit = text[position++];
        if (digit < '0' || digit > '9') return false;
        value = value * 10 + (digit - '0');
    }
    output = value;
    return true;
}

std::int64_t DaysFromCivil(int year,
                           unsigned month,
                           unsigned day) noexcept {
    year -= month <= 2U;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
    const unsigned adjustedMonth = month > 2U ? month - 3U : month + 9U;
    const unsigned dayOfYear =
        (153U * adjustedMonth + 2U) / 5U + day - 1U;
    const unsigned dayOfEra = yearOfEra * 365U + yearOfEra / 4U -
                              yearOfEra / 100U + dayOfYear;
    return static_cast<std::int64_t>(era) * 146097LL +
           static_cast<std::int64_t>(dayOfEra) - 719468LL;
}

std::optional<std::int64_t> ParseIso8601Milliseconds(
    std::string_view text) noexcept {
    std::size_t position = 0;
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!ParseDigits(text, position, 4, year) ||
        position >= text.size() || text[position++] != '-' ||
        !ParseDigits(text, position, 2, month) ||
        position >= text.size() || text[position++] != '-' ||
        !ParseDigits(text, position, 2, day) ||
        position >= text.size() ||
        (text[position] != 'T' && text[position] != 't')) {
        return std::nullopt;
    }
    ++position;
    if (!ParseDigits(text, position, 2, hour) ||
        position >= text.size() || text[position++] != ':' ||
        !ParseDigits(text, position, 2, minute) ||
        position >= text.size() || text[position++] != ':' ||
        !ParseDigits(text, position, 2, second) || year < 1 || month < 1 ||
        month > 12 || day < 1 || day > DaysInMonth(year, month) ||
        hour > 23 || minute > 59 || second > 59) {
        return std::nullopt;
    }
    int milliseconds = 0;
    if (position < text.size() && text[position] == '.') {
        ++position;
        const std::size_t fractionStart = position;
        int digits = 0;
        while (position < text.size() && text[position] >= '0' &&
               text[position] <= '9') {
            if (digits < 3) {
                milliseconds = milliseconds * 10 + (text[position] - '0');
            }
            ++digits;
            ++position;
        }
        if (position == fractionStart) return std::nullopt;
        while (digits < 3) {
            milliseconds *= 10;
            ++digits;
        }
    }
    int offsetSeconds = 0;
    if (position < text.size() &&
        (text[position] == 'Z' || text[position] == 'z')) {
        ++position;
    } else if (position < text.size() &&
               (text[position] == '+' || text[position] == '-')) {
        const bool negative = text[position++] == '-';
        int offsetHour = 0;
        int offsetMinute = 0;
        if (!ParseDigits(text, position, 2, offsetHour) ||
            position >= text.size() || text[position++] != ':' ||
            !ParseDigits(text, position, 2, offsetMinute) ||
            offsetHour > 23 || offsetMinute > 59) {
            return std::nullopt;
        }
        offsetSeconds = offsetHour * 3600 + offsetMinute * 60;
        if (negative) offsetSeconds = -offsetSeconds;
    } else {
        return std::nullopt;
    }
    if (position != text.size()) return std::nullopt;
    const std::int64_t seconds =
        DaysFromCivil(year, static_cast<unsigned>(month),
                      static_cast<unsigned>(day)) *
            86400LL +
        static_cast<std::int64_t>(hour) * 3600LL +
        static_cast<std::int64_t>(minute) * 60LL + second - offsetSeconds;
    return seconds * 1000LL + milliseconds;
}

bool StartsWithInsensitive(std::string_view value,
                           std::string_view prefix) noexcept {
    if (value.size() < prefix.size()) return false;
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(value[index])) !=
            std::tolower(static_cast<unsigned char>(prefix[index]))) {
            return false;
        }
    }
    return true;
}

bool EndsWithInsensitive(std::string_view value,
                         std::string_view suffix) noexcept {
    if (value.size() < suffix.size()) return false;
    return StartsWithInsensitive(value.substr(value.size() - suffix.size()),
                                 suffix);
}

enum class CandidateKind {
    kIgnored,
    kPlain,
    kCompressed,
};

CandidateKind ClassifyCandidate(const std::filesystem::path& filename) {
    const std::string name = filename.generic_u8string();
    if (!StartsWithInsensitive(name, "rollout-")) {
        return CandidateKind::kIgnored;
    }
    if (EndsWithInsensitive(name, ".jsonl.zst")) {
        return CandidateKind::kCompressed;
    }
    return EndsWithInsensitive(name, ".jsonl")
               ? CandidateKind::kPlain
               : CandidateKind::kIgnored;
}

struct NativeMetadata {
    bool regular = false;
    bool unsafeLink = false;
    std::uint64_t sizeBytes = 0;
    std::int64_t modifiedUnixMilliseconds = 0;
    std::error_code error;
};

#ifdef _WIN32

std::error_code LastWindowsError() {
    return std::error_code(static_cast<int>(GetLastError()),
                           std::system_category());
}

bool MissingWindowsError(DWORD error) noexcept {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

std::int64_t FileTimeToUnixMilliseconds(const FILETIME& value) noexcept {
    constexpr std::uint64_t kWindowsToUnixEpochTicks =
        116444736000000000ULL;
    const std::uint64_t ticks =
        (static_cast<std::uint64_t>(value.dwHighDateTime) << 32U) |
        static_cast<std::uint64_t>(value.dwLowDateTime);
    if (ticks < kWindowsToUnixEpochTicks) return 0;
    const std::uint64_t milliseconds =
        (ticks - kWindowsToUnixEpochTicks) / 10000ULL;
    return milliseconds >
                   static_cast<std::uint64_t>(
                       std::numeric_limits<std::int64_t>::max())
               ? std::numeric_limits<std::int64_t>::max()
               : static_cast<std::int64_t>(milliseconds);
}

NativeMetadata InspectNativeFile(const std::filesystem::path& path) {
    NativeMetadata result;
    HANDLE handle = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        result.error = LastWindowsError();
        return result;
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle, &information)) {
        result.error = LastWindowsError();
        CloseHandle(handle);
        return result;
    }
    CloseHandle(handle);
    result.unsafeLink =
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    result.regular = !result.unsafeLink &&
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    result.sizeBytes =
        (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32U) |
        static_cast<std::uint64_t>(information.nFileSizeLow);
    result.modifiedUnixMilliseconds =
        FileTimeToUnixMilliseconds(information.ftLastWriteTime);
    return result;
}

enum class DirectoryStatus {
    kOk,
    kMissing,
    kNotDirectory,
    kUnsafe,
    kIoError,
};

DirectoryStatus InspectDirectoryPath(const std::filesystem::path& input,
                                     std::error_code& error) {
    error.clear();
    const std::filesystem::path path = input.lexically_normal();
    std::filesystem::path current = path.root_path();
    for (const auto& component : path.relative_path()) {
        current /= component;
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD nativeError = GetLastError();
            if (MissingWindowsError(nativeError)) return DirectoryStatus::kMissing;
            error = std::error_code(static_cast<int>(nativeError),
                                    std::system_category());
            return DirectoryStatus::kIoError;
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return DirectoryStatus::kUnsafe;
        }
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return DirectoryStatus::kMissing;
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
               ? DirectoryStatus::kOk
               : DirectoryStatus::kNotDirectory;
}

bool IsRemoteRoot(const std::filesystem::path& path) noexcept {
    const std::wstring native = path.native();
    if (native.size() < 3) return true;
    const std::array<wchar_t, 4> root = {native[0], L':', L'\\', L'\0'};
    return GetDriveTypeW(root.data()) == DRIVE_REMOTE;
}

#else

NativeMetadata InspectNativeFile(const std::filesystem::path& path) {
    NativeMetadata result;
    struct stat information {};
    if (::lstat(path.c_str(), &information) != 0) {
        result.error = std::error_code(errno, std::generic_category());
        return result;
    }
    result.unsafeLink = S_ISLNK(information.st_mode);
    result.regular = !result.unsafeLink && S_ISREG(information.st_mode);
    result.sizeBytes = result.regular && information.st_size > 0
                           ? static_cast<std::uint64_t>(information.st_size)
                           : 0;
#if defined(__APPLE__)
    result.modifiedUnixMilliseconds =
        static_cast<std::int64_t>(information.st_mtimespec.tv_sec) * 1000LL +
        information.st_mtimespec.tv_nsec / 1000000LL;
#else
    result.modifiedUnixMilliseconds =
        static_cast<std::int64_t>(information.st_mtim.tv_sec) * 1000LL +
        information.st_mtim.tv_nsec / 1000000LL;
#endif
    return result;
}

enum class DirectoryStatus {
    kOk,
    kMissing,
    kNotDirectory,
    kUnsafe,
    kIoError,
};

DirectoryStatus InspectDirectoryPath(const std::filesystem::path& path,
                                     std::error_code& error) {
    error.clear();
    struct stat information {};
    if (::lstat(path.c_str(), &information) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) return DirectoryStatus::kMissing;
        error = std::error_code(errno, std::generic_category());
        return DirectoryStatus::kIoError;
    }
    if (S_ISLNK(information.st_mode)) return DirectoryStatus::kUnsafe;
    return S_ISDIR(information.st_mode) ? DirectoryStatus::kOk
                                        : DirectoryStatus::kNotDirectory;
}

#endif

bool IsRecent(std::int64_t modifiedUnixMilliseconds,
              std::int64_t nowUnixMilliseconds) noexcept {
    if (modifiedUnixMilliseconds <= 0) return false;
    const std::int64_t window = kCodexActivityWindowSeconds * 1000LL;
    if (modifiedUnixMilliseconds > nowUnixMilliseconds) {
        return modifiedUnixMilliseconds - nowUnixMilliseconds <= window;
    }
    return nowUnixMilliseconds - modifiedUnixMilliseconds <= window;
}

struct TailReadResult {
    bool readable = false;
    bool incompleteSingleLine = false;
    std::int64_t modifiedUnixMilliseconds = 0;
    std::uint64_t bytesRead = 0;
    std::vector<std::string> lines;
    std::error_code error;
};

TailReadResult ReadTail(const std::filesystem::path& path) {
    TailReadResult result;
#ifdef _WIN32
    HANDLE handle = CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        result.error = LastWindowsError();
        return result;
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle, &information)) {
        result.error = LastWindowsError();
        CloseHandle(handle);
        return result;
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        result.error =
            std::make_error_code(std::errc::operation_not_permitted);
        CloseHandle(handle);
        return result;
    }
    const std::uint64_t size =
        (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32U) |
        static_cast<std::uint64_t>(information.nFileSizeLow);
    result.modifiedUnixMilliseconds =
        FileTimeToUnixMilliseconds(information.ftLastWriteTime);
    const std::uint64_t offset =
        size > kCodexActivityMaximumTailBytes
            ? size - kCodexActivityMaximumTailBytes
            : 0;
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) {
        result.error = LastWindowsError();
        CloseHandle(handle);
        return result;
    }
    std::string text(static_cast<std::size_t>(size - offset), '\0');
    std::size_t total = 0;
    while (total < text.size()) {
        DWORD chunk = 0;
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(
            text.size() - total, std::numeric_limits<DWORD>::max()));
        if (!ReadFile(handle, text.data() + total, request, &chunk, nullptr)) {
            result.error = LastWindowsError();
            CloseHandle(handle);
            return result;
        }
        if (chunk == 0) break;
        total += chunk;
    }
    CloseHandle(handle);
#else
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        result.error = std::error_code(errno, std::generic_category());
        return result;
    }
    struct stat information {};
    if (::fstat(descriptor, &information) != 0 ||
        !S_ISREG(information.st_mode) || information.st_size < 0) {
        result.error = std::error_code(
            errno == 0 ? EINVAL : errno, std::generic_category());
        ::close(descriptor);
        return result;
    }
    const std::uint64_t size = static_cast<std::uint64_t>(information.st_size);
#if defined(__APPLE__)
    result.modifiedUnixMilliseconds =
        static_cast<std::int64_t>(information.st_mtimespec.tv_sec) * 1000LL +
        information.st_mtimespec.tv_nsec / 1000000LL;
#else
    result.modifiedUnixMilliseconds =
        static_cast<std::int64_t>(information.st_mtim.tv_sec) * 1000LL +
        information.st_mtim.tv_nsec / 1000000LL;
#endif
    const std::uint64_t offset =
        size > kCodexActivityMaximumTailBytes
            ? size - kCodexActivityMaximumTailBytes
            : 0;
    std::string text(static_cast<std::size_t>(size - offset), '\0');
    std::size_t total = 0;
    while (total < text.size()) {
        const ssize_t count = ::pread(
            descriptor, text.data() + total, text.size() - total,
            static_cast<off_t>(offset + total));
        if (count < 0) {
            if (errno == EINTR) continue;
            result.error = std::error_code(errno, std::generic_category());
            ::close(descriptor);
            return result;
        }
        if (count == 0) break;
        total += static_cast<std::size_t>(count);
    }
    ::close(descriptor);
#endif
    text.resize(total);
    result.bytesRead = total;
    std::size_t begin = 0;
    if (offset > 0) {
        const std::size_t newline = text.find('\n');
        if (newline == std::string::npos) {
            result.incompleteSingleLine = true;
            result.readable = true;
            return result;
        }
        begin = newline + 1;
    }
    while (begin <= text.size()) {
        const std::size_t end = text.find('\n', begin);
        std::string line = text.substr(
            begin, end == std::string::npos ? text.size() - begin
                                            : end - begin);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) result.lines.push_back(std::move(line));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    result.readable = true;
    return result;
}

struct Candidate {
    std::filesystem::path path;
    std::int64_t modifiedUnixMilliseconds = 0;
};

bool Cancelled(const CodexActivityScanRequest& request) {
    return request.shouldCancel && request.shouldCancel();
}

}  // namespace

CodexActivityLineResult ParseCodexActivityJsonlLine(
    std::string_view line) noexcept {
    try {
        ActivityRootFields root;
        if (!ParseActivityRoot(line, root)) {
            return {CodexActivityLineDisposition::kMalformed, 0};
        }
        if (!root.timestamp || !root.type || !root.payload) return {};
        const std::optional<std::int64_t> timestamp =
            ParseIso8601Milliseconds(*root.timestamp);
        if (!timestamp) return {};
        const std::string& payloadType = root.payload->type.value_or("");
        if (*root.type == "response_item" && payloadType == "message") {
            if (root.payload->role.value_or("") == "user") {
                return {CodexActivityLineDisposition::kStarted, *timestamp};
            }
            if (root.payload->role.value_or("") == "assistant" &&
                root.payload->phase.value_or("") == "final_answer") {
                return {CodexActivityLineDisposition::kFinished, *timestamp};
            }
        } else if (*root.type == "event_msg") {
            if (payloadType == "task_started" ||
                payloadType == "turn_started" ||
                payloadType == "user_message") {
                return {CodexActivityLineDisposition::kStarted, *timestamp};
            }
            if (payloadType == "task_complete" ||
                payloadType == "turn_completed") {
                return {CodexActivityLineDisposition::kFinished, *timestamp};
            }
        }
        return {};
    } catch (...) {
        return {CodexActivityLineDisposition::kMalformed, 0};
    }
}

CodexActivityFileState InferCodexActivityFileState(
    const std::vector<std::string>& jsonLines,
    std::int64_t modifiedAtUnixMilliseconds,
    std::int64_t nowUnixMilliseconds) noexcept {
    CodexActivityFileState result;
    for (const std::string& line : jsonLines) {
        const CodexActivityLineResult parsed =
            ParseCodexActivityJsonlLine(line);
        if (parsed.disposition == CodexActivityLineDisposition::kMalformed) {
            ++result.malformedLineCount;
        } else if (parsed.disposition ==
                   CodexActivityLineDisposition::kStarted) {
            result.startedAtUnixMilliseconds = std::max(
                result.startedAtUnixMilliseconds,
                parsed.timestampUnixMilliseconds);
        } else if (parsed.disposition ==
                   CodexActivityLineDisposition::kFinished) {
            result.finishedAtUnixMilliseconds = std::max(
                result.finishedAtUnixMilliseconds,
                parsed.timestampUnixMilliseconds);
        }
    }
    const bool recentlyWritten =
        IsRecent(modifiedAtUnixMilliseconds, nowUnixMilliseconds);
    const bool unfinished = result.startedAtUnixMilliseconds > 0 &&
        result.startedAtUnixMilliseconds > result.finishedAtUnixMilliseconds;
    const bool plausibleStart = result.startedAtUnixMilliseconds <=
        nowUnixMilliseconds + kCodexActivityWindowSeconds * 1000LL;
    result.active = recentlyWritten && unfinished && plausibleStart;
    if (result.active) {
        result.durationSeconds = std::max<std::int64_t>(
            0, (nowUnixMilliseconds - result.startedAtUnixMilliseconds) /
                   1000LL);
    }
    return result;
}

CodexActivityScanResult ScanRecentCodexActivity(
    const CodexActivityScanRequest& request) noexcept {
    CodexActivityScanResult result;
    try {
        if (request.sessionsRoot.empty() || request.nowUnixSeconds < 0) {
            result.status = CodexActivityScanStatus::kInvalidArgument;
            result.error = std::make_error_code(std::errc::invalid_argument);
            return result;
        }
        const std::filesystem::path root =
            request.sessionsRoot.lexically_normal();
#ifdef _WIN32
        if (!IsSafeAbsoluteWindowsLocalPath(root.wstring()) ||
            IsRemoteRoot(root)) {
#else
        if (!root.is_absolute()) {
#endif
            result.status = CodexActivityScanStatus::kUnsafeRoot;
            result.error =
                std::make_error_code(std::errc::operation_not_permitted);
            return result;
        }
        std::error_code directoryError;
        const DirectoryStatus rootStatus =
            InspectDirectoryPath(root, directoryError);
        if (rootStatus == DirectoryStatus::kMissing) {
            result.status = CodexActivityScanStatus::kRootNotFound;
            return result;
        }
        if (rootStatus == DirectoryStatus::kUnsafe) {
            result.status = CodexActivityScanStatus::kUnsafeRoot;
            result.error =
                std::make_error_code(std::errc::operation_not_permitted);
            return result;
        }
        if (rootStatus == DirectoryStatus::kNotDirectory) {
            result.status = CodexActivityScanStatus::kRootNotDirectory;
            result.error = std::make_error_code(std::errc::not_a_directory);
            return result;
        }
        if (rootStatus == DirectoryStatus::kIoError) {
            result.status = CodexActivityScanStatus::kIoError;
            result.error = directoryError;
            return result;
        }
        if (Cancelled(request)) {
            result.status = CodexActivityScanStatus::kCancelled;
            return result;
        }

        const std::int64_t nowMilliseconds =
            request.nowUnixSeconds >
                    std::numeric_limits<std::int64_t>::max() / 1000LL
                ? std::numeric_limits<std::int64_t>::max()
                : request.nowUnixSeconds * 1000LL;
        std::vector<Candidate> candidates;
        for (const std::filesystem::path& relative :
             RecentLocalCodexSessionDatePaths(request.nowUnixSeconds, 2)) {
            const std::filesystem::path directory = root / relative;
            directoryError.clear();
            const DirectoryStatus status =
                InspectDirectoryPath(directory, directoryError);
            if (status == DirectoryStatus::kMissing) continue;
            if (status != DirectoryStatus::kOk) {
                ++result.unresolvedRecentFileCount;
                if (!result.error && directoryError) result.error = directoryError;
                continue;
            }
            std::error_code iteratorError;
            std::filesystem::directory_iterator iterator(directory,
                                                         iteratorError);
            if (iteratorError) {
                ++result.unresolvedRecentFileCount;
                if (!result.error) result.error = iteratorError;
                continue;
            }
            std::size_t entryCount = 0;
            const std::filesystem::directory_iterator end;
            while (iterator != end) {
                if (Cancelled(request)) {
                    result.status = CodexActivityScanStatus::kCancelled;
                    return result;
                }
                if (++entryCount > kMaximumDirectoryEntries) {
                    ++result.unresolvedRecentFileCount;
                    break;
                }
                const std::filesystem::path path = iterator->path();
                const CandidateKind kind = ClassifyCandidate(path.filename());
                if (kind != CandidateKind::kIgnored) {
                    const NativeMetadata metadata = InspectNativeFile(path);
                    if (metadata.error) {
                        ++result.unresolvedRecentFileCount;
                        if (!result.error) result.error = metadata.error;
                    } else if (IsRecent(metadata.modifiedUnixMilliseconds,
                                        nowMilliseconds)) {
                        if (!metadata.regular || metadata.unsafeLink) {
                            ++result.unresolvedRecentFileCount;
                        } else if (kind == CandidateKind::kCompressed) {
                            ++result.unresolvedRecentFileCount;
                            ++result.skippedCompressedFileCount;
                        } else {
                            candidates.push_back(
                                {path, metadata.modifiedUnixMilliseconds});
                        }
                    }
                }
                iterator.increment(iteratorError);
                if (iteratorError) {
                    ++result.unresolvedRecentFileCount;
                    if (!result.error) result.error = iteratorError;
                    break;
                }
            }
        }

        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& left, const Candidate& right) {
                      if (left.modifiedUnixMilliseconds !=
                          right.modifiedUnixMilliseconds) {
                          return left.modifiedUnixMilliseconds >
                                 right.modifiedUnixMilliseconds;
                      }
                      return left.path.native() < right.path.native();
                  });
        if (candidates.size() > kCodexActivityMaximumCandidateFiles) {
            result.unresolvedRecentFileCount +=
                candidates.size() - kCodexActivityMaximumCandidateFiles;
            candidates.resize(kCodexActivityMaximumCandidateFiles);
        }
        for (const Candidate& candidate : candidates) {
            if (Cancelled(request)) {
                result.status = CodexActivityScanStatus::kCancelled;
                return result;
            }
            TailReadResult tail = ReadTail(candidate.path);
            result.bytesRead += tail.bytesRead;
            if (!tail.readable || tail.incompleteSingleLine ||
                !IsRecent(tail.modifiedUnixMilliseconds, nowMilliseconds)) {
                ++result.unresolvedRecentFileCount;
                if (!result.error && tail.error) result.error = tail.error;
                continue;
            }
            ++result.readableRecentFileCount;
            const CodexActivityFileState state = InferCodexActivityFileState(
                tail.lines, tail.modifiedUnixMilliseconds, nowMilliseconds);
            result.malformedLineCount += state.malformedLineCount;
            if (state.active) {
                ++result.activeTaskCount;
                result.longestActiveTaskSeconds = std::max(
                    result.longestActiveTaskSeconds, state.durationSeconds);
            }
        }
        if (result.readableRecentFileCount == 0 &&
            result.unresolvedRecentFileCount > 0) {
            result.status = CodexActivityScanStatus::kRecentFilesUnresolved;
            result.activeTaskCount = 0;
            result.longestActiveTaskSeconds = 0;
            return result;
        }
        result.status = CodexActivityScanStatus::kAvailable;
        result.partial = result.unresolvedRecentFileCount > 0;
        return result;
    } catch (const std::filesystem::filesystem_error& error) {
        result.status = CodexActivityScanStatus::kIoError;
        result.error = error.code();
        return result;
    } catch (...) {
        result.status = CodexActivityScanStatus::kIoError;
        result.error = std::make_error_code(std::errc::io_error);
        return result;
    }
}

}  // namespace codex_monitor::codex
