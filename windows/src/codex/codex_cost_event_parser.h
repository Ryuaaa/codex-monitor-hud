#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "codex/codex_cost_model.h"

namespace codex_monitor::codex {

// Privacy-trimmed state for one rollout JSONL stream. It deliberately retains
// no source path, raw JSON, task text, account data, or other session content.
struct CodexCostEventParserState {
    std::string currentModel = "unknown";
    bool hasRawTotalsWatermark = false;
    CodexTokenUsage rawTotalsWatermark;

    // Only the stable hash of an emitted source line is retained. Repeated
    // last_token_usage records can legitimately describe separate turns, so
    // their occurrence suffixes must remain distinct and deterministic.
    std::unordered_map<std::uint64_t, std::uint64_t> emittedOccurrences;
};

// The scanner will convert timestampUnixMilliseconds to the machine's local
// Gregorian date before constructing the existing CodexCostEvent summary row.
// Keeping an absolute timestamp here avoids treating a UTC date as a Windows
// local date around midnight or daylight-saving boundaries.
struct ParsedCodexCostEvent {
    // Source-local identity, byte-for-byte compatible with the frozen macOS
    // line hash. The integration layer must namespace it with the scanner's
    // privacy-safe fileId before constructing a globally deduplicated summary.
    std::string fingerprint;
    std::int64_t timestampUnixMilliseconds = 0;
    std::string model;
    CodexTokenUsage usage;
};

enum class CodexCostLineDisposition {
    kIgnored,
    kMalformed,
    kStateUpdated,
    kEvent,
};

struct CodexCostLineParseResult {
    CodexCostLineDisposition disposition =
        CodexCostLineDisposition::kIgnored;
    std::optional<ParsedCodexCostEvent> event;
};

// Parses exactly one UTF-8 rollout JSONL record. The JSON reader is portable
// C++ and extracts only turn_context.payload.model plus event_msg token_count
// fields. Syntactically malformed records never mutate state.
//
// Cumulative total_token_usage values are counted only above the per-stream
// component-wise high-water mark. If no usable total is present, the parser
// falls back to last_token_usage, matching the frozen macOS 1.0 accounting
// rules. Negative token values are clipped to zero; out-of-range or
// non-integral token fields invalidate that usage object.
[[nodiscard]] CodexCostLineParseResult ParseCodexCostJsonlLine(
    std::string_view line,
    CodexCostEventParserState& state);

}  // namespace codex_monitor::codex
