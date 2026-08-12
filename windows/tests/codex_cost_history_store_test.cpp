#include "codex/codex_cost_history_store.h"
#include "codex/codex_cost_history_state.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {

using namespace codex_monitor::codex;

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
                ("codex-monitor-cost-cache-" + std::to_string(sequence));
        std::error_code error;
        Require(std::filesystem::create_directories(path_, error) && !error,
                "temporary directory must be created");
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    Require(static_cast<bool>(input), "test file must be readable");
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

void WriteFile(const std::filesystem::path& path, std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    Require(static_cast<bool>(output), "test file must be writable");
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.flush();
    Require(static_cast<bool>(output), "test file write must finish");
}

std::string WindowsFileId(std::uint64_t index) {
    std::ostringstream output;
    output << "win-12345678-" << std::hex << std::setfill('0') << std::setw(16)
           << index;
    return output.str();
}

CodexCostHistoryRowSnapshot Row(std::string date,
                                std::string model,
                                std::int64_t input,
                                std::int64_t output,
                                double cost,
                                std::int64_t priced) {
    CodexCostHistoryRowSnapshot row;
    row.localDate = std::move(date);
    row.model = std::move(model);
    row.usage.inputTokens = input;
    row.usage.cachedInputTokens = input / 4;
    row.usage.cacheWriteInputTokens = input / 8;
    row.usage.outputTokens = output;
    row.cachedEstimatedUsd = cost;
    row.cachedPricedTokens = priced;
    return row;
}

CodexCostHistoryFileSnapshot File(std::string id) {
    CodexCostHistoryFileSnapshot file;
    file.fileId = std::move(id);
    file.observedSizeBytes = 4096;
    file.modifiedUnixNanoseconds = 1'775'000'000'123'456'700LL;
    file.parsedOffsetBytes = 4096;
    file.complete = true;
    file.parser.currentModel = "gpt-5.6-sol";
    file.parser.hasRawTotalsWatermark = true;
    file.parser.rawTotalsWatermark = {1000, 250, 125, 100};
    file.rows.push_back(
        Row("2026-08-11", "gpt-5.6-sol", 1000, 100, 0.00375, 1100));
    return file;
}

CodexCostHistorySnapshot Snapshot() {
    CodexCostHistorySnapshot snapshot;
    snapshot.trackingStartedAtUnixSeconds = 1'774'999'000;
    snapshot.updatedAtUnixSeconds = 1'775'000'000;
    snapshot.files.push_back(File(WindowsFileId(2)));
    CodexCostHistoryFileSnapshot partial = File("posix-a-b");
    partial.observedSizeBytes = 8192;
    partial.parsedOffsetBytes = 6144;
    partial.complete = false;
    partial.hasSkippedOversizedLine = true;
    partial.rows.clear();
    partial.rows.push_back(
        Row("2026-08-10", "gpt-5.6-terra", 700, 50, 0.0012, 750));
    snapshot.files.push_back(std::move(partial));
    return snapshot;
}

void RequireSameUsage(const CodexTokenUsage& left,
                      const CodexTokenUsage& right,
                      std::string_view message) {
    Require(left.inputTokens == right.inputTokens &&
                left.cachedInputTokens == right.cachedInputTokens &&
                left.cacheWriteInputTokens == right.cacheWriteInputTokens &&
                left.outputTokens == right.outputTokens,
            message);
}

const CodexCostHistoryFileSnapshot* FindFile(
    const CodexCostHistorySnapshot& snapshot,
    std::string_view fileId) {
    const auto found = std::find_if(
        snapshot.files.begin(), snapshot.files.end(),
        [fileId](const CodexCostHistoryFileSnapshot& file) {
            return file.fileId == fileId;
        });
    return found == snapshot.files.end() ? nullptr : &*found;
}

const CodexCostHistoryRowSnapshot* FindRow(
    const CodexCostHistoryFileSnapshot& file,
    std::string_view date,
    std::string_view model) {
    const auto found = std::find_if(
        file.rows.begin(), file.rows.end(),
        [date, model](const CodexCostHistoryRowSnapshot& row) {
            return row.localDate == date && row.model == model;
        });
    return found == file.rows.end() ? nullptr : &*found;
}

