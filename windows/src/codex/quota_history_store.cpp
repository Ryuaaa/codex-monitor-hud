#include "codex/quota_history_store.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace codex_monitor::codex {
namespace {

constexpr std::string_view kVersionLine = "version=1";
constexpr std::int64_t kMaximumUnixSeconds = 253402300799LL;
constexpr std::int64_t kRetentionSeconds = 30LL * 24LL * 60LL * 60LL;
constexpr std::int64_t kMinimumAppendIntervalSeconds = 60;
constexpr std::size_t kMaximumSamples = 10000;
constexpr std::size_t kMaximumLineBytes = 1024;

std::atomic<std::uint64_t> gTemporarySequence{1};

bool IsValidUnixSeconds(std::int64_t value) {
    return value >= 0 && value <= kMaximumUnixSeconds;
}

bool IsValidPercent(double value) {
    return std::isfinite(value) && value >= 0.0 && value <= 100.0;
}

bool IsValidWindow(const QuotaHistoryWindowSample& window) {
    if (window.remainingPercent && !IsValidPercent(*window.remainingPercent)) {
        return false;
    }
    if (window.resetsAtUnixSeconds &&
        !IsValidUnixSeconds(*window.resetsAtUnixSeconds)) {
        return false;
    }
    return true;
}

bool IsValidSample(const QuotaHistorySample& sample) {
    if (!IsValidUnixSeconds(sample.capturedAtUnixSeconds) ||
        !IsValidWindow(sample.fiveHour) || !IsValidWindow(sample.weekly)) {
        return false;
    }
    return sample.fiveHour.remainingPercent ||
           sample.fiveHour.resetsAtUnixSeconds ||
           sample.weekly.remainingPercent ||
           sample.weekly.resetsAtUnixSeconds;
}

bool ParseInteger(std::string_view text, std::int64_t& value) {
    if (text.empty()) return false;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto result = std::from_chars(begin, end, value, 10);
    return result.ec == std::errc{} && result.ptr == end &&
           IsValidUnixSeconds(value);
}

bool ParsePercent(std::string_view text, double& value) {
    if (text.empty()) return false;
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    input >> std::noskipws >> value;
    return input && input.peek() == std::char_traits<char>::eof() &&
           IsValidPercent(value);
}

template <typename T, typename Parser>
bool ParseOptional(std::string_view text,
                   std::optional<T>& value,
                   Parser parser) {
    if (text == "-") {
        value.reset();
        return true;
    }
    T parsed{};
    if (!parser(text, parsed)) return false;
    value = parsed;
    return true;
}

std::array<std::string_view, 6> SplitSampleLine(std::string_view line,
                                                bool& validCount) {
    std::array<std::string_view, 6> fields{};
    std::size_t start = 0;
    for (std::size_t fieldIndex = 0; fieldIndex + 1 < fields.size();
         ++fieldIndex) {
        const std::size_t separator = line.find('\t', start);
        if (separator == std::string_view::npos) {
            validCount = false;
            return fields;
        }
        fields[fieldIndex] = line.substr(start, separator - start);
        start = separator + 1;
    }
    fields.back() = line.substr(start);
    validCount = fields.back().find('\t') == std::string_view::npos;
    return fields;
}

bool StripPrefix(std::string_view field,
                 std::string_view prefix,
                 std::string_view& value) {
    if (field.size() < prefix.size() || field.substr(0, prefix.size()) != prefix) {
        return false;
    }
    value = field.substr(prefix.size());
    return true;
}

std::optional<QuotaHistorySample> ParseSampleLine(std::string_view line) {
    bool validCount = false;
    const auto fields = SplitSampleLine(line, validCount);
    if (!validCount || fields[0] != "sample") return std::nullopt;

    constexpr std::array<std::string_view, 5> prefixes = {
        "captured_at=",
        "five_hour_remaining=",
        "five_hour_reset_at=",
        "weekly_remaining=",
        "weekly_reset_at=",
    };
    std::array<std::string_view, 5> values{};
    for (std::size_t index = 0; index < prefixes.size(); ++index) {
        if (!StripPrefix(fields[index + 1], prefixes[index], values[index])) {
            return std::nullopt;
        }
    }

    QuotaHistorySample sample;
    if (!ParseInteger(values[0], sample.capturedAtUnixSeconds) ||
        !ParseOptional<double>(values[1], sample.fiveHour.remainingPercent,
                               ParsePercent) ||
        !ParseOptional<std::int64_t>(values[2],
                                     sample.fiveHour.resetsAtUnixSeconds,
                                     ParseInteger) ||
        !ParseOptional<double>(values[3], sample.weekly.remainingPercent,
                               ParsePercent) ||
        !ParseOptional<std::int64_t>(values[4],
                                     sample.weekly.resetsAtUnixSeconds,
                                     ParseInteger) ||
        !IsValidSample(sample)) {
        return std::nullopt;
    }
    return sample;
}

void SortAndTrim(std::vector<QuotaHistorySample>& samples,
                 std::size_t& discardedSamples) {
    std::stable_sort(samples.begin(), samples.end(),
                     [](const auto& left, const auto& right) {
                         return left.capturedAtUnixSeconds <
                                right.capturedAtUnixSeconds;
                     });
    if (samples.size() <= kMaximumSamples) return;
    const std::size_t excess = samples.size() - kMaximumSamples;
    samples.erase(samples.begin(), samples.begin() +
                                      static_cast<std::ptrdiff_t>(excess));
    discardedSamples += excess;
}

QuotaHistoryLoadResult LoadAt(
    const std::filesystem::path& path,
    std::int64_t nowUnixSeconds,
    std::optional<std::int64_t>* newestParsedAtUnixSeconds = nullptr) {
    QuotaHistoryLoadResult result;
    if (!IsValidUnixSeconds(nowUnixSeconds)) {
        result.status = QuotaHistoryLoadStatus::kInvalidNow;
        return result;
    }
    if (path.empty()) {
        result.status = QuotaHistoryLoadStatus::kIoError;
        result.error = std::make_error_code(std::errc::invalid_argument);
        return result;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::error_code existsError;
        const bool exists = std::filesystem::exists(path, existsError);
        if (!exists && !existsError) {
            result.status = QuotaHistoryLoadStatus::kNotFound;
        } else {
            result.status = QuotaHistoryLoadStatus::kIoError;
            result.error = existsError ? existsError
                                       : std::make_error_code(std::errc::io_error);
        }
        return result;
    }

    std::string line;
    if (!std::getline(input, line)) {
        result.status = input.eof() ? QuotaHistoryLoadStatus::kUnsupportedVersion
                                    : QuotaHistoryLoadStatus::kIoError;
        if (!input.eof()) result.error = std::make_error_code(std::errc::io_error);
        return result;
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line != kVersionLine) {
        result.status = QuotaHistoryLoadStatus::kUnsupportedVersion;
        return result;
    }

    const std::int64_t cutoff = nowUnixSeconds >= kRetentionSeconds
                                    ? nowUnixSeconds - kRetentionSeconds
                                    : 0;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > kMaximumLineBytes) {
            ++result.skippedMalformedLines;
            continue;
        }
        const auto sample = ParseSampleLine(line);
        if (!sample) {
            ++result.skippedMalformedLines;
            continue;
        }
        if (newestParsedAtUnixSeconds &&
            (!*newestParsedAtUnixSeconds ||
             sample->capturedAtUnixSeconds > **newestParsedAtUnixSeconds)) {
            *newestParsedAtUnixSeconds = sample->capturedAtUnixSeconds;
        }
        if (sample->capturedAtUnixSeconds < cutoff ||
            sample->capturedAtUnixSeconds > nowUnixSeconds) {
            ++result.discardedSamples;
            continue;
        }
        result.samples.push_back(*sample);
        if (result.samples.size() >= kMaximumSamples * 2) {
            SortAndTrim(result.samples, result.discardedSamples);
        }
    }
    if (!input.eof()) {
        result.status = QuotaHistoryLoadStatus::kIoError;
        result.error = std::make_error_code(std::errc::io_error);
        result.samples.clear();
        return result;
    }

    SortAndTrim(result.samples, result.discardedSamples);
    result.status = QuotaHistoryLoadStatus::kOk;
    return result;
}

