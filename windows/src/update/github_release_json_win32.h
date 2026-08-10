#pragma once

#include "update/github_release_selector.h"

#include <optional>
#include <string_view>
#include <vector>

namespace codex_monitor::update {

// Parses only the public fields needed by the Windows update selector. The
// returned model contains no account, credential, or response metadata.
std::optional<std::vector<GitHubReleaseCandidate>>
ParseGitHubReleaseListJson(std::wstring_view json) noexcept;

}  // namespace codex_monitor::update
