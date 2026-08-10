#include "codex/codex_cost_file_scan.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using codex_monitor::codex::CodexCostFileCursor;
using codex_monitor::codex::CodexCostFileScanRequest;
using codex_monitor::codex::CodexCostFileScanStatus;
using codex_monitor::codex::IsSafeAbsoluteWindowsLocalPath;
using codex_monitor::codex::RecentLocalCodexSessionDatePaths;
using codex_monitor::codex::ScanCodexCostRolloutFiles;

void Require(bool condition, std::string_view message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto sequence = std::chrono::high_resolution_clock::now()
                                  .time_since_epoch()
                                  .count();
        path_ = std::filesystem::temp_directory_path() /
                ("codex-monitor-cost-scan-" + std::to_string(sequence));
        std::error_code error;
        Require(std::filesystem::create_directories(path_, error) && !error,
                "temporary directory must be created");
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

std::int64_t NowUnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::filesystem::path CurrentSessionDirectory(
    const std::filesystem::path& home,
    std::int64_t now) {
    const auto dates = RecentLocalCodexSessionDatePaths(now, 1);
    Require(dates.size() == 1, "the current local date must be available");
    const auto directory = home / "sessions" / dates.front();
    std::error_code error;
    Require(std::filesystem::create_directories(directory, error) && !error,
            "current session directory must be created");
    return directory;
}

void WriteFile(const std::filesystem::path& path,
               std::string_view contents) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    Require(!error, "file parent must be created");
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    Require(static_cast<bool>(output), "test file must be writable");
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.flush();
    Require(static_cast<bool>(output), "test file write must complete");
}

void AppendFile(const std::filesystem::path& path,
                std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::app);
    Require(static_cast<bool>(output), "test file must be appendable");
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.flush();
    Require(static_cast<bool>(output), "test append must complete");
}

CodexCostFileScanRequest Request(const std::filesystem::path& home,
                                 std::int64_t now) {
    CodexCostFileScanRequest request;
    request.codexHome = home;
    request.nowUnixSeconds = now;
    return request;
}

std::set<std::string> LineTexts(
    const std::vector<codex_monitor::codex::CodexCostFileLine>& lines) {
    std::set<std::string> result;
    for (const auto& line : lines) result.insert(line.text);
    return result;
}

void TestWindowsRootSyntaxPolicy() {
    Require(IsSafeAbsoluteWindowsLocalPath(L"C:\\Users\\alice\\.codex"),
            "drive-absolute Windows root must be accepted");
    Require(IsSafeAbsoluteWindowsLocalPath(L"d:/Codex/Data"),
            "forward slash drive-absolute root must be accepted");
    Require(!IsSafeAbsoluteWindowsLocalPath(L"C:relative\\.codex"),
            "drive-relative root must be rejected");
    Require(!IsSafeAbsoluteWindowsLocalPath(L"\\root-relative"),
            "root-relative path must be rejected");
    Require(!IsSafeAbsoluteWindowsLocalPath(L".\\relative"),
            "ordinary relative path must be rejected");
    Require(!IsSafeAbsoluteWindowsLocalPath(L"\\\\server\\share\\.codex"),
            "UNC root must be rejected");
    Require(!IsSafeAbsoluteWindowsLocalPath(L"//server/share/.codex"),
            "slash UNC root must be rejected");
    Require(!IsSafeAbsoluteWindowsLocalPath(L"\\\\?\\C:\\Users\\alice"),
            "device namespace root must be rejected");
    const std::wstring embeddedNul(L"C:\\safe\0hidden", 14);
    Require(!IsSafeAbsoluteWindowsLocalPath(embeddedNul),
            "embedded NUL root must be rejected");
}

void TestRecentDatePolicy() {
    const auto dates = RecentLocalCodexSessionDatePaths(NowUnixSeconds(), 30);
    Require(dates.size() == 30, "exactly thirty local dates must be returned");
    std::set<std::filesystem::path> unique(dates.begin(), dates.end());
    Require(unique.size() == dates.size(), "recent local dates must be unique");
    for (const auto& path : dates) {
        std::size_t components = 0;
        for (const auto& component : path) {
            ++components;
            const std::string value = component.string();
            Require(!value.empty() &&
                        std::all_of(value.begin(), value.end(), [](char byte) {
                            return byte >= '0' && byte <= '9';
                        }),
                    "date components must contain ASCII digits only");
        }
        Require(components == 3, "a date path must be YYYY/MM/DD");
    }
    Require(RecentLocalCodexSessionDatePaths(NowUnixSeconds(), 31).size() ==
                30,
            "date policy must not exceed the thirty-day product limit");
}