void AppendOptionalPercent(std::ostringstream& output,
                           const std::optional<double>& value) {
    if (value) {
        output << std::setprecision(std::numeric_limits<double>::max_digits10)
               << *value;
    } else {
        output << '-';
    }
}

void AppendOptionalInteger(std::ostringstream& output,
                           const std::optional<std::int64_t>& value) {
    if (value) {
        output << *value;
    } else {
        output << '-';
    }
}

std::string Serialize(const std::vector<QuotaHistorySample>& samples) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << kVersionLine << '\n';
    for (const auto& sample : samples) {
        output << "sample\tcaptured_at=" << sample.capturedAtUnixSeconds
               << "\tfive_hour_remaining=";
        AppendOptionalPercent(output, sample.fiveHour.remainingPercent);
        output << "\tfive_hour_reset_at=";
        AppendOptionalInteger(output, sample.fiveHour.resetsAtUnixSeconds);
        output << "\tweekly_remaining=";
        AppendOptionalPercent(output, sample.weekly.remainingPercent);
        output << "\tweekly_reset_at=";
        AppendOptionalInteger(output, sample.weekly.resetsAtUnixSeconds);
        output << '\n';
    }
    return output.str();
}

std::filesystem::path TemporaryPathFor(const std::filesystem::path& path) {
    std::filesystem::path temporary = path;
    const std::string suffix =
        ".tmp." + std::to_string(gTemporarySequence.fetch_add(1));
    temporary += std::filesystem::path(suffix);
    return temporary;
}

