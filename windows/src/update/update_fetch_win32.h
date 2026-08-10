#pragma once

#include <functional>
#include <string>

namespace codex_monitor::update {

enum class WindowsUpdateFetchFailureKind {
    kNone,
    kCancelled,
    kNetwork,
    kHttp,
    kResponseTooLarge,
    kInvalidResponse,
    kInvalidUtf8,
    kUnexpected,
};

struct WindowsUpdateFetchResult {
    bool succeeded = false;
    std::wstring json;
    WindowsUpdateFetchFailureKind failure =
        WindowsUpdateFetchFailureKind::kUnexpected;
    std::wstring error;
};

using WindowsUpdateCancellationCheck = std::function<bool()>;

// Reads the public release list from the repository's fixed GitHub API
// endpoint. The request sends no cookies or credentials, follows no redirects,
// and accepts at most 2 MiB of response data.
WindowsUpdateFetchResult FetchWindowsUpdateReleasesJson(
    const WindowsUpdateCancellationCheck& cancelled = {}) noexcept;

}  // namespace codex_monitor::update
