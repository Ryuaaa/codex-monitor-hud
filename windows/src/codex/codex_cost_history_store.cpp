#include "codex/codex_cost_history_store.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string_view>
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
#endif

namespace codex_monitor::codex {
namespace {

constexpr std::string_view kVersionLine = "version=1";
constexpr std::int64_t kMaximumUnixSeconds = 253402300799LL;
constexpr std::size_t kMaximumFileIdBytes = 96;
constexpr std::size_t kMaximumModelBytes = 128;
constexpr std::size_t kMaximumLogicalLines =
    2 + kCodexCostHistoryCacheMaximumFiles +
    kCodexCostHistoryCacheMaximumRows;

std::atomic<std::uint64_t> gTemporarySequence{1};

enum class SnapshotValidation {
    kOk,
    kInvalid,
    kTooLarge,
};

enum class ExistingVersionStatus {
    kWritable,
    kUnsupported,
    kIoError,
};

bool StartsWith(std::string_view value, std::string_view prefix) noexcept {
    return value.size() >= prefix.size() &&
           value.substr(0, prefix.size()) == prefix;
}

bool IsLowerHex(char value) noexcept {
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f');
}

bool IsSafeFileId(std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaximumFileIdBytes) return false;
    if (StartsWith(value, "win-")) {
        if (value.size() != 4 + 8 + 1 + 16 || value[12] != '-') return false;
        for (std::size_t index = 4; index < value.size(); ++index) {
            if (index == 12) continue;
            if (!IsLowerHex(value[index])) return false;
        }
        return true;
    }
    if (!StartsWith(value, "posix-")) return false;
    const std::size_t separator = value.find('-', 6);
    if (separator == std::string_view::npos || separator == 6 ||
        separator + 1 >= value.size() ||
        value.find('-', separator + 1) != std::string_view::npos) {
        return false;
    }
    for (std::size_t index = 6; index < value.size(); ++index) {
        if (index == separator) continue;
        if (!IsLowerHex(value[index])) return false;
    }
    return true;
}

bool IsSafeModel(std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaximumModelBytes) return false;
    for (const char character : value) {
        const bool asciiLetter =
            (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z');
        const bool digit = character >= '0' && character <= '9';
        if (!asciiLetter && !digit && character != '.' && character != '-' &&
            character != '_') {
            return false;
        }
    }
    return true;
}

bool IsLeapYear(int year) noexcept {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int DaysInMonth(int year, int month) noexcept {
    static constexpr std::array<int, 12> kDays{
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2 && IsLeapYear(year)) return 29;
    return kDays[static_cast<std::size_t>(month - 1)];
}

bool IsCanonicalDate(std::string_view value) noexcept {
    if (value.size() != 10 || value[4] != '-' || value[7] != '-') {
        return false;
    }
    int digits[8]{};
    std::size_t digitIndex = 0;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 4 || index == 7) continue;
        if (value[index] < '0' || value[index] > '9') return false;
        digits[digitIndex++] = value[index] - '0';
    }
    const int year = digits[0] * 1000 + digits[1] * 100 + digits[2] * 10 +
                     digits[3];
    const int month = digits[4] * 10 + digits[5];
    const int day = digits[6] * 10 + digits[7];
    return year >= 1 && month >= 1 && month <= 12 && day >= 1 &&
           day <= DaysInMonth(year, month);
}

bool UsageIsNonNegative(const CodexTokenUsage& usage) noexcept {
    return usage.inputTokens >= 0 && usage.cachedInputTokens >= 0 &&
           usage.cacheWriteInputTokens >= 0 && usage.outputTokens >= 0;
}

std::int64_t SaturatingCountedTokens(const CodexTokenUsage& usage) noexcept {
    if (usage.inputTokens >
        std::numeric_limits<std::int64_t>::max() - usage.outputTokens) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return usage.inputTokens + usage.outputTokens;
}

