#pragma once

#include "codex_types.h"

#include <string_view>

namespace codex_monitor::codex {

// Each function accepts the UTF-8 JSON object contained in the corresponding
// app-server response's `result` field, not the outer JSON-RPC envelope.
MethodParseResult<RateLimitsData> ParseRateLimitsResultJson(std::string_view json);
MethodParseResult<AccountData> ParseAccountResultJson(std::string_view json);
MethodParseResult<UsageData> ParseUsageResultJson(std::string_view json);
MethodParseResult<ThreadListData> ParseThreadListResultJson(std::string_view json);

}  // namespace codex_monitor::codex
