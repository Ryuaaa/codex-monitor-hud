#pragma once

#include "codex_types.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>

namespace codex_monitor::codex {

enum class AppServerClientFailureKind {
    kStartFailed,
    kInitializeRejected,
    kWriteFailed,
    kTransportFailed,
    kTimedOut,
    kCancelled,
};

struct AppServerRefreshReport {
    bool initialized = false;
    bool rateLimitsResponseReceived = false;
    bool accountResponseReceived = false;
    bool usageResponseReceived = false;
    bool threadListResponseReceived = false;
    std::size_t ignoredNotificationCount = 0;
    std::size_t ignoredUnknownIdCount = 0;
    std::size_t malformedEnvelopeCount = 0;
    std::optional<AppServerClientFailureKind> failure;

    [[nodiscard]] bool allMethodsCompleted() const noexcept {
        return rateLimitsResponseReceived && accountResponseReceived &&
               usageResponseReceived && threadListResponseReceived;
    }
};

// One Refresh owns one short-lived app-server process. Parsed method state is
// retained across refreshes, but raw JSON-RPC lines, stderr, account identity,
// previews, paths, thread identifiers, and session content are not retained.
class CodexAppServerClient {
public:
    static constexpr std::size_t kMaximumResponseLines = 512;

    AppServerRefreshReport Refresh(const std::filesystem::path& executable,
                                   std::string_view clientVersion,
                                   const std::function<bool()>& isCancelled = {});

    [[nodiscard]] const CodexDataState& data() const noexcept { return data_; }
    // Thread-confined input for future local-only scanners. It is intentionally
    // absent from CodexDataState and every refresh result passed to the UI.
    [[nodiscard]] const std::optional<std::filesystem::path>& codexHome() const
        noexcept {
        return codexHome_;
    }

private:
    CodexDataState data_;
    std::optional<std::filesystem::path> codexHome_;
};

}  // namespace codex_monitor::codex
