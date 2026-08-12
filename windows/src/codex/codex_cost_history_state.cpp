#include "codex/codex_cost_history_state.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace codex_monitor::codex {
namespace {

constexpr std::int64_t kMaximumUnixSeconds = 253402300799LL;
constexpr std::size_t kMaximumFileIdBytes = 96;
constexpr std::size_t kMaximumModelBytes = 128;

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

bool SnapshotIsConsistent(const CodexCostHistorySnapshot& snapshot) {
    if (snapshot.trackingStartedAtUnixSeconds <= 0 ||
        snapshot.trackingStartedAtUnixSeconds > snapshot.updatedAtUnixSeconds ||
        snapshot.updatedAtUnixSeconds < 0 ||
        snapshot.updatedAtUnixSeconds > kMaximumUnixSeconds ||
        snapshot.files.size() > kCodexCostHistoryCacheMaximumFiles) {
        return false;
    }

    std::size_t totalRows = 0;
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
            !UsageIsNonNegative(file.parser.rawTotalsWatermark) ||
            file.rows.size() >
                kCodexCostHistoryCacheMaximumRows - totalRows) {
            return false;
        }
        const CodexTokenUsage& watermark = file.parser.rawTotalsWatermark;
        if (!file.parser.hasRawTotalsWatermark &&
            (watermark.inputTokens != 0 || watermark.cachedInputTokens != 0 ||
             watermark.cacheWriteInputTokens != 0 ||
             watermark.outputTokens != 0)) {
            return false;
        }
        totalRows += file.rows.size();

        std::set<std::pair<std::string, std::string>> rowKeys;
        for (const CodexCostHistoryRowSnapshot& row : file.rows) {
            if (!IsCanonicalDate(row.localDate) || !IsSafeModel(row.model) ||
                !UsageIsNonNegative(row.usage) ||
                !std::isfinite(row.cachedEstimatedUsd) ||
                row.cachedEstimatedUsd < 0.0 ||
                row.cachedPricedTokens < 0) {
                return false;
            }
            const std::int64_t counted = SaturatingCountedTokens(row.usage);
            if (counted <= 0 || row.cachedPricedTokens > counted ||
                (row.cachedPricedTokens == 0 &&
                 row.cachedEstimatedUsd != 0.0) ||
                !rowKeys.emplace(row.localDate, row.model).second) {
                return false;
            }
        }
    }
    return true;
}

void SaturatingAdd(std::int64_t value,
                   std::int64_t& target,
                   bool& saturated) noexcept {
    value = std::max<std::int64_t>(0, value);
    if (value > std::numeric_limits<std::int64_t>::max() - target) {
        target = std::numeric_limits<std::int64_t>::max();
        saturated = true;
    } else {
        target += value;
    }
}

void SaturatingAdd(double value,
                   double& target,
                   bool& saturated) noexcept {
    if (!std::isfinite(value) || value < 0.0) return;
    if (value > std::numeric_limits<double>::max() - target) {
        target = std::numeric_limits<double>::max();
        saturated = true;
    } else {
        target += value;
    }
}

std::int64_t CountPricedTokens(const CodexTokenUsage& usage,
                               bool& saturated) noexcept {
    std::int64_t result = 0;
    SaturatingAdd(usage.inputTokens, result, saturated);
    SaturatingAdd(usage.outputTokens, result, saturated);
    return result;
}

}  // namespace

struct CodexCostHistoryState::FileState {
    CodexCostFileCursor cursor;
    CodexCostEventParserState parser;
    std::map<std::pair<std::string, std::string>, CodexCostEvent> rows;
};

CodexCostHistoryState::CodexCostHistoryState() = default;
CodexCostHistoryState::~CodexCostHistoryState() = default;

std::vector<CodexCostFileCursor> CodexCostHistoryState::Cursors() const {
    std::vector<CodexCostFileCursor> result;
    result.reserve(files_.size());
    for (const FileState& file : files_) {
        CodexCostFileCursor cursor = file.cursor;
        cursor.needsModelSeed = file.parser.currentModel == "unknown";
        result.push_back(std::move(cursor));
    }
    return result;
}

