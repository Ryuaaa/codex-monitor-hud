#include "codex/codex_cost_history_state.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <unordered_set>
#include <utility>

namespace codex_monitor::codex {
namespace {

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
    for (const FileState& file : files_) result.push_back(file.cursor);
    return result;
}

CodexCostHistoryApplyResult CodexCostHistoryState::Apply(
    const CodexCostFileScanResult& scan,
    const CodexCostLocalDateResolver& localDateResolver) {
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
        if (cursor.resetAfterTruncation) {
            existing->parser = CodexCostEventParserState{};
            existing->rows.clear();
        }
        existing->cursor = cursor;
    }
    files_.erase(
        std::remove_if(files_.begin(), files_.end(), [&present](const FileState& file) {
            return present.find(file.cursor.fileId) == present.end();
        }),
        files_.end());

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
}

}  // namespace codex_monitor::codex