bool SameSnapshot(const CodexCostHistorySnapshot& left,
                  const CodexCostHistorySnapshot& right) {
    if (left.trackingStartedAtUnixSeconds !=
            right.trackingStartedAtUnixSeconds ||
        left.updatedAtUnixSeconds != right.updatedAtUnixSeconds ||
        left.files.size() != right.files.size()) {
        return false;
    }
    for (const CodexCostHistoryFileSnapshot& leftFile : left.files) {
        const CodexCostHistoryFileSnapshot* rightFile =
            FindFile(right, leftFile.fileId);
        if (!rightFile ||
            leftFile.observedSizeBytes != rightFile->observedSizeBytes ||
            leftFile.modifiedUnixNanoseconds !=
                rightFile->modifiedUnixNanoseconds ||
            leftFile.parsedOffsetBytes != rightFile->parsedOffsetBytes ||
            leftFile.discardingOversizedLine !=
                rightFile->discardingOversizedLine ||
            leftFile.hasSkippedOversizedLine !=
                rightFile->hasSkippedOversizedLine ||
            leftFile.complete != rightFile->complete ||
            leftFile.parser.currentModel !=
                rightFile->parser.currentModel ||
            leftFile.parser.baselinePending !=
                rightFile->parser.baselinePending ||
            leftFile.parser.hasRawTotalsWatermark !=
                rightFile->parser.hasRawTotalsWatermark ||
            leftFile.rows.size() != rightFile->rows.size()) {
            return false;
        }
        const CodexTokenUsage& leftWatermark =
            leftFile.parser.rawTotalsWatermark;
        const CodexTokenUsage& rightWatermark =
            rightFile->parser.rawTotalsWatermark;
        if (leftWatermark.inputTokens != rightWatermark.inputTokens ||
            leftWatermark.cachedInputTokens !=
                rightWatermark.cachedInputTokens ||
            leftWatermark.cacheWriteInputTokens !=
                rightWatermark.cacheWriteInputTokens ||
            leftWatermark.outputTokens != rightWatermark.outputTokens) {
            return false;
        }
        for (const CodexCostHistoryRowSnapshot& leftRow : leftFile.rows) {
            const CodexCostHistoryRowSnapshot* rightRow =
                FindRow(*rightFile, leftRow.localDate, leftRow.model);
            if (!rightRow ||
                leftRow.usage.inputTokens != rightRow->usage.inputTokens ||
                leftRow.usage.cachedInputTokens !=
                    rightRow->usage.cachedInputTokens ||
                leftRow.usage.cacheWriteInputTokens !=
                    rightRow->usage.cacheWriteInputTokens ||
                leftRow.usage.outputTokens != rightRow->usage.outputTokens ||
                leftRow.cachedEstimatedUsd !=
                    rightRow->cachedEstimatedUsd ||
                leftRow.cachedPricedTokens !=
                    rightRow->cachedPricedTokens) {
                return false;
            }
        }
    }
    return true;
}

void TestRoundTripAndPrivacyWhitelist() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "codex-cost-cache.txt";
    const CodexCostHistorySnapshot source = Snapshot();
    const auto saved = CodexCostHistoryStore(path).Save(source);
    Require(saved.status == CodexCostHistorySaveStatus::kWritten &&
                saved.storedFileCount == 2 && saved.storedRowCount == 2,
            "a valid snapshot must be written");

    const std::string contents = ReadFile(path);
    Require(contents.size() <= kCodexCostHistoryCacheMaximumBytes,
            "serialized cache must respect the eight MiB cap");
    Require(contents.rfind("version=2\nmeta\tstarted_at=", 0) == 0,
            "cache must start with the exact version and metadata records");
    Require(contents.find("path=") == std::string::npos &&
                contents.find("account") == std::string::npos &&
                contents.find("task") == std::string::npos &&
                contents.find("prompt") == std::string::npos &&
                contents.find("raw_json") == std::string::npos &&
                contents.find("C:\\Users") == std::string::npos,
            "cache must contain only the privacy whitelist");

    const auto loaded = CodexCostHistoryStore(path).Load();
    Require(loaded.status == CodexCostHistoryLoadStatus::kOk &&
                loaded.snapshot.updatedAtUnixSeconds ==
                    source.updatedAtUnixSeconds &&
                loaded.snapshot.files.size() == 2,
            "a written snapshot must load completely");
    // Serialization is canonical, so the POSIX identity sorts before win-*.
    const auto& partial = loaded.snapshot.files.front();
    Require(partial.fileId == "posix-a-b" && !partial.complete &&
                partial.hasSkippedOversizedLine &&
                partial.parsedOffsetBytes == 6144 &&
                partial.rows.size() == 1,
            "cursor flags and compacted rows must round-trip");
    Require(partial.parser.currentModel == "gpt-5.6-sol" &&
                partial.parser.hasRawTotalsWatermark,
            "parser state must round-trip");
    RequireSameUsage(partial.parser.rawTotalsWatermark,
                     source.files[1].parser.rawTotalsWatermark,
                     "the cumulative watermark must round-trip");
    Require(partial.rows.front().cachedEstimatedUsd == 0.0012 &&
                partial.rows.front().cachedPricedTokens == 750,
            "frozen cost fields must round-trip exactly");
}

