#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "codex_executable.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace codex_monitor::codex {
namespace {

struct CandidateBudget {
    std::size_t consumed = 0;

    bool Consume() {
        if (consumed >= kMaximumCodexExecutableCandidates) return false;
        ++consumed;
        return true;
    }

    [[nodiscard]] bool exhausted() const noexcept {
        return consumed >= kMaximumCodexExecutableCandidates;
    }
};

struct NewestCandidate {
    std::optional<std::filesystem::path> path;
    std::uint64_t lastWriteTime = 0;
};

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

bool IsDotDirectory(std::wstring_view name) {
    return name == L"." || name == L"..";
}

bool PathHasNoReparsePoints(const std::filesystem::path& input) {
    const std::filesystem::path path = input.lexically_normal();
    if (!path.is_absolute()) return false;

    std::filesystem::path current = path.root_path();
    if (!current.empty()) {
        const DWORD rootAttributes = GetFileAttributesW(current.c_str());
        if (rootAttributes == INVALID_FILE_ATTRIBUTES ||
            (rootAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return false;
        }
    }
    for (const auto& component : path.relative_path()) {
        current /= component;
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return false;
        }
    }
    return true;
}

bool IsSafeDiscoveryDirectory(const std::filesystem::path& input) {
    const std::filesystem::path path = input.lexically_normal();
    if (!path.is_absolute() || !PathHasNoReparsePoints(path)) return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

std::optional<std::uint64_t> LastWriteTime(
    const std::filesystem::path& candidate) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(candidate.c_str(), GetFileExInfoStandard, &data)) {
        return std::nullopt;
    }
    ULARGE_INTEGER value{};
    value.LowPart = data.ftLastWriteTime.dwLowDateTime;
    value.HighPart = data.ftLastWriteTime.dwHighDateTime;
    return value.QuadPart;
}

void ConsiderAutoCandidate(const std::filesystem::path& input,
                           CandidateBudget& budget,
                           NewestCandidate& newest,
                           bool budgetAlreadyConsumed = false) {
    if (!budgetAlreadyConsumed && !budget.Consume()) return;
    const std::filesystem::path candidate = input.lexically_normal();
    if (!IsSafeCodexExecutable(candidate) ||
        !PathHasNoReparsePoints(candidate)) {
        return;
    }
    const auto modified = LastWriteTime(candidate);
    if (!modified) return;
    if (!newest.path || *modified > newest.lastWriteTime ||
        (*modified == newest.lastWriteTime &&
         candidate.wstring() < newest.path->wstring())) {
        newest.path = candidate;
        newest.lastWriteTime = *modified;
    }
}

std::optional<std::filesystem::path> FindNewestChildBinCandidate(
    const std::filesystem::path& root,
    bool includeRootBin,
    CandidateBudget& budget) {
    NewestCandidate newest;
    if (includeRootBin) {
        ConsiderAutoCandidate(root / L"bin" / L"codex.exe", budget, newest);
    }
    if (budget.exhausted() || !IsSafeDiscoveryDirectory(root)) {
        return newest.path;
    }

    const std::filesystem::path wildcard = root / L"*";
    WIN32_FIND_DATAW entry{};
    HANDLE search = FindFirstFileW(wildcard.c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) return newest.path;
    for (;;) {
        const std::wstring_view name(entry.cFileName);
        if (!IsDotDirectory(name) &&
            !(includeRootBin && EqualsInsensitive(name, L"bin"))) {
            if (!budget.Consume()) break;
            if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                ConsiderAutoCandidate(
                    root / entry.cFileName / L"bin" / L"codex.exe",
                    budget, newest, true);
            }
            if (budget.exhausted()) break;
        }
        if (!FindNextFileW(search, &entry)) break;
    }
    FindClose(search);
    return newest.path;
}

// Desktop installations currently use Codex/bin/<version-or-hash>/codex.exe.
// Inspect that one bounded level only; do not recursively walk the cache.
std::optional<std::filesystem::path> FindNewestBinChildCandidate(
    const std::filesystem::path& root,
    CandidateBudget& budget) {
    NewestCandidate newest;
    const std::filesystem::path binRoot = root / L"bin";
    ConsiderAutoCandidate(binRoot / L"codex.exe", budget, newest);
    if (budget.exhausted() || !IsSafeDiscoveryDirectory(binRoot)) {
        return newest.path;
    }

    const std::filesystem::path wildcard = binRoot / L"*";
    WIN32_FIND_DATAW entry{};
    HANDLE search = FindFirstFileW(wildcard.c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) return newest.path;
    do {
        const std::wstring_view name(entry.cFileName);
        if (IsDotDirectory(name)) continue;
        if (!budget.Consume()) break;
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            continue;
        }
        ConsiderAutoCandidate(binRoot / entry.cFileName / L"codex.exe",
                              budget, newest, true);
    } while (FindNextFileW(search, &entry));
    FindClose(search);
    return newest.path;
}