std::optional<CodexCostHistorySnapshot>
CodexCostHistoryState::ExportSnapshot(
    std::int64_t updatedAtUnixSeconds,
    std::int64_t trackingStartedAtUnixSeconds) const noexcept {
    try {
        CodexCostHistorySnapshot snapshot;
        snapshot.trackingStartedAtUnixSeconds = trackingStartedAtUnixSeconds > 0
            ? trackingStartedAtUnixSeconds
            : (trackingStartedAtUnixSeconds_ > 0
                   ? trackingStartedAtUnixSeconds_
                   : updatedAtUnixSeconds);
        snapshot.updatedAtUnixSeconds = updatedAtUnixSeconds;
        snapshot.files.reserve(files_.size());
        for (const FileState& file : files_) {
            CodexCostHistoryFileSnapshot exported;
            exported.fileId = file.cursor.fileId;
            exported.observedSizeBytes = file.cursor.observedSizeBytes;
            exported.modifiedUnixNanoseconds =
                file.cursor.modifiedUnixNanoseconds;
            exported.parsedOffsetBytes = file.cursor.parsedOffsetBytes;
            exported.discardingOversizedLine =
                file.cursor.discardingOversizedLine;
            exported.hasSkippedOversizedLine =
                file.cursor.hasSkippedOversizedLine;
            exported.complete = file.cursor.complete;
            exported.parser.currentModel = file.parser.currentModel;
            exported.parser.baselinePending = file.parser.baselinePending;
            exported.parser.hasRawTotalsWatermark =
                file.parser.hasRawTotalsWatermark;
            exported.parser.rawTotalsWatermark =
                file.parser.rawTotalsWatermark;
            exported.rows.reserve(file.rows.size());

            for (const auto& entry : file.rows) {
                const auto& key = entry.first;
                const CodexCostEvent& event = entry.second;
                const std::string expectedFingerprint =
                    file.cursor.fileId + "|" + key.first + "|" + key.second;
                if (event.localDate != key.first || event.model != key.second ||
                    event.fingerprint != expectedFingerprint ||
                    !event.cachedEstimatedUsd || !event.cachedPricedTokens) {
                    return std::nullopt;
                }
                CodexCostHistoryRowSnapshot row;
                row.localDate = event.localDate;
                row.model = event.model;
                row.usage = event.usage;
                row.cachedEstimatedUsd = *event.cachedEstimatedUsd;
                row.cachedPricedTokens = *event.cachedPricedTokens;
                exported.rows.push_back(std::move(row));
            }
            snapshot.files.push_back(std::move(exported));
        }
        if (!SnapshotIsConsistent(snapshot)) return std::nullopt;
        return snapshot;
    } catch (...) {
        return std::nullopt;
    }
}