void TestUnknownVersionIsNeverOverwritten() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "codex-cost-cache.txt";
    const std::string future = "version=99\nprivate-future-format\n";
    WriteFile(path, future);

    const auto loaded = CodexCostHistoryStore(path).Load();
    Require(loaded.status ==
                CodexCostHistoryLoadStatus::kUnsupportedVersion &&
                loaded.snapshot.files.empty(),
            "an unknown version must not be interpreted");

    bool replacementCalled = false;
    CodexCostHistoryStore store(
        path, [&](const std::filesystem::path&, const std::filesystem::path&) {
            replacementCalled = true;
            return std::error_code{};
        });
    const auto saved = store.Save(Snapshot());
    Require(saved.status ==
                CodexCostHistorySaveStatus::kUnsupportedVersion &&
                !replacementCalled && ReadFile(path) == future,
            "an older writer must preserve a newer cache byte-for-byte");
}

void TestLegacyVersionCanBeReplacedByInstallBaseline() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "codex-cost-cache.txt";
    WriteFile(path, "version=1\nlegacy-history-must-not-load\n");
    Require(CodexCostHistoryStore(path).Load().status ==
                CodexCostHistoryLoadStatus::kUnsupportedVersion,
            "the pre-install-history cache must not be imported");
    Require(CodexCostHistoryStore(path).Save(Snapshot()).written(),
            "a validated installation baseline may replace version one");
    Require(ReadFile(path).rfind("version=2\nmeta\tstarted_at=", 0) == 0,
            "legacy replacement must write the installation start marker");
}

void TestAtomicFailureAndCancellationPreserveOldFile() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "codex-cost-cache.txt";
    Require(CodexCostHistoryStore(path).Save(Snapshot()).written(),
            "atomic failure fixture must write");
    const std::string original = ReadFile(path);

    std::filesystem::path failedTemporary;
    CodexCostHistoryStore failing(
        path,
        [&](const std::filesystem::path& temporaryPath,
            const std::filesystem::path& destinationPath) {
            failedTemporary = temporaryPath;
            Require(temporaryPath.parent_path() ==
                        destinationPath.parent_path(),
                    "temporary file must share the destination directory");
            Require(std::filesystem::exists(temporaryPath),
                    "temporary file must be complete before replacement");
            Require(ReadFile(destinationPath) == original,
                    "old file must still exist before replacement");
            return std::make_error_code(std::errc::permission_denied);
        });
    CodexCostHistorySnapshot changed = Snapshot();
    changed.updatedAtUnixSeconds += 60;
    const auto failed = failing.Save(changed);
    Require(failed.status == CodexCostHistorySaveStatus::kIoError &&
                ReadFile(path) == original && !failedTemporary.empty() &&
                !std::filesystem::exists(failedTemporary),
            "replacement failure must preserve the old file and clean temp");

    std::filesystem::path cancelledTemporary;
    CodexCostHistoryStore cancelled(
        path,
        [&](const std::filesystem::path& temporaryPath,
            const std::filesystem::path&) {
            cancelledTemporary = temporaryPath;
            return std::make_error_code(std::errc::operation_canceled);
        });
    const auto cancelledResult = cancelled.Save(changed);
    Require(cancelledResult.status == CodexCostHistorySaveStatus::kCancelled &&
                ReadFile(path) == original && !cancelledTemporary.empty() &&
                !std::filesystem::exists(cancelledTemporary),
            "cancelled replacement must also preserve the old file");

    std::filesystem::path throwingTemporary;
    CodexCostHistoryStore throwing(
        path,
        [&](const std::filesystem::path& temporaryPath,
            const std::filesystem::path&) -> std::error_code {
            throwingTemporary = temporaryPath;
            throw std::runtime_error("injected replacement exception");
        });
    const auto throwingResult = throwing.Save(changed);
    Require(throwingResult.status == CodexCostHistorySaveStatus::kIoError &&
                ReadFile(path) == original && !throwingTemporary.empty() &&
                !std::filesystem::exists(throwingTemporary),
            "a throwing callback must not leak temp or damage the old file");
}