SnapshotValidation ValidateSnapshot(const CodexCostHistorySnapshot& snapshot,
                                    std::size_t& rowCount) {
    rowCount = 0;
    if (snapshot.updatedAtUnixSeconds < 0 ||
        snapshot.updatedAtUnixSeconds > kMaximumUnixSeconds) {
        return SnapshotValidation::kInvalid;
    }
    if (snapshot.files.size() > kCodexCostHistoryCacheMaximumFiles) {
        return SnapshotValidation::kTooLarge;
    }

    std::unordered_set<std::string> fileIds;
    fileIds.reserve(snapshot.files.size());
    for (const CodexCostHistoryFileSnapshot& file : snapshot.files) {
        if (!IsSafeFileId(file.fileId) ||
            !fileIds.insert(file.fileId).second ||
            file.parsedOffsetBytes > file.observedSizeBytes ||
            file.complete !=
                (file.parsedOffsetBytes == file.observedSizeBytes) ||
            (file.discardingOversizedLine &&
             !file.hasSkippedOversizedLine) ||
            !IsSafeModel(file.parser.currentModel) ||
            !UsageIsNonNegative(file.parser.rawTotalsWatermark)) {
            return SnapshotValidation::kInvalid;
        }
        const CodexTokenUsage& watermark = file.parser.rawTotalsWatermark;
        if (!file.parser.hasRawTotalsWatermark &&
            (watermark.inputTokens != 0 || watermark.cachedInputTokens != 0 ||
             watermark.cacheWriteInputTokens != 0 ||
             watermark.outputTokens != 0)) {
            return SnapshotValidation::kInvalid;
        }
        if (file.rows.size() >
            kCodexCostHistoryCacheMaximumRows - rowCount) {
            return SnapshotValidation::kTooLarge;
        }
        rowCount += file.rows.size();

        std::set<std::pair<std::string, std::string>> rowKeys;
        for (const CodexCostHistoryRowSnapshot& row : file.rows) {
            if (!IsCanonicalDate(row.localDate) || !IsSafeModel(row.model) ||
                !UsageIsNonNegative(row.usage) ||
                !std::isfinite(row.cachedEstimatedUsd) ||
                row.cachedEstimatedUsd < 0.0 ||
                row.cachedPricedTokens < 0) {
                return SnapshotValidation::kInvalid;
            }
            const std::int64_t counted = SaturatingCountedTokens(row.usage);
            if (counted <= 0 || row.cachedPricedTokens > counted ||
                (row.cachedPricedTokens == 0 &&
                 row.cachedEstimatedUsd != 0.0) ||
                !rowKeys.emplace(row.localDate, row.model).second) {
                return SnapshotValidation::kInvalid;
            }
        }
    }
    return SnapshotValidation::kOk;
}

std::string HexEncode(std::string_view value) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() * 2);
    for (const unsigned char byte : value) {
        result.push_back(kHex[byte >> 4U]);
        result.push_back(kHex[byte & 0x0fU]);
    }
    return result;
}

