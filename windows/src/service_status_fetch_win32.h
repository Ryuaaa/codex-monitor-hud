#pragma once

#include "service_status_model.h"

#include <functional>
#include <string>

namespace codex_monitor {

enum class OpenAIServiceStatusFailureKind {
    kNone,
    kCancelled,
    kNetwork,
    kHttp,
    kResponseTooLarge,
    kInvalidResponse,
};

struct OpenAIServiceStatusFetchResult {
    bool succeeded = false;
    OpenAIServiceStatusModel status;
    OpenAIServiceStatusFailureKind failure =
        OpenAIServiceStatusFailureKind::kInvalidResponse;
    std::wstring error;
};

using ServiceStatusCancellationCheck = std::function<bool()>;

// Reads only OpenAI's public Statuspage summary. The request is fixed to the
// official HTTPS host, sends no account data, and accepts at most 1 MiB.
OpenAIServiceStatusFetchResult FetchOpenAIServiceStatus(
    const ServiceStatusCancellationCheck& cancelled = {}) noexcept;

}  // namespace codex_monitor