void TestAnyCurrentVersionDamageRejectsTheWholeCache() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "codex-cost-cache.txt";
    Require(CodexCostHistoryStore(path).Save(Snapshot()).written(),
            "corruption fixture must write");
    std::string contents = ReadFile(path);
    const std::string needle = "priced=750";
    const std::size_t position = contents.find(needle);
    Require(position != std::string::npos,
            "corruption fixture must expose the priced field");
    contents.replace(position, needle.size(), "priced=999999");
    WriteFile(path, contents);

    const auto loaded = CodexCostHistoryStore(path).Load();
    Require(loaded.status == CodexCostHistoryLoadStatus::kCorrupt &&
                loaded.snapshot.files.empty(),
            "one damaged row must reject every cursor and aggregate");

    contents = "version=2\nmeta\tstarted_at=1774999000\tupdated_at=1775000000\tfiles=0\nextra\n";
    WriteFile(path, contents);
    const auto extra = CodexCostHistoryStore(path).Load();
    Require(extra.status == CodexCostHistoryLoadStatus::kCorrupt &&
                extra.snapshot.files.empty(),
            "unknown records must not bypass the whitelist");

    WriteFile(path, "version=2\nmeta\tstarted_at=1774999000\tupdated_at=1775000000\tfiles=0");
    const auto truncated = CodexCostHistoryStore(path).Load();
    Require(truncated.status == CodexCostHistoryLoadStatus::kCorrupt &&
                truncated.snapshot.files.empty(),
            "a missing final newline must be treated as truncation");
}

void TestLoadByteAndLineLimits() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "codex-cost-cache.txt";
    std::string huge(kCodexCostHistoryCacheMaximumBytes + 1, 'x');
    WriteFile(path, huge);
    Require(CodexCostHistoryStore(path).Load().status ==
                CodexCostHistoryLoadStatus::kTooLarge,
            "a cache over eight MiB must be rejected before parsing");

    std::string longLine = "version=2\n";
    longLine.append(kCodexCostHistoryCacheMaximumLineBytes + 1, 'x');
    longLine.push_back('\n');
    WriteFile(path, longLine);
    Require(CodexCostHistoryStore(path).Load().status ==
                CodexCostHistoryLoadStatus::kTooLarge,
            "a logical line over four KiB must be rejected");

    WriteFile(path,
              "version=2\nmeta\tstarted_at=1774999000\tupdated_at=1775000000\tfiles=4097\n");
    Require(CodexCostHistoryStore(path).Load().status ==
                CodexCostHistoryLoadStatus::kTooLarge,
            "a declared file count over 4096 must be rejected as too large");

    std::ostringstream excessiveRows;
    excessiveRows
        << "version=2\nmeta\tstarted_at=1774999000\tupdated_at=1775000000\tfiles=1\n"
        << "file\tid=" << WindowsFileId(1)
        << "\tsize=0\tmtime_ns=0\toffset=0\tdiscard=0\tskipped=0"
           "\tcomplete=1\tcurrent_model=756e6b6e6f776e\tbaseline_pending=0\thas_watermark=0"
           "\twi=0\twc=0\tww=0\two=0\trows=32769\n";
    WriteFile(path, excessiveRows.str());
    Require(CodexCostHistoryStore(path).Load().status ==
                CodexCostHistoryLoadStatus::kTooLarge,
            "a declared aggregate count over 32768 must be rejected as too large");
}

