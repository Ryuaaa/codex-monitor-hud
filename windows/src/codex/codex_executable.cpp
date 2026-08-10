#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "codex_executable.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

namespace codex_monitor::codex {
namespace {

std::optional<std::wstring> ReadEnvironmentVariable(const wchar_t* name) {
    SetLastError(ERROR_SUCCESS);
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) return std::nullopt;
    if (required > 32768) return std::nullopt;
    std::wstring value(required, L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, value.data(), required);
    if (copied == 0 || copied >= required) return std::nullopt;
    value.resize(copied);
    return value;
}

bool EqualsInsensitive(std::wstring_view left, std::wstring_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::towlower(left[index]) != std::towlower(right[index])) return false;
    }
    return true;
}

std::wstring UnquotePathEntry(std::wstring_view entry) {
    if (entry.size() >= 2 && entry.front() == L'"' && entry.back() == L'"') {
        entry.remove_prefix(1);
        entry.remove_suffix(1);
    }
    return std::wstring(entry);
}

std::vector<std::wstring> SplitPath(std::wstring_view pathValue) {
    std::vector<std::wstring> entries;
    std::size_t start = 0;
    while (start <= pathValue.size()) {
        const std::size_t end = pathValue.find(L';', start);
        std::wstring entry = UnquotePathEntry(pathValue.substr(
            start, end == std::wstring_view::npos ? pathValue.size() - start : end - start));
        if (!entry.empty()) entries.push_back(std::move(entry));
        if (end == std::wstring_view::npos) break;
        start = end + 1;
    }
    return entries;
}

}  // namespace

bool IsSafeCodexExecutable(const std::filesystem::path& candidate) {
    if (candidate.empty() || !candidate.is_absolute()) return false;
    if (!EqualsInsensitive(candidate.filename().wstring(), L"codex.exe")) return false;

    const std::filesystem::path normalized = candidate.lexically_normal();
    if (!normalized.is_absolute()) return false;
    const DWORD attributes = GetFileAttributesW(normalized.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return false;
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return false;
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return false;

    HANDLE file = CreateFileW(normalized.c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool isDiskFile = GetFileType(file) == FILE_TYPE_DISK;
    CloseHandle(file);
    return isDiskFile;
}

std::optional<std::filesystem::path> FindCodexExecutable() {
    if (const auto configured = ReadEnvironmentVariable(L"CODEX_CLI_PATH")) {
        const std::filesystem::path candidate(*configured);
        if (IsSafeCodexExecutable(candidate)) return candidate.lexically_normal();
    }

    const auto pathValue = ReadEnvironmentVariable(L"PATH");
    if (!pathValue) return std::nullopt;
    for (const std::wstring& rawDirectory : SplitPath(*pathValue)) {
        const std::filesystem::path directory(rawDirectory);
        // In particular, reject '.', drive-relative paths, and empty PATH entries.
        if (!directory.is_absolute()) continue;
        const std::filesystem::path candidate = (directory / L"codex.exe").lexically_normal();
        if (IsSafeCodexExecutable(candidate)) return candidate;
    }
    return std::nullopt;
}

}  // namespace codex_monitor::codex