std::optional<std::string> HexDecode(std::string_view value) {
    if (value.empty() || value.size() % 2 != 0 ||
        value.size() / 2 > kMaximumModelBytes) {
        return std::nullopt;
    }
    const auto nibble = [](char character) -> int {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') {
            return 10 + character - 'a';
        }
        return -1;
    };
    std::string result;
    result.reserve(value.size() / 2);
    for (std::size_t index = 0; index < value.size(); index += 2) {
        const int high = nibble(value[index]);
        const int low = nibble(value[index + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        result.push_back(static_cast<char>((high << 4) | low));
    }
    if (!IsSafeModel(result)) return std::nullopt;
    return result;
}

bool AppendLine(std::string& output, const std::string& line) {
    if (line.size() > kCodexCostHistoryCacheMaximumLineBytes ||
        output.size() > kCodexCostHistoryCacheMaximumBytes - line.size() ||
        output.size() + line.size() >=
            kCodexCostHistoryCacheMaximumBytes) {
        return false;
    }
    output.append(line);
    output.push_back('\n');
    return true;
}

SnapshotValidation SerializeSnapshot(
    const CodexCostHistorySnapshot& snapshot,
    std::string& output,
    std::size_t& rowCount) {
    const SnapshotValidation validation = ValidateSnapshot(snapshot, rowCount);
    if (validation != SnapshotValidation::kOk) return validation;

    output.clear();
    output.reserve(std::min<std::size_t>(
        kCodexCostHistoryCacheMaximumBytes,
        128 + snapshot.files.size() * 256 + rowCount * 192));
    if (!AppendLine(output, std::string(kVersionLine))) {
        return SnapshotValidation::kTooLarge;
    }
    {
        std::ostringstream line;
        line.imbue(std::locale::classic());
        line << "meta\tupdated_at=" << snapshot.updatedAtUnixSeconds
             << "\tfiles=" << snapshot.files.size();
        if (!AppendLine(output, line.str())) {
            return SnapshotValidation::kTooLarge;
        }
    }

    std::vector<const CodexCostHistoryFileSnapshot*> files;
    files.reserve(snapshot.files.size());
    for (const auto& file : snapshot.files) files.push_back(&file);
    std::sort(files.begin(), files.end(), [](const auto* left, const auto* right) {
        return left->fileId < right->fileId;
    });

    for (const CodexCostHistoryFileSnapshot* file : files) {
        std::vector<const CodexCostHistoryRowSnapshot*> rows;
        rows.reserve(file->rows.size());
        for (const auto& row : file->rows) rows.push_back(&row);
        std::sort(rows.begin(), rows.end(), [](const auto* left, const auto* right) {
            if (left->localDate != right->localDate) {
                return left->localDate < right->localDate;
            }
            return left->model < right->model;
        });

        std::ostringstream fileLine;
        fileLine.imbue(std::locale::classic());
        fileLine << "file\tid=" << file->fileId
                 << "\tsize=" << file->observedSizeBytes
                 << "\tmtime_ns=" << file->modifiedUnixNanoseconds
                 << "\toffset=" << file->parsedOffsetBytes
                 << "\tdiscard=" << (file->discardingOversizedLine ? 1 : 0)
                 << "\tskipped=" << (file->hasSkippedOversizedLine ? 1 : 0)
                 << "\tcomplete=" << (file->complete ? 1 : 0)
                 << "\tcurrent_model=" << HexEncode(file->parser.currentModel)
                 << "\thas_watermark="
                 << (file->parser.hasRawTotalsWatermark ? 1 : 0)
                 << "\twi=" << file->parser.rawTotalsWatermark.inputTokens
                 << "\twc="
                 << file->parser.rawTotalsWatermark.cachedInputTokens
                 << "\tww="
                 << file->parser.rawTotalsWatermark.cacheWriteInputTokens
                 << "\two=" << file->parser.rawTotalsWatermark.outputTokens
                 << "\trows=" << rows.size();
        if (!AppendLine(output, fileLine.str())) {
            return SnapshotValidation::kTooLarge;
        }

        for (const CodexCostHistoryRowSnapshot* row : rows) {
            std::ostringstream rowLine;
            rowLine.imbue(std::locale::classic());
            rowLine << "row\tdate=" << row->localDate
                    << "\tmodel=" << HexEncode(row->model)
                    << "\ti=" << row->usage.inputTokens
                    << "\tc=" << row->usage.cachedInputTokens
                    << "\tw=" << row->usage.cacheWriteInputTokens
                    << "\to=" << row->usage.outputTokens
                    << "\tcost="
                    << std::setprecision(std::numeric_limits<double>::max_digits10)
                    << row->cachedEstimatedUsd
                    << "\tpriced=" << row->cachedPricedTokens;
            if (!AppendLine(output, rowLine.str())) {
                return SnapshotValidation::kTooLarge;
            }
        }
    }
    return SnapshotValidation::kOk;
}

template <std::size_t Count>
std::optional<std::array<std::string_view, Count>> SplitExact(
    std::string_view line) {
    std::array<std::string_view, Count> result{};
    std::size_t start = 0;
    for (std::size_t index = 0; index + 1 < Count; ++index) {
        const std::size_t separator = line.find('\t', start);
        if (separator == std::string_view::npos) return std::nullopt;
        result[index] = line.substr(start, separator - start);
        start = separator + 1;
    }
    result.back() = line.substr(start);
    if (result.back().find('\t') != std::string_view::npos) {
        return std::nullopt;
    }
    return result;
}

bool StripPrefix(std::string_view field,
                 std::string_view prefix,
                 std::string_view& value) noexcept {
    if (!StartsWith(field, prefix)) return false;
    value = field.substr(prefix.size());
    return true;
}

bool ParseUnsigned(std::string_view text, std::uint64_t& value) noexcept {
    if (text.empty()) return false;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value, 10);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool ParseSigned(std::string_view text, std::int64_t& value) noexcept {
    if (text.empty()) return false;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value, 10);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool ParseBoolean(std::string_view text, bool& value) noexcept {
    if (text == "0") {
        value = false;
        return true;
    }
    if (text == "1") {
        value = true;
        return true;
    }
    return false;
}

bool ParseFiniteNonNegativeDouble(std::string_view text, double& value) {
    if (text.empty()) return false;
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    input >> std::noskipws >> value;
    return input && input.peek() == std::char_traits<char>::eof() &&
           std::isfinite(value) && value >= 0.0;
}

std::optional<std::vector<std::string_view>> LinesFromContents(
    std::string_view contents,
    bool& tooLarge) {
    tooLarge = false;
    if (contents.empty() || contents.back() != '\n') return std::nullopt;
    std::vector<std::string_view> lines;
    lines.reserve(std::min<std::size_t>(kMaximumLogicalLines, 1024));
    std::size_t start = 0;
    while (start < contents.size()) {
        const std::size_t end = contents.find('\n', start);
        if (end == std::string_view::npos) return std::nullopt;
        std::string_view line = contents.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.size() > kCodexCostHistoryCacheMaximumLineBytes) {
            tooLarge = true;
            return std::nullopt;
        }
        if (lines.size() >= kMaximumLogicalLines) {
            tooLarge = true;
            return std::nullopt;
        }
        lines.push_back(line);
        start = end + 1;
    }
    return lines;
}