void TestSaveCountAndSerializedByteLimits() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "codex-cost-cache.txt";

    CodexCostHistorySnapshot tooManyFiles;
    tooManyFiles.trackingStartedAtUnixSeconds = 1'774'999'000;
    tooManyFiles.updatedAtUnixSeconds = 1'775'000'000;
    tooManyFiles.files.reserve(kCodexCostHistoryCacheMaximumFiles + 1);
    for (std::size_t index = 0;
         index <= kCodexCostHistoryCacheMaximumFiles; ++index) {
        CodexCostHistoryFileSnapshot file;
        file.fileId = WindowsFileId(index + 1);
        file.complete = true;
        file.parser.currentModel = "unknown";
        tooManyFiles.files.push_back(std::move(file));
    }
    Require(CodexCostHistoryStore(path).Save(tooManyFiles).status ==
                CodexCostHistorySaveStatus::kTooLarge &&
                !std::filesystem::exists(path),
            "more than 4096 file states must never be written");

    CodexCostHistorySnapshot tooManyRows;
    tooManyRows.trackingStartedAtUnixSeconds = 1'774'999'000;
    tooManyRows.updatedAtUnixSeconds = 1'775'000'000;
    CodexCostHistoryFileSnapshot file;
    file.fileId = WindowsFileId(1);
    file.complete = true;
    file.parser.currentModel = "unknown";
    file.rows.resize(kCodexCostHistoryCacheMaximumRows + 1);
    tooManyRows.files.push_back(std::move(file));
    Require(CodexCostHistoryStore(path).Save(tooManyRows).status ==
                CodexCostHistorySaveStatus::kTooLarge &&
                !std::filesystem::exists(path),
            "more than 32768 aggregate rows must never be written");

    CodexCostHistorySnapshot tooManyBytes;
    tooManyBytes.trackingStartedAtUnixSeconds = 1'774'999'000;
    tooManyBytes.updatedAtUnixSeconds = 1'775'000'000;
    CodexCostHistoryFileSnapshot largeFile;
    largeFile.fileId = WindowsFileId(2);
    largeFile.complete = true;
    largeFile.parser.currentModel = "unknown";
    largeFile.rows.reserve(kCodexCostHistoryCacheMaximumRows);
    for (std::size_t index = 0; index < kCodexCostHistoryCacheMaximumRows;
         ++index) {
        std::ostringstream model;
        model << 'm' << std::setw(6) << std::setfill('0') << index;
        std::string modelName = model.str();
        modelName.append(128 - modelName.size(), 'x');
        largeFile.rows.push_back(
            Row("2026-08-11", std::move(modelName), 1, 1, 0.000001, 2));
    }
    tooManyBytes.files.push_back(std::move(largeFile));
    Require(CodexCostHistoryStore(path).Save(tooManyBytes).status ==
                CodexCostHistorySaveStatus::kTooLarge &&
                !std::filesystem::exists(path),
            "valid counts must still respect the serialized eight MiB cap");
}