bool CodexCostHistoryState::ImportSnapshot(
    const CodexCostHistorySnapshot& snapshot) noexcept {
    try {
        if (!SnapshotIsConsistent(snapshot)) return false;

        std::vector<FileState> restored;
        restored.reserve(snapshot.files.size());
        for (const CodexCostHistoryFileSnapshot& imported : snapshot.files) {
            FileState file;
            file.cursor.fileId = imported.fileId;
            file.cursor.observedSizeBytes = imported.observedSizeBytes;
            file.cursor.modifiedUnixNanoseconds =
                imported.modifiedUnixNanoseconds;
            file.cursor.parsedOffsetBytes = imported.parsedOffsetBytes;
            file.cursor.discardingOversizedLine =
                imported.discardingOversizedLine;
            file.cursor.hasSkippedOversizedLine =
                imported.hasSkippedOversizedLine;
            file.cursor.complete = imported.complete;
            // resetAfterTruncation is a one-scan command, never durable state.
            file.cursor.resetAfterTruncation = false;
            file.parser.currentModel = imported.parser.currentModel;
            file.parser.baselinePending = imported.parser.baselinePending;
            file.parser.hasRawTotalsWatermark =
                imported.parser.hasRawTotalsWatermark;
            file.parser.rawTotalsWatermark =
                imported.parser.rawTotalsWatermark;
            file.parser.emittedOccurrences.clear();

            for (const CodexCostHistoryRowSnapshot& importedRow :
                 imported.rows) {
                const std::pair<std::string, std::string> key{
                    importedRow.localDate, importedRow.model};
                CodexCostEvent event;
                event.fingerprint = imported.fileId + "|" + key.first + "|" +
                                    key.second;
                event.localDate = key.first;
                event.model = key.second;
                event.usage = importedRow.usage;
                event.cachedEstimatedUsd = importedRow.cachedEstimatedUsd;
                event.cachedPricedTokens = importedRow.cachedPricedTokens;
                const auto inserted =
                    file.rows.emplace(std::move(key), std::move(event));
                if (!inserted.second) return false;
            }
            restored.push_back(std::move(file));
        }

        files_.swap(restored);
        trackingStartedAtUnixSeconds_ =
            snapshot.trackingStartedAtUnixSeconds;
        return true;
    } catch (...) {
        return false;
    }
}