std::vector<std::filesystem::path> CompatibleNpmVendorCandidates(
    const std::filesystem::path& npmRoot) {
    const std::filesystem::path packageRoot =
        npmRoot / L"node_modules" / L"@openai" / L"codex" /
        L"node_modules" / L"@openai";
    const std::filesystem::path x64 =
        packageRoot / L"codex-win32-x64" / L"vendor" /
        L"x86_64-pc-windows-msvc" / L"bin" / L"codex.exe";
    const std::filesystem::path arm64 =
        packageRoot / L"codex-win32-arm64" / L"vendor" /
        L"aarch64-pc-windows-msvc" / L"bin" / L"codex.exe";
#if defined(_M_ARM64) || defined(__aarch64__)
    // Windows on ARM can run the native binary and normally supports x64
    // emulation as a fallback. Never select by timestamp across architectures.
    return {arm64, x64};
#else
    // An x64 Windows process cannot launch an ARM64 executable.
    return {x64};
#endif
}

std::optional<std::filesystem::path> FindFirstCandidate(
    const std::vector<std::filesystem::path>& candidates,
    CandidateBudget& budget) {
    for (const auto& candidate : candidates) {
        if (!budget.Consume()) return std::nullopt;
        const std::filesystem::path normalized = candidate.lexically_normal();
        if (IsSafeCodexExecutable(normalized) &&
            PathHasNoReparsePoints(normalized)) {
            return normalized;
        }
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> AbsoluteEnvironmentPath(
    const wchar_t* name) {
    const auto value = ReadEnvironmentVariable(name);
    if (!value) return std::nullopt;
    const std::filesystem::path path(*value);
    if (!path.is_absolute()) return std::nullopt;
    return path.lexically_normal();
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
    CandidateBudget budget;
    if (const auto configured = ReadEnvironmentVariable(L"CODEX_CLI_PATH")) {
        if (!budget.Consume()) return std::nullopt;
        const std::filesystem::path candidate(*configured);
        if (IsSafeCodexExecutable(candidate) && PathHasNoReparsePoints(candidate)) {
            return candidate.lexically_normal();
        }
    }

    std::vector<std::filesystem::path> absolutePathDirectories;
    const auto pathValue = ReadEnvironmentVariable(L"PATH");
    if (pathValue) {
        for (const std::wstring& rawDirectory : SplitPath(*pathValue)) {
            const std::filesystem::path directory(rawDirectory);
            // In particular, reject '.', drive-relative paths, and empty PATH entries.
            if (!directory.is_absolute()) continue;
            if (absolutePathDirectories.size() >=
                kMaximumDirectPathDirectories) {
                break;
            }
            const std::filesystem::path normalized = directory.lexically_normal();
            if (!budget.Consume()) return std::nullopt;
            absolutePathDirectories.push_back(normalized);
            const std::filesystem::path candidate = normalized / L"codex.exe";
            if (IsSafeCodexExecutable(candidate) &&
                PathHasNoReparsePoints(candidate)) {
                return candidate;
            }
        }
    }

    const auto localAppData = AbsoluteEnvironmentPath(L"LOCALAPPDATA");
    if (localAppData) {
        NewestCandidate programs;
        ConsiderAutoCandidate(*localAppData / L"Programs" / L"OpenAI" / L"Codex" /
                                  L"bin" / L"codex.exe",
                              budget, programs);
        if (programs.path) return programs.path;
        if (budget.exhausted()) return std::nullopt;

        if (const auto found = FindNewestBinChildCandidate(
                *localAppData / L"OpenAI" / L"Codex", budget)) {
            return found;
        }
        if (budget.exhausted()) return std::nullopt;

        if (const auto found = FindNewestBinChildCandidate(
                *localAppData / L"Packages" / L"OpenAI.Codex_2p2nqsd0c76g0" /
                    L"LocalCache" / L"Local" / L"OpenAI" / L"Codex",
                budget)) {
            return found;
        }
        if (budget.exhausted()) return std::nullopt;
    }

    const auto userProfile = AbsoluteEnvironmentPath(L"USERPROFILE");
    if (userProfile) {
        if (const auto found = FindNewestChildBinCandidate(
                *userProfile / L".codex" / L"packages" / L"standalone" /
                    L"releases",
                false, budget)) {
            return found;
        }
        if (budget.exhausted()) return std::nullopt;
    }

    const auto appData = AbsoluteEnvironmentPath(L"APPDATA");
    if (appData) {
        if (const auto found = FindFirstCandidate(
                CompatibleNpmVendorCandidates(*appData / L"npm"), budget)) {
            return found;
        }
    }

    // A PATH entry may be an npm prefix rather than the directory containing
    // codex.exe. Do this optional expansion after the bounded official
    // user-level layouts so a long PATH cannot starve a normal desktop install.
    for (const auto& directory : absolutePathDirectories) {
        const auto found = FindFirstCandidate(
            CompatibleNpmVendorCandidates(directory), budget);
        if (found) return found;
        if (budget.exhausted()) return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace codex_monitor::codex