void TestInvalidPrivacyFieldsNeverReachDisk() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "codex-cost-cache.txt";
    CodexCostHistorySnapshot snapshot = Snapshot();
    snapshot.files.front().fileId = "C:\\Users\\Alice\\.codex\\secret.jsonl";
    Require(CodexCostHistoryStore(path).Save(snapshot).status ==
                CodexCostHistorySaveStatus::kInvalidSnapshot &&
                !std::filesystem::exists(path),
            "a source path must not be accepted as a file identity");

    snapshot = Snapshot();
    snapshot.files.front().rows.front().model = "gpt-5.6-sol\tprivate";
    Require(CodexCostHistoryStore(path).Save(snapshot).status ==
                CodexCostHistorySaveStatus::kInvalidSnapshot &&
                !std::filesystem::exists(path),
            "a model field must not inject private or structural data");

    snapshot = Snapshot();
    snapshot.files.front().parser.hasRawTotalsWatermark = false;
    Require(CodexCostHistoryStore(path).Save(snapshot).status ==
                CodexCostHistorySaveStatus::kInvalidSnapshot &&
                !std::filesystem::exists(path),
            "disabled watermark state must not hide nonzero counters");

    snapshot = Snapshot();
    snapshot.files.front().rows.front().cachedPricedTokens = 0;
    Require(CodexCostHistoryStore(path).Save(snapshot).status ==
                CodexCostHistorySaveStatus::kInvalidSnapshot &&
                !std::filesystem::exists(path),
            "a positive frozen cost without priced tokens must not be stored");

    snapshot = Snapshot();
    snapshot.files.push_back(snapshot.files.front());
    Require(CodexCostHistoryStore(path).Save(snapshot).status ==
                CodexCostHistorySaveStatus::kInvalidSnapshot &&
                !std::filesystem::exists(path),
            "duplicate file identities must reject the whole snapshot");

    snapshot = Snapshot();
    snapshot.files.front().rows.push_back(snapshot.files.front().rows.front());
    Require(CodexCostHistoryStore(path).Save(snapshot).status ==
                CodexCostHistorySaveStatus::kInvalidSnapshot &&
                !std::filesystem::exists(path),
            "duplicate date and model rows must reject the whole snapshot");
}

void TestHistoryStateSnapshotRoundTripAndIncrementalResume() {
    const CodexCostHistorySnapshot seed = Snapshot();
    CodexCostHistoryState state;
    Require(state.ImportSnapshot(seed),
            "history state must accept a fully validated cache snapshot");

    const auto exported = state.ExportSnapshot(seed.updatedAtUnixSeconds);
    Require(exported && SameSnapshot(seed, *exported),
            "history export must preserve cursors, parser watermarks, and frozen rows");
    const auto cursors = state.Cursors();
    const auto winCursor = std::find_if(
        cursors.begin(), cursors.end(), [](const CodexCostFileCursor& cursor) {
            return cursor.fileId == WindowsFileId(2);
        });
    Require(winCursor != cursors.end() && winCursor->complete &&
                winCursor->parsedOffsetBytes == 4096 &&
                !winCursor->resetAfterTruncation,
            "imported cursor must resume at the durable offset without replaying reset");

    TemporaryDirectory temporary;
    const auto cachePath = temporary.path() / "state-round-trip-cache.txt";
    Require(CodexCostHistoryStore(cachePath).Save(*exported).written(),
            "an exported state snapshot must pass strict store validation");
    const auto loaded = CodexCostHistoryStore(cachePath).Load();
    Require(loaded.status == CodexCostHistoryLoadStatus::kOk,
            "the persisted state snapshot must load before restoration");
    CodexCostHistoryState restored;
    Require(restored.ImportSnapshot(loaded.snapshot),
            "a store-loaded snapshot must import into a fresh state");
    CodexCostFileScanResult failedScan;
    failedScan.status = CodexCostFileScanStatus::kIoError;
    const auto lastKnown = restored.Apply(
        failedScan, [](std::int64_t) -> std::optional<std::string> {
            return "2026-08-11";
        });
    Require(lastKnown.events.size() == 2,
            "a failed scan after restore must retain every frozen aggregate");
    const auto frozen = std::find_if(
        lastKnown.events.begin(), lastKnown.events.end(),
        [](const CodexCostEvent& event) {
            return event.model == "gpt-5.6-sol";
        });
    Require(frozen != lastKnown.events.end() && frozen->cachedEstimatedUsd &&
                *frozen->cachedEstimatedUsd == 0.00375 &&
                frozen->cachedPricedTokens &&
                *frozen->cachedPricedTokens == 1100 &&
                frozen->fingerprint ==
                    WindowsFileId(2) + "|2026-08-11|gpt-5.6-sol",
            "restore must reconstruct the deterministic fingerprint and frozen cost");

    CodexCostFileScanResult incremental;
    incremental.status = CodexCostFileScanStatus::kOk;
    incremental.files = restored.Cursors();
    const auto updatedCursor = std::find_if(
        incremental.files.begin(), incremental.files.end(),
        [](const CodexCostFileCursor& cursor) {
            return cursor.fileId == WindowsFileId(2);
        });
    Require(updatedCursor != incremental.files.end(),
            "incremental fixture must find the restored Windows cursor");
    updatedCursor->observedSizeBytes = 4400;
    updatedCursor->modifiedUnixNanoseconds += 100;
    updatedCursor->parsedOffsetBytes = 4400;
    updatedCursor->complete = true;
    const std::string tokenLine =
        R"({"timestamp":"2026-08-11T10:00:00Z","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":1100,"cached_input_tokens":300,"cache_write_input_tokens":140,"output_tokens":120}}}})";
    incremental.lines.push_back(
        {WindowsFileId(2), 4096, 4400, tokenLine});
    const auto applied = restored.Apply(
        incremental, [](std::int64_t) -> std::optional<std::string> {
            return "2026-08-11";
        });
    const auto resumed = std::find_if(
        applied.events.begin(), applied.events.end(),
        [](const CodexCostEvent& event) {
            return event.model == "gpt-5.6-sol";
        });
    Require(resumed != applied.events.end() &&
                resumed->usage.inputTokens == 1100 &&
                resumed->usage.cachedInputTokens == 300 &&
                resumed->usage.cacheWriteInputTokens == 140 &&
                resumed->usage.outputTokens == 120 &&
                resumed->cachedPricedTokens &&
                *resumed->cachedPricedTokens == 1220 &&
                resumed->cachedEstimatedUsd &&
                *resumed->cachedEstimatedUsd > 0.00375,
            "restored watermark must count only the new cumulative delta");

    const auto resumedSnapshot =
        restored.ExportSnapshot(seed.updatedAtUnixSeconds + 300);
    Require(resumedSnapshot.has_value(),
            "state must remain exportable after an incremental update");
    const CodexCostHistoryFileSnapshot* resumedFile =
        FindFile(*resumedSnapshot, WindowsFileId(2));
    Require(resumedFile && resumedFile->parsedOffsetBytes == 4400 &&
                resumedFile->parser.rawTotalsWatermark.inputTokens == 1100 &&
                resumedFile->parser.rawTotalsWatermark.outputTokens == 120,
            "the next snapshot must advance both cursor and cumulative watermark");
}