void TestCandidateDiscoveryRetentionAndPrivacy() {
    TemporaryDirectory temporary;
    const std::int64_t now = NowUnixSeconds();
    const auto session = CurrentSessionDirectory(temporary.path(), now);
    WriteFile(session / "rollout-active.jsonl", "session-one\nsession-two\r\n");
    WriteFile(session / "notes.jsonl", "must-not-read\n");
    WriteFile(session / "rollout-compressed.jsonl.zst", "compressed\n");
    WriteFile(temporary.path() / "sessions" / "1900" / "01" / "01" /
                  "rollout-old-session.jsonl",
              "old-session\n");

    const auto archived = temporary.path() / "archived_sessions";
    WriteFile(archived / "archived-session.jsonl", "archive\n");
    const auto expired = archived / "rollout-expired.jsonl";
    WriteFile(expired, "expired\n");
    std::error_code timeError;
    std::filesystem::last_write_time(
        expired,
        std::filesystem::file_time_type::clock::now() -
            std::chrono::hours(31 * 24),
        timeError);
    Require(!timeError, "expired archive time must be adjustable");

    const auto result = ScanCodexCostRolloutFiles(Request(temporary.path(), now));
    Require(result.ok(), "valid root scan must succeed");
    Require(result.files.size() == 2,
            "only active session and retained archive files must be discovered");
    Require(LineTexts(result.lines) ==
                std::set<std::string>({"session-one", "session-two", "archive"}),
            "only whitelisted rollout JSONL content must be returned");
    Require(result.skippedCompressedFiles == 1 &&
                result.ignoredExpiredArchivedFiles == 1,
            "compressed and expired archive coverage must be accounted for");
    Require(result.coverageIncomplete,
            "an unsupported compressed rollout must mark coverage incomplete");
    Require(result.bytesRead <=
                codex_monitor::codex::kCodexCostMaximumScanBytes,
            "scan must stay inside the global byte budget");
    for (const auto& file : result.files) {
        Require(file.complete, "newline-terminated fixtures must scan completely");
        Require(file.fileId.find(temporary.path().string()) == std::string::npos &&
                    file.fileId.find("rollout-") == std::string::npos,
                "persistent file identity must not contain the raw source path");
    }
}

void TestIncrementalAppendPartialLineAndTruncation() {
    TemporaryDirectory temporary;
    const std::int64_t now = NowUnixSeconds();
    const auto path = CurrentSessionDirectory(temporary.path(), now) /
                      "rollout-incremental.jsonl";
    WriteFile(path, "one\n");

    auto first = ScanCodexCostRolloutFiles(Request(temporary.path(), now));
    Require(first.ok() && first.files.size() == 1 &&
                LineTexts(first.lines) == std::set<std::string>({"one"}),
            "initial complete line must be scanned");
    Require(first.files.front().parsedOffsetBytes == 4 &&
                first.files.front().complete,
            "initial cursor must stop after the newline");

    AppendFile(path, "two\n");
    auto secondRequest = Request(temporary.path(), now);
    secondRequest.previousFiles = first.files;
    auto second = ScanCodexCostRolloutFiles(secondRequest);
    Require(LineTexts(second.lines) == std::set<std::string>({"two"}),
            "an append scan must emit only newly appended lines");
    Require(second.files.front().parsedOffsetBytes == 8 &&
                second.files.front().complete,
            "append cursor must advance to the new end");

    AppendFile(path, "partial");
    auto partialRequest = Request(temporary.path(), now);
    partialRequest.previousFiles = second.files;
    auto partial = ScanCodexCostRolloutFiles(partialRequest);
    Require(partial.lines.empty() && !partial.files.front().complete &&
                partial.files.front().parsedOffsetBytes == 8,
            "an unterminated line must not be emitted or committed");

    AppendFile(path, "-done\n");
    auto completedRequest = Request(temporary.path(), now);
    completedRequest.previousFiles = partial.files;
    auto completed = ScanCodexCostRolloutFiles(completedRequest);
    Require(LineTexts(completed.lines) ==
                std::set<std::string>({"partial-done"}) &&
                completed.files.front().complete,
            "a previously partial line must be emitted after its newline arrives");

    WriteFile(path, "x\n");
    auto truncatedRequest = Request(temporary.path(), now);
    truncatedRequest.previousFiles = completed.files;
    auto truncated = ScanCodexCostRolloutFiles(truncatedRequest);
    Require(LineTexts(truncated.lines) == std::set<std::string>({"x"}) &&
                truncated.files.front().resetAfterTruncation,
            "a truncated file must reset to offset zero");

    const auto rewriteBaseline = truncated.files;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    WriteFile(path, "y\n");
    auto rewriteRequest = Request(temporary.path(), now);
    rewriteRequest.previousFiles = rewriteBaseline;
    auto rewritten = ScanCodexCostRolloutFiles(rewriteRequest);
    Require(LineTexts(rewritten.lines) == std::set<std::string>({"y"}) &&
                rewritten.files.front().resetAfterTruncation,
            "a same-size in-place rewrite must reset retained parser state");
}