CodexCostHistoryLoadStatus ParseContents(
    std::string_view contents,
    CodexCostHistorySnapshot& snapshot) {
    bool linesTooLarge = false;
    const auto lines = LinesFromContents(contents, linesTooLarge);
    if (!lines) {
        return linesTooLarge ? CodexCostHistoryLoadStatus::kTooLarge
                             : CodexCostHistoryLoadStatus::kCorrupt;
    }
    if (lines->empty()) return CodexCostHistoryLoadStatus::kCorrupt;
    if ((*lines)[0] != kVersionLine) {
        return StartsWith((*lines)[0], "version=")
                   ? CodexCostHistoryLoadStatus::kUnsupportedVersion
                   : CodexCostHistoryLoadStatus::kCorrupt;
    }
    if (lines->size() < 2) return CodexCostHistoryLoadStatus::kCorrupt;

    const auto meta = SplitExact<3>((*lines)[1]);
    if (!meta || (*meta)[0] != "meta") {
        return CodexCostHistoryLoadStatus::kCorrupt;
    }
    std::string_view updatedText;
    std::string_view fileCountText;
    std::uint64_t rawFileCount = 0;
    if (!StripPrefix((*meta)[1], "updated_at=", updatedText) ||
        !StripPrefix((*meta)[2], "files=", fileCountText) ||
        !ParseSigned(updatedText, snapshot.updatedAtUnixSeconds) ||
        snapshot.updatedAtUnixSeconds < 0 ||
        snapshot.updatedAtUnixSeconds > kMaximumUnixSeconds ||
        !ParseUnsigned(fileCountText, rawFileCount)) {
        return CodexCostHistoryLoadStatus::kCorrupt;
    }
    if (rawFileCount > kCodexCostHistoryCacheMaximumFiles) {
        return CodexCostHistoryLoadStatus::kTooLarge;
    }
    const std::size_t fileCount = static_cast<std::size_t>(rawFileCount);

    snapshot.files.clear();
    snapshot.files.reserve(fileCount);
    std::size_t lineIndex = 2;
    std::size_t totalRows = 0;
    for (std::size_t fileIndex = 0; fileIndex < fileCount; ++fileIndex) {
        if (lineIndex >= lines->size()) {
            return CodexCostHistoryLoadStatus::kCorrupt;
        }
        const auto fields = SplitExact<15>((*lines)[lineIndex++]);
        if (!fields || (*fields)[0] != "file") {
            return CodexCostHistoryLoadStatus::kCorrupt;
        }

        constexpr std::array<std::string_view, 14> kPrefixes{
            "id=", "size=", "mtime_ns=", "offset=", "discard=", "skipped=",
            "complete=", "current_model=", "has_watermark=", "wi=", "wc=",
            "ww=", "wo=", "rows="};
        std::array<std::string_view, 14> values{};
        for (std::size_t index = 0; index < kPrefixes.size(); ++index) {
            if (!StripPrefix((*fields)[index + 1], kPrefixes[index],
                             values[index])) {
                return CodexCostHistoryLoadStatus::kCorrupt;
            }
        }

        CodexCostHistoryFileSnapshot file;
        file.fileId.assign(values[0]);
        const auto currentModel = HexDecode(values[7]);
        std::uint64_t rawRowCount = 0;
        if (!IsSafeFileId(file.fileId) ||
            !ParseUnsigned(values[1], file.observedSizeBytes) ||
            !ParseSigned(values[2], file.modifiedUnixNanoseconds) ||
            !ParseUnsigned(values[3], file.parsedOffsetBytes) ||
            !ParseBoolean(values[4], file.discardingOversizedLine) ||
            !ParseBoolean(values[5], file.hasSkippedOversizedLine) ||
            !ParseBoolean(values[6], file.complete) || !currentModel ||
            !ParseBoolean(values[8], file.parser.hasRawTotalsWatermark) ||
            !ParseSigned(values[9],
                         file.parser.rawTotalsWatermark.inputTokens) ||
            !ParseSigned(values[10],
                         file.parser.rawTotalsWatermark.cachedInputTokens) ||
            !ParseSigned(
                values[11],
                file.parser.rawTotalsWatermark.cacheWriteInputTokens) ||
            !ParseSigned(values[12],
                         file.parser.rawTotalsWatermark.outputTokens) ||
            !ParseUnsigned(values[13], rawRowCount)) {
            return CodexCostHistoryLoadStatus::kCorrupt;
        }
        if (rawRowCount > kCodexCostHistoryCacheMaximumRows ||
            rawRowCount >
                static_cast<std::uint64_t>(
                    kCodexCostHistoryCacheMaximumRows - totalRows)) {
            return CodexCostHistoryLoadStatus::kTooLarge;
        }
        const std::size_t rowCount = static_cast<std::size_t>(rawRowCount);
        totalRows += rowCount;
        file.parser.currentModel = *currentModel;
        file.rows.reserve(rowCount);

        for (std::size_t rowIndex = 0; rowIndex < rowCount; ++rowIndex) {
            if (lineIndex >= lines->size()) {
                return CodexCostHistoryLoadStatus::kCorrupt;
            }
            const auto rowFields = SplitExact<9>((*lines)[lineIndex++]);
            if (!rowFields || (*rowFields)[0] != "row") {
                return CodexCostHistoryLoadStatus::kCorrupt;
            }
            constexpr std::array<std::string_view, 8> kRowPrefixes{
                "date=", "model=", "i=", "c=", "w=", "o=", "cost=",
                "priced="};
            std::array<std::string_view, 8> rowValues{};
            for (std::size_t index = 0; index < kRowPrefixes.size(); ++index) {
                if (!StripPrefix((*rowFields)[index + 1],
                                 kRowPrefixes[index], rowValues[index])) {
                    return CodexCostHistoryLoadStatus::kCorrupt;
                }
            }
            CodexCostHistoryRowSnapshot row;
            row.localDate.assign(rowValues[0]);
            const auto model = HexDecode(rowValues[1]);
            if (!model ||
                !ParseSigned(rowValues[2], row.usage.inputTokens) ||
                !ParseSigned(rowValues[3], row.usage.cachedInputTokens) ||
                !ParseSigned(rowValues[4],
                             row.usage.cacheWriteInputTokens) ||
                !ParseSigned(rowValues[5], row.usage.outputTokens) ||
                !ParseFiniteNonNegativeDouble(rowValues[6],
                                              row.cachedEstimatedUsd) ||
                !ParseSigned(rowValues[7], row.cachedPricedTokens)) {
                return CodexCostHistoryLoadStatus::kCorrupt;
            }
            row.model = *model;
            file.rows.push_back(std::move(row));
        }
        snapshot.files.push_back(std::move(file));
    }
    if (lineIndex != lines->size()) {
        return CodexCostHistoryLoadStatus::kCorrupt;
    }

    std::size_t validatedRows = 0;
    const SnapshotValidation validation =
        ValidateSnapshot(snapshot, validatedRows);
    if (validation == SnapshotValidation::kTooLarge) {
        return CodexCostHistoryLoadStatus::kTooLarge;
    }
    if (validation != SnapshotValidation::kOk || validatedRows != totalRows) {
        return CodexCostHistoryLoadStatus::kCorrupt;
    }
    return CodexCostHistoryLoadStatus::kOk;
}