void TestInvalidSnapshotImportIsTransactional() {
    CodexCostHistoryState state;
    const CodexCostHistorySnapshot seed = Snapshot();
    Require(state.ImportSnapshot(seed),
            "transactional rejection fixture must import a baseline");
    const auto before = state.ExportSnapshot(seed.updatedAtUnixSeconds);
    Require(before.has_value(),
            "transactional rejection fixture must export a baseline");

    const auto RequireRejectedWithoutMutation =
        [&](const CodexCostHistorySnapshot& candidate,
            std::string_view message) {
            Require(!state.ImportSnapshot(candidate), message);
            const auto after =
                state.ExportSnapshot(seed.updatedAtUnixSeconds);
            Require(after && SameSnapshot(*before, *after),
                    "a rejected import must leave every prior field unchanged");
        };

    CodexCostHistorySnapshot invalid = seed;
    invalid.files.front().parsedOffsetBytes =
        invalid.files.front().observedSizeBytes + 1;
    invalid.files.front().complete = false;
    RequireRejectedWithoutMutation(
        invalid, "an offset beyond observed size must be rejected");

    invalid = seed;
    invalid.files.push_back(invalid.files.front());
    RequireRejectedWithoutMutation(
        invalid, "duplicate file identities must be rejected transactionally");

    invalid = seed;
    invalid.files.front().rows.push_back(invalid.files.front().rows.front());
    RequireRejectedWithoutMutation(
        invalid, "duplicate compacted row keys must be rejected transactionally");

    invalid = seed;
    invalid.files.front().rows.front().cachedPricedTokens =
        std::numeric_limits<std::int64_t>::max();
    RequireRejectedWithoutMutation(
        invalid, "priced tokens beyond the counted total must be rejected");

    invalid = seed;
    invalid.files.front().rows.front().cachedPricedTokens = 0;
    RequireRejectedWithoutMutation(
        invalid,
        "a positive frozen cost without priced tokens must be rejected");

    invalid = seed;
    invalid.files.front().parser.hasRawTotalsWatermark = false;
    RequireRejectedWithoutMutation(
        invalid, "a hidden nonzero parser watermark must be rejected");

    invalid = seed;
    invalid.files.front().parser.currentModel = "gpt-5.6-sol/private";
    RequireRejectedWithoutMutation(
        invalid, "a non-whitelisted parser model must be rejected");

    Require(!state.ExportSnapshot(-1),
            "an invalid capture timestamp must not produce a snapshot");
    const auto afterInvalidExport =
        state.ExportSnapshot(seed.updatedAtUnixSeconds);
    Require(afterInvalidExport && SameSnapshot(*before, *afterInvalidExport),
            "a failed export must not mutate live state");

    CodexCostHistorySnapshot empty;
    empty.trackingStartedAtUnixSeconds = seed.trackingStartedAtUnixSeconds;
    empty.updatedAtUnixSeconds = seed.updatedAtUnixSeconds;
    Require(state.ImportSnapshot(empty) && state.Cursors().empty(),
            "a valid empty cache must atomically clear restored state");
}