std::error_code DefaultAtomicReplace(const std::filesystem::path& temporary,
                                     const std::filesystem::path& destination) {
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    return error;
}

QuotaHistoryUpdateStatus MapLoadFailure(QuotaHistoryLoadStatus status) {
    return status == QuotaHistoryLoadStatus::kUnsupportedVersion
               ? QuotaHistoryUpdateStatus::kUnsupportedVersion
               : QuotaHistoryUpdateStatus::kIoError;
}

}  // namespace

QuotaHistoryStore::QuotaHistoryStore(
    std::filesystem::path path,
    QuotaHistoryAtomicReplace atomicReplace)
    : path_(std::move(path)), atomicReplace_(std::move(atomicReplace)) {}

QuotaHistoryLoadResult QuotaHistoryStore::Load(
    std::int64_t nowUnixSeconds) const noexcept {
    try {
        return LoadAt(path_, nowUnixSeconds);
    } catch (...) {
        QuotaHistoryLoadResult result;
        result.status = QuotaHistoryLoadStatus::kIoError;
        result.error = std::make_error_code(std::errc::io_error);
        return result;
    }
}

QuotaHistoryLoadResult QuotaHistoryStore::Load() const noexcept {
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    return Load(now);
}

QuotaHistoryUpdateResult QuotaHistoryStore::Update(
    const QuotaHistorySample& sample) const noexcept {
    QuotaHistoryUpdateResult result;
    try {
        if (!IsValidSample(sample)) {
            result.status = QuotaHistoryUpdateStatus::kInvalidSample;
            return result;
        }
        if (path_.empty()) {
            result.status = QuotaHistoryUpdateStatus::kIoError;
            result.error = std::make_error_code(std::errc::invalid_argument);
            return result;
        }

        std::optional<std::int64_t> newestParsedAtUnixSeconds;
        QuotaHistoryLoadResult loaded = LoadAt(
            path_, sample.capturedAtUnixSeconds, &newestParsedAtUnixSeconds);
        if (!loaded.ok()) {
            result.status = MapLoadFailure(loaded.status);
            result.error = loaded.error;
            return result;
        }
        if (newestParsedAtUnixSeconds) {
            const std::int64_t latest = *newestParsedAtUnixSeconds;
            if (sample.capturedAtUnixSeconds < latest) {
                result.status = QuotaHistoryUpdateStatus::kInvalidSample;
                result.storedSampleCount = loaded.samples.size();
                return result;
            }
            if (sample.capturedAtUnixSeconds - latest <
                kMinimumAppendIntervalSeconds) {
                result.status = QuotaHistoryUpdateStatus::kSkippedTooSoon;
                result.storedSampleCount = loaded.samples.size();
                return result;
            }
        }

        loaded.samples.push_back(sample);
        SortAndTrim(loaded.samples, loaded.discardedSamples);
        const std::string contents = Serialize(loaded.samples);

        std::error_code directoryError;
        const std::filesystem::path parent = path_.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, directoryError);
        }
        if (directoryError) {
            result.status = QuotaHistoryUpdateStatus::kIoError;
            result.error = directoryError;
            return result;
        }

        const std::filesystem::path temporary = TemporaryPathFor(path_);
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) {
                result.status = QuotaHistoryUpdateStatus::kIoError;
                result.error = std::make_error_code(std::errc::io_error);
                return result;
            }
            output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            output.flush();
            output.close();
            if (!output) {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                result.status = QuotaHistoryUpdateStatus::kIoError;
                result.error = std::make_error_code(std::errc::io_error);
                return result;
            }
        }

        const std::error_code replaceError = atomicReplace_
                                                 ? atomicReplace_(temporary, path_)
                                                 : DefaultAtomicReplace(temporary, path_);
        if (replaceError) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            result.status = QuotaHistoryUpdateStatus::kIoError;
            result.error = replaceError;
            return result;
        }

        result.status = QuotaHistoryUpdateStatus::kWritten;
        result.storedSampleCount = loaded.samples.size();
        return result;
    } catch (...) {
        result.status = QuotaHistoryUpdateStatus::kIoError;
        result.error = std::make_error_code(std::errc::io_error);
        return result;
    }
}

}  // namespace codex_monitor::codex