void TestBudgetBoundaryAndResume() {
    TemporaryDirectory temporary;
    const std::int64_t now = NowUnixSeconds();
    const auto path = CurrentSessionDirectory(temporary.path(), now) /
                      "rollout-budget.jsonl";
    WriteFile(path, "aaa\nbbb\nccc\n");

    auto request = Request(temporary.path(), now);
    request.byteBudgetBytes = 6;
    auto first = ScanCodexCostRolloutFiles(request);
    Require(first.bytesRead == 6 && first.budgetExhausted &&
                first.coverageIncomplete,
            "a constrained pass must consume no more than its exact budget");
    Require(LineTexts(first.lines) == std::set<std::string>({"aaa"}) &&
                first.files.front().parsedOffsetBytes == 4,
            "a budget-ending partial line must not advance the cursor");

    auto resumedRequest = Request(temporary.path(), now);
    resumedRequest.previousFiles = first.files;
    auto resumed = ScanCodexCostRolloutFiles(resumedRequest);
    Require(LineTexts(resumed.lines) ==
                std::set<std::string>({"bbb", "ccc"}) &&
                resumed.files.front().complete,
            "the next pass must resume from the last complete line boundary");
}

void TestOversizedLineIsBoundedAndCanResume() {
    TemporaryDirectory temporary;
    const std::int64_t now = NowUnixSeconds();
    const auto path = CurrentSessionDirectory(temporary.path(), now) /
                      "rollout-long.jsonl";
    WriteFile(path, std::string(20, 'x') + "\nok\n");

    auto firstRequest = Request(temporary.path(), now);
    firstRequest.maximumLineBytes = 8;
    firstRequest.byteBudgetBytes = 12;
    auto first = ScanCodexCostRolloutFiles(firstRequest);
    Require(first.skippedOversizedLines == 1 && first.lines.empty() &&
                first.files.front().discardingOversizedLine &&
                first.files.front().parsedOffsetBytes == 12,
            "an oversized line must be discarded with bounded memory and progress");

    auto resumedRequest = Request(temporary.path(), now);
    resumedRequest.maximumLineBytes = 8;
    resumedRequest.previousFiles = first.files;
    auto resumed = ScanCodexCostRolloutFiles(resumedRequest);
    Require(LineTexts(resumed.lines) == std::set<std::string>({"ok"}) &&
                resumed.files.front().complete &&
                resumed.files.front().hasSkippedOversizedLine &&
                resumed.coverageIncomplete,
            "resuming mid-oversized line must discard to newline then recover");
}

void TestUnsafeRootsAndCandidateLinks() {
    TemporaryDirectory temporary;
    const std::int64_t now = NowUnixSeconds();

    auto relativeRequest = Request(std::filesystem::path("relative-home"), now);
    Require(ScanCodexCostRolloutFiles(relativeRequest).status ==
                CodexCostFileScanStatus::kUnsafeRoot,
            "a relative root must be rejected before enumeration");

    const auto realHome = temporary.path() / "real-home";
    const auto session = CurrentSessionDirectory(realHome, now);
    const auto outside = temporary.path() / "outside.jsonl";
    WriteFile(outside, "outside\n");

    std::error_code linkError;
    const auto linkedCandidate = session / "rollout-link.jsonl";
    std::filesystem::create_symlink(outside, linkedCandidate, linkError);
    if (!linkError) {
        const auto result = ScanCodexCostRolloutFiles(Request(realHome, now));
        Require(result.ok() && result.lines.empty() &&
                    result.rejectedUnsafeEntries == 1 &&
                    result.coverageIncomplete,
                "a rollout symlink or reparse point must never be followed");
    }

    linkError.clear();
    const auto linkedRoot = temporary.path() / "linked-home";
    std::filesystem::create_directory_symlink(realHome, linkedRoot, linkError);
    if (!linkError) {
        const auto result = ScanCodexCostRolloutFiles(Request(linkedRoot, now));
        Require(result.status == CodexCostFileScanStatus::kUnsafeRoot,
                "a symlink or reparse point root must be rejected");
    }
}

}  // namespace

int main() {
    TestWindowsRootSyntaxPolicy();
    TestRecentDatePolicy();
    TestCandidateDiscoveryRetentionAndPrivacy();
    TestIncrementalAppendPartialLineAndTruncation();
    TestBudgetBoundaryAndResume();
    TestOversizedLineIsBoundedAndCanResume();
    TestUnsafeRootsAndCandidateLinks();
    std::cout << "codex_cost_file_scan_test: pass\n";
    return 0;
}