void TestOneScanResetSignalIsNeverDurable() {
    CodexCostHistoryState state;
    Require(state.ImportSnapshot(Snapshot()),
            "reset durability fixture must import a baseline");

    CodexCostFileCursor resetCursor;
    resetCursor.fileId = WindowsFileId(2);
    resetCursor.observedSizeBytes = 200;
    resetCursor.modifiedUnixNanoseconds = 1'775'000'001'000'000'000LL;
    resetCursor.parsedOffsetBytes = 200;
    resetCursor.complete = true;
    resetCursor.resetAfterTruncation = true;
    const std::string replacement =
        R"({"timestamp":"2026-08-11T12:00:00Z","type":"event_msg","payload":{"type":"token_count","info":{"model":"gpt-5.6-terra","last_token_usage":{"input_tokens":10,"output_tokens":2}}}})";
    CodexCostFileScanResult scan;
    scan.status = CodexCostFileScanStatus::kOk;
    scan.files.push_back(resetCursor);
    scan.lines.push_back({resetCursor.fileId, 0, 200, replacement});
    const auto applied = state.Apply(
        scan, [](std::int64_t) -> std::optional<std::string> {
            return "2026-08-11";
        });
    Require(applied.events.size() == 1 && state.Cursors().size() == 1 &&
                state.Cursors().front().resetAfterTruncation,
            "live state must observe the scanner's one-pass reset signal");

    const auto exported = state.ExportSnapshot(1'775'000'000);
    Require(exported && exported->files.size() == 1,
            "state with a completed reset pass must remain exportable");
    CodexCostHistoryState restored;
    Require(restored.ImportSnapshot(*exported) &&
                restored.Cursors().size() == 1 &&
                !restored.Cursors().front().resetAfterTruncation,
            "resetAfterTruncation must not replay after process restart");
}

void TestMissingCacheIsAnEmptySuccess() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "missing-cache.txt";
    const auto loaded = CodexCostHistoryStore(path).Load();
    Require(loaded.status == CodexCostHistoryLoadStatus::kNotFound &&
                loaded.snapshot.files.empty() && loaded.ok(),
            "a first run must treat a missing cache as normal");
}

}  // namespace

int main() {
    TestRoundTripAndPrivacyWhitelist();
    TestUnknownVersionIsNeverOverwritten();
    TestLegacyVersionCanBeReplacedByInstallBaseline();
    TestAtomicFailureAndCancellationPreserveOldFile();
    TestAnyCurrentVersionDamageRejectsTheWholeCache();
    TestLoadByteAndLineLimits();
    TestSaveCountAndSerializedByteLimits();
    TestInvalidPrivacyFieldsNeverReachDisk();
    TestHistoryStateSnapshotRoundTripAndIncrementalResume();
    TestInvalidSnapshotImportIsTransactional();
    TestOneScanResetSignalIsNeverDurable();
    TestMissingCacheIsAnEmptySuccess();
    std::cout << "codex_cost_history_store_test: pass\n";
    return 0;
}