ExistingVersionStatus CheckExistingVersion(
    const std::filesystem::path& path,
    std::error_code& error) {
    error.clear();
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::error_code existsError;
        const bool exists = std::filesystem::exists(path, existsError);
        if (!exists && !existsError) return ExistingVersionStatus::kWritable;
        error = existsError ? existsError
                            : std::make_error_code(std::errc::io_error);
        return ExistingVersionStatus::kIoError;
    }

    std::string firstLine;
    firstLine.reserve(64);
    for (;;) {
        const int next = input.get();
        if (next == std::char_traits<char>::eof() || next == '\n') break;
        if (firstLine.size() >= 64) {
            return StartsWith(firstLine, "version=")
                       ? ExistingVersionStatus::kUnsupported
                       : ExistingVersionStatus::kWritable;
        }
        firstLine.push_back(static_cast<char>(next));
    }
    if (input.bad()) {
        error = std::make_error_code(std::errc::io_error);
        return ExistingVersionStatus::kIoError;
    }
    if (!firstLine.empty() && firstLine.back() == '\r') firstLine.pop_back();
    if (StartsWith(firstLine, "version=") && firstLine != kVersionLine) {
        return ExistingVersionStatus::kUnsupported;
    }
    return ExistingVersionStatus::kWritable;
}

