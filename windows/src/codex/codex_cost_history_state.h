#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "codex/codex_cost_event_parser.h"
#include "codex/codex_cost_file_scan.h"
#include "codex/codex_cost_summary.h"

namespace codex_monitor::codex {

using CodexCostLocalDateResolver =
    std::function<std::optional<std::string>(std::int64_t)>;

struct CodexCostHistoryApplyResult {
    std::vector<CodexCostEvent> events;
    std::size_t malformedLineCount = 0;
    std::size_t invalidTimestampCount = 0;
    bool saturated = false;
};

// Keeps only privacy-trimmed parser state and day/model aggregates in memory.
// It never retains source paths, raw JSON, task text, or account data.
class CodexCostHistoryState {
public:
    CodexCostHistoryState();
    ~CodexCostHistoryState();

    CodexCostHistoryState(const CodexCostHistoryState&) = delete;
    CodexCostHistoryState& operator=(const CodexCostHistoryState&) = delete;

    [[nodiscard]] std::vector<CodexCostFileCursor> Cursors() const;

    [[nodiscard]] CodexCostHistoryApplyResult Apply(
        const CodexCostFileScanResult& scan,
        const CodexCostLocalDateResolver& localDateResolver);

    void Clear() noexcept;

private:
    struct FileState;
    std::vector<FileState> files_;
};

}  // namespace codex_monitor::codex