CodexCostHistoryApplyResult CodexCostHistoryState::Apply(
    const CodexCostFileScanResult& scan,
    const CodexCostLocalDateResolver& localDateResolver,
    std::int64_t earliestEventUnixMilliseconds) {
    CodexCostHistoryApplyResult output;
    if (!scan.ok() || !localDateResolver) {
        for (const FileState& file : files_) {
            for (const auto& entry : file.rows) output.events.push_back(entry.second);
        }
        return output;
    }

    std::unordered_set<std::string> present;
    present.reserve(scan.files.size());
    for (const CodexCostFileCursor& cursor : scan.files) {
        if (cursor.fileId.empty()) continue;
        present.insert(cursor.fileId);
        auto existing = std::find_if(
            files_.begin(), files_.end(), [&cursor](const FileState& file) {
                return file.cursor.fileId == cursor.fileId;
            });
        if (existing == files_.end()) {
            files_.push_back(FileState{});
            existing = std::prev(files_.end());
            existing->cursor.fileId = cursor.fileId;
        }
        if (cursor.establishBaseline) {
            existing->parser = CodexCostEventParserState{};
            if (!cursor.baselineModel.empty()) {
                existing->parser.currentModel =
                    NormalizeCodexCostModel(cursor.baselineModel);
                if (existing->parser.currentModel.empty()) {
                    existing->parser.currentModel = "unknown";
                }
            }
            existing->parser.baselinePending = true;
            existing->rows.clear();
        }
        if (!cursor.baselineModel.empty() &&
            existing->parser.currentModel == "unknown") {
            const std::string seededModel =
                NormalizeCodexCostModel(cursor.baselineModel);
            if (!seededModel.empty() && seededModel != "unknown") {
                existing->parser.currentModel = seededModel;
                std::vector<CodexCostEvent> reattributed;
                for (auto iterator = existing->rows.begin();
                     iterator != existing->rows.end();) {
                    if (iterator->first.second != "unknown") {
                        ++iterator;
                        continue;
                    }
                    CodexCostEvent event = std::move(iterator->second);
                    iterator = existing->rows.erase(iterator);
                    event.model = seededModel;
                    event.fingerprint = existing->cursor.fileId + "|" +
                        event.localDate + "|" + seededModel;
                    const CodexCostEstimate estimate =
                        EstimateCodexApiEquivalentCost(seededModel,
                                                       event.usage);
                    event.cachedEstimatedUsd =
                        estimate.available ? estimate.estimatedUsd : 0.0;
                    bool saturated = false;
                    event.cachedPricedTokens = estimate.available
                        ? CountPricedTokens(event.usage, saturated)
                        : 0;
                    output.saturated = output.saturated || saturated;
                    reattributed.push_back(std::move(event));
                }
                for (CodexCostEvent& event : reattributed) {
                    const std::pair<std::string, std::string> key{
                        event.localDate, event.model};
                    existing->rows.emplace(key, std::move(event));
                }
            }
        }
        if (cursor.resetAfterTruncation) {
            existing->parser = CodexCostEventParserState{};
            existing->rows.clear();
        }
        existing->cursor = cursor;
        existing->cursor.establishBaseline = false;
        existing->cursor.needsModelSeed = false;
        existing->cursor.baselineModel.clear();
    }
    // A partial discovery cannot distinguish deletion from a temporary
    // permission or enumeration failure. Preserve the last good rows until a
    // complete discovery confirms that a file is gone.
    if (!scan.discoveryIncomplete) {
        files_.erase(
            std::remove_if(
                files_.begin(), files_.end(),
                [&present](const FileState& file) {
                    return present.find(file.cursor.fileId) == present.end();
                }),
            files_.end());
    }

    for (const CodexCostFileLine& line : scan.lines) {
        auto file = std::find_if(
            files_.begin(), files_.end(), [&line](const FileState& candidate) {
                return candidate.cursor.fileId == line.fileId;
            });
        if (file == files_.end()) continue;
        CodexCostLineParseResult parsed =
            ParseCodexCostJsonlLine(line.text, file->parser);
        if (parsed.disposition == CodexCostLineDisposition::kMalformed) {
            ++output.malformedLineCount;
            continue;
        }
        if (!parsed.event) continue;
        if (earliestEventUnixMilliseconds > 0 &&
            parsed.event->timestampUnixMilliseconds <
                earliestEventUnixMilliseconds) {
            continue;
        }
        const std::optional<std::string> localDate =
            localDateResolver(parsed.event->timestampUnixMilliseconds);
        if (!localDate) {
            ++output.invalidTimestampCount;
            continue;
        }

        const std::pair<std::string, std::string> key{
            *localDate, parsed.event->model.empty() ? "unknown" : parsed.event->model};
        CodexCostEvent& row = file->rows[key];
        if (row.fingerprint.empty()) {
            row.fingerprint = file->cursor.fileId + "|" + key.first + "|" + key.second;
            row.localDate = key.first;
            row.model = key.second;
            row.cachedEstimatedUsd = 0.0;
            row.cachedPricedTokens = 0;
        }

        SaturatingAdd(parsed.event->usage.inputTokens,
                      row.usage.inputTokens, output.saturated);
        SaturatingAdd(parsed.event->usage.cachedInputTokens,
                      row.usage.cachedInputTokens, output.saturated);
        SaturatingAdd(parsed.event->usage.cacheWriteInputTokens,
                      row.usage.cacheWriteInputTokens, output.saturated);
        SaturatingAdd(parsed.event->usage.outputTokens,
                      row.usage.outputTokens, output.saturated);

        const CodexCostEstimate estimate = EstimateCodexApiEquivalentCost(
            parsed.event->model, parsed.event->usage);
        if (estimate.available) {
            SaturatingAdd(estimate.estimatedUsd,
                          *row.cachedEstimatedUsd, output.saturated);
            const std::int64_t priced =
                CountPricedTokens(parsed.event->usage, output.saturated);
            SaturatingAdd(priced, *row.cachedPricedTokens, output.saturated);
        }
    }

    for (FileState& file : files_) {
        // Line fingerprints are only needed while parsing one delivered batch.
        // File offsets prevent rereading completed records across later scans.
        file.parser.emittedOccurrences.clear();
        for (const auto& entry : file.rows) output.events.push_back(entry.second);
    }
    return output;
}

void CodexCostHistoryState::Clear() noexcept {
    files_.clear();
    trackingStartedAtUnixSeconds_ = 0;
}

}  // namespace codex_monitor::codex