std::filesystem::path TemporaryPathFor(const std::filesystem::path& path) {
    const auto tick = std::chrono::high_resolution_clock::now()
                          .time_since_epoch()
                          .count();
    const std::uint64_t random =
        (static_cast<std::uint64_t>(std::random_device{}()) << 32U) ^
        static_cast<std::uint64_t>(std::random_device{}());
    std::ostringstream suffix;
    suffix.imbue(std::locale::classic());
    suffix << ".tmp." << std::hex << tick << '.' << random << '.'
           << gTemporarySequence.fetch_add(1);
    std::filesystem::path temporary = path;
    temporary += std::filesystem::path(suffix.str());
    return temporary;
}

class TemporaryFileGuard {
public:
    explicit TemporaryFileGuard(std::filesystem::path path)
        : path_(std::move(path)) {}

    ~TemporaryFileGuard() {
        if (!active_) return;
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    TemporaryFileGuard(const TemporaryFileGuard&) = delete;
    TemporaryFileGuard& operator=(const TemporaryFileGuard&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    void Release() noexcept { active_ = false; }

private:
    std::filesystem::path path_;
    bool active_ = true;
};

std::error_code DefaultAtomicReplace(const std::filesystem::path& temporary,
                                     const std::filesystem::path& destination) {
#ifdef _WIN32
    if (MoveFileExW(temporary.c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return {};
    }
    return std::error_code(static_cast<int>(GetLastError()),
                           std::system_category());
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    return error;
#endif
}

}  // namespace

CodexCostHistoryStore::CodexCostHistoryStore(
    std::filesystem::path path,
    CodexCostHistoryAtomicReplace atomicReplace)
    : path_(std::move(path)), atomicReplace_(std::move(atomicReplace)) {}

CodexCostHistoryLoadResult CodexCostHistoryStore::Load() const noexcept {
    CodexCostHistoryLoadResult result;
    try {
        if (path_.empty()) {
            result.status = CodexCostHistoryLoadStatus::kIoError;
            result.error = std::make_error_code(std::errc::invalid_argument);
            return result;
        }
        std::ifstream input(path_, std::ios::binary);
        if (!input) {
            std::error_code existsError;
            const bool exists = std::filesystem::exists(path_, existsError);
            if (!exists && !existsError) {
                result.status = CodexCostHistoryLoadStatus::kNotFound;
            } else {
                result.status = CodexCostHistoryLoadStatus::kIoError;
                result.error = existsError
                                   ? existsError
                                   : std::make_error_code(std::errc::io_error);
            }
            return result;
        }

        input.seekg(0, std::ios::end);
        const std::streamoff length = input.tellg();
        if (length < 0) {
            result.status = CodexCostHistoryLoadStatus::kIoError;
            result.error = std::make_error_code(std::errc::io_error);
            return result;
        }
        if (static_cast<std::uintmax_t>(length) >
            kCodexCostHistoryCacheMaximumBytes) {
            result.status = CodexCostHistoryLoadStatus::kTooLarge;
            return result;
        }
        input.seekg(0, std::ios::beg);
        std::string contents(static_cast<std::size_t>(length), '\0');
        if (!contents.empty()) {
            input.read(contents.data(),
                       static_cast<std::streamsize>(contents.size()));
            if (input.gcount() !=
                static_cast<std::streamsize>(contents.size())) {
                result.status = CodexCostHistoryLoadStatus::kIoError;
                result.error = std::make_error_code(std::errc::io_error);
                return result;
            }
        }
        const int trailing = input.peek();
        if (input.bad() || trailing != std::char_traits<char>::eof()) {
            result.status = CodexCostHistoryLoadStatus::kIoError;
            result.error = std::make_error_code(std::errc::io_error);
            return result;
        }

        CodexCostHistorySnapshot parsed;
        result.status = ParseContents(contents, parsed);
        if (result.status == CodexCostHistoryLoadStatus::kOk) {
            result.snapshot = std::move(parsed);
        }
        return result;
    } catch (const std::filesystem::filesystem_error& exception) {
        result.status = CodexCostHistoryLoadStatus::kIoError;
        result.error = exception.code();
        return result;
    } catch (...) {
        result.status = CodexCostHistoryLoadStatus::kIoError;
        result.error = std::make_error_code(std::errc::io_error);
        return result;
    }
}

CodexCostHistorySaveResult CodexCostHistoryStore::Save(
    const CodexCostHistorySnapshot& snapshot) const noexcept {
    CodexCostHistorySaveResult result;
    try {
        if (path_.empty()) {
            result.status = CodexCostHistorySaveStatus::kIoError;
            result.error = std::make_error_code(std::errc::invalid_argument);
            return result;
        }

        std::string contents;
        std::size_t rowCount = 0;
        const SnapshotValidation validation =
            SerializeSnapshot(snapshot, contents, rowCount);
        if (validation == SnapshotValidation::kInvalid) {
            result.status = CodexCostHistorySaveStatus::kInvalidSnapshot;
            return result;
        }
        if (validation == SnapshotValidation::kTooLarge) {
            result.status = CodexCostHistorySaveStatus::kTooLarge;
            return result;
        }

        std::error_code versionError;
        ExistingVersionStatus version =
            CheckExistingVersion(path_, versionError);
        if (version == ExistingVersionStatus::kUnsupported) {
            result.status = CodexCostHistorySaveStatus::kUnsupportedVersion;
            return result;
        }
        if (version == ExistingVersionStatus::kIoError) {
            result.status = CodexCostHistorySaveStatus::kIoError;
            result.error = versionError;
            return result;
        }

        std::error_code directoryError;
        const std::filesystem::path parent = path_.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, directoryError);
        }
        if (directoryError) {
            result.status = CodexCostHistorySaveStatus::kIoError;
            result.error = directoryError;
            return result;
        }

        TemporaryFileGuard temporary(TemporaryPathFor(path_));
        {
            std::ofstream output(temporary.path(),
                                 std::ios::binary | std::ios::trunc);
            if (!output) {
                result.status = CodexCostHistorySaveStatus::kIoError;
                result.error = std::make_error_code(std::errc::io_error);
                return result;
            }
            output.write(contents.data(),
                         static_cast<std::streamsize>(contents.size()));
            output.flush();
            output.close();
            if (!output) {
                result.status = CodexCostHistorySaveStatus::kIoError;
                result.error = std::make_error_code(std::errc::io_error);
                return result;
            }
        }

        // Check again immediately before the trusted atomic callback so a
        // downgrade cannot casually replace a newer cache observed mid-save.
        version = CheckExistingVersion(path_, versionError);
        if (version != ExistingVersionStatus::kWritable) {
            result.status = version == ExistingVersionStatus::kUnsupported
                                ? CodexCostHistorySaveStatus::kUnsupportedVersion
                                : CodexCostHistorySaveStatus::kIoError;
            result.error = versionError;
            return result;
        }

        const std::error_code replaceError =
            atomicReplace_ ? atomicReplace_(temporary.path(), path_)
                           : DefaultAtomicReplace(temporary.path(), path_);
        if (replaceError) {
            result.status =
                replaceError == std::make_error_code(std::errc::operation_canceled)
                    ? CodexCostHistorySaveStatus::kCancelled
                    : CodexCostHistorySaveStatus::kIoError;
            result.error = replaceError;
            return result;
        }
        temporary.Release();

        result.status = CodexCostHistorySaveStatus::kWritten;
        result.storedFileCount = snapshot.files.size();
        result.storedRowCount = rowCount;
        return result;
    } catch (const std::filesystem::filesystem_error& exception) {
        result.status = CodexCostHistorySaveStatus::kIoError;
        result.error = exception.code();
        return result;
    } catch (...) {
        result.status = CodexCostHistorySaveStatus::kIoError;
        result.error = std::make_error_code(std::errc::io_error);
        return result;
    }
}

}  // namespace codex_monitor::codex
