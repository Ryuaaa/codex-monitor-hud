#pragma once

#include "codex_types.h"

#include <filesystem>
#include <optional>
#include <string_view>

namespace codex_monitor::codex {

// Each function accepts the UTF-8 JSON object contained in the corresponding
// app-server response's `result` field, not the outer JSON-RPC envelope.
// The initialize parser deliberately exposes only the validated local Codex
// home path; it does not add that path to the product-facing Codex data model.
std::optional<std::filesystem::path> ParseInitializeCodexHomeResultJson(
    std::string_view json);
MethodParseResult<RateLimitsData> ParseRateLimitsResultJson(std::string_view json);
MethodParseResult<AccountData> ParseAccountResultJson(std::string_view json);
MethodParseResult<UsageData> ParseUsageResultJson(std::string_view json);
MethodParseResult<ThreadListData> ParseThreadListResultJson(std::string_view json);

}  // namespace codex_monitor::codex
