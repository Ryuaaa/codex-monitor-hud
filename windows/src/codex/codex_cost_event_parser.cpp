#include "codex/codex_cost_event_parser.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace codex_monitor::codex {
namespace {

constexpr std::size_t kMaximumJsonDepth = 64;
constexpr std::uint64_t kFnvOffsetBasis = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

struct JsonCursor {
    std::string_view input;
    std::size_t position = 0;

    void SkipWhitespace() noexcept {
        while (position < input.size()) {
            const char value = input[position];
            if (value != ' ' && value != '\t' && value != '\r' &&
                value != '\n') {
                break;
            }
            ++position;
        }
    }

    [[nodiscard]] bool Consume(char expected) noexcept {
        SkipWhitespace();
        if (position >= input.size() || input[position] != expected) {
            return false;
        }
        ++position;
        return true;
    }
};

[[nodiscard]] int HexDigit(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

void AppendUtf8(std::uint32_t value, std::string& output) {
    if (value <= 0x7fU) {
        output.push_back(static_cast<char>(value));
    } else if (value <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (value >> 6U)));
        output.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
    } else if (value <= 0xffffU) {
        output.push_back(static_cast<char>(0xe0U | (value >> 12U)));
        output.push_back(
            static_cast<char>(0x80U | ((value >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
    } else {
        output.push_back(static_cast<char>(0xf0U | (value >> 18U)));
        output.push_back(
            static_cast<char>(0x80U | ((value >> 12U) & 0x3fU)));
        output.push_back(
            static_cast<char>(0x80U | ((value >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
    }
}

[[nodiscard]] bool ParseHexCodeUnit(JsonCursor& cursor,
                                    std::uint32_t& output) noexcept {
    if (cursor.position + 4 > cursor.input.size()) return false;
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index) {
        const int digit = HexDigit(cursor.input[cursor.position++]);
        if (digit < 0) return false;
        value = value * 16U + static_cast<std::uint32_t>(digit);
    }
    output = value;
    return true;
}

[[nodiscard]] bool ParseJsonString(JsonCursor& cursor, std::string* output) {
    cursor.SkipWhitespace();
    if (cursor.position >= cursor.input.size() ||
        cursor.input[cursor.position] != '"') {
        return false;
    }
    ++cursor.position;
    if (output) output->clear();

    while (cursor.position < cursor.input.size()) {
        const unsigned char value =
            static_cast<unsigned char>(cursor.input[cursor.position++]);
        if (value == '"') return true;
        if (value < 0x20U) return false;
        if (value != '\\') {
            if (output) output->push_back(static_cast<char>(value));
            continue;
        }
        if (cursor.position >= cursor.input.size()) return false;
        const char escape = cursor.input[cursor.position++];
        switch (escape) {
            case '"':
            case '\\':
            case '/':
                if (output) output->push_back(escape);
                break;
            case 'b':
                if (output) output->push_back('\b');
                break;
            case 'f':
                if (output) output->push_back('\f');
                break;
            case 'n':
                if (output) output->push_back('\n');
                break;
            case 'r':
                if (output) output->push_back('\r');
                break;
            case 't':
                if (output) output->push_back('\t');
                break;
            case 'u': {
                std::uint32_t first = 0;
                if (!ParseHexCodeUnit(cursor, first)) return false;
                std::uint32_t scalar = first;
                if (first >= 0xd800U && first <= 0xdbffU) {
                    if (cursor.position + 2 > cursor.input.size() ||
                        cursor.input[cursor.position] != '\\' ||
                        cursor.input[cursor.position + 1] != 'u') {
                        return false;
                    }
                    cursor.position += 2;
                    std::uint32_t second = 0;
                    if (!ParseHexCodeUnit(cursor, second) ||
                        second < 0xdc00U || second > 0xdfffU) {
                        return false;
                    }
                    scalar = 0x10000U + ((first - 0xd800U) << 10U) +
                             (second - 0xdc00U);
                } else if (first >= 0xdc00U && first <= 0xdfffU) {
                    return false;
                }
                if (output) AppendUtf8(scalar, *output);
                break;
            }
            default:
                return false;
        }
    }
    return false;
}

struct NumberToken {
    std::size_t begin = 0;
    std::size_t end = 0;
    bool integral = true;
};

[[nodiscard]] bool ParseNumberToken(JsonCursor& cursor,
                                    NumberToken& output) noexcept {
    cursor.SkipWhitespace();
    const std::size_t begin = cursor.position;
    if (cursor.position < cursor.input.size() &&
        cursor.input[cursor.position] == '-') {
        ++cursor.position;
    }
    if (cursor.position >= cursor.input.size()) return false;
    if (cursor.input[cursor.position] == '0') {
        ++cursor.position;
        if (cursor.position < cursor.input.size() &&
            std::isdigit(static_cast<unsigned char>(
                cursor.input[cursor.position]))) {
            return false;
        }
    } else if (cursor.input[cursor.position] >= '1' &&
               cursor.input[cursor.position] <= '9') {
        do {
            ++cursor.position;
        } while (cursor.position < cursor.input.size() &&
                 std::isdigit(static_cast<unsigned char>(
                     cursor.input[cursor.position])));
    } else {
        return false;
    }

    bool integral = true;
    if (cursor.position < cursor.input.size() &&
        cursor.input[cursor.position] == '.') {
        integral = false;
        ++cursor.position;
        const std::size_t fractionStart = cursor.position;
        while (cursor.position < cursor.input.size() &&
               std::isdigit(static_cast<unsigned char>(
                   cursor.input[cursor.position]))) {
            ++cursor.position;
        }
        if (cursor.position == fractionStart) return false;
    }
    if (cursor.position < cursor.input.size() &&
        (cursor.input[cursor.position] == 'e' ||
         cursor.input[cursor.position] == 'E')) {
        integral = false;
        ++cursor.position;
        if (cursor.position < cursor.input.size() &&
            (cursor.input[cursor.position] == '+' ||
             cursor.input[cursor.position] == '-')) {
            ++cursor.position;
        }
        const std::size_t exponentStart = cursor.position;
        while (cursor.position < cursor.input.size() &&
               std::isdigit(static_cast<unsigned char>(
                   cursor.input[cursor.position]))) {
            ++cursor.position;
        }
        if (cursor.position == exponentStart) return false;
    }
    output = NumberToken{begin, cursor.position, integral};
    return true;
}

[[nodiscard]] bool ParseInt64(std::string_view token,
                              std::int64_t& output) noexcept {
    if (token.empty()) return false;
    std::size_t position = 0;
    const bool negative = token[position] == '-';
    if (negative && ++position == token.size()) return false;

    const std::uint64_t negativeLimit =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) +
        1ULL;
    const std::uint64_t limit =
        negative ? negativeLimit
                 : static_cast<std::uint64_t>(
                       std::numeric_limits<std::int64_t>::max());
    std::uint64_t value = 0;
    for (; position < token.size(); ++position) {
        const char digitCharacter = token[position];
        if (digitCharacter < '0' || digitCharacter > '9') return false;
        const std::uint64_t digit =
            static_cast<std::uint64_t>(digitCharacter - '0');
        if (value > (limit - digit) / 10ULL) return false;
        value = value * 10ULL + digit;
    }
    if (negative) {
        output = value == negativeLimit
                     ? std::numeric_limits<std::int64_t>::min()
                     : -static_cast<std::int64_t>(value);
    } else {
        output = static_cast<std::int64_t>(value);
    }
    return true;
}

[[nodiscard]] bool SkipJsonValue(JsonCursor& cursor, std::size_t depth);

[[nodiscard]] bool SkipJsonObject(JsonCursor& cursor, std::size_t depth) {
    if (depth > kMaximumJsonDepth || !cursor.Consume('{')) return false;
    cursor.SkipWhitespace();
    if (cursor.position < cursor.input.size() &&
        cursor.input[cursor.position] == '}') {
        ++cursor.position;
        return true;
    }
    while (true) {
        if (!ParseJsonString(cursor, nullptr) || !cursor.Consume(':') ||
            !SkipJsonValue(cursor, depth + 1)) {
            return false;
        }
        cursor.SkipWhitespace();
        if (cursor.position >= cursor.input.size()) return false;
        const char separator = cursor.input[cursor.position++];
        if (separator == '}') return true;
        if (separator != ',') return false;
    }
}

[[nodiscard]] bool SkipJsonArray(JsonCursor& cursor, std::size_t depth) {
    if (depth > kMaximumJsonDepth || !cursor.Consume('[')) return false;
    cursor.SkipWhitespace();
    if (cursor.position < cursor.input.size() &&
        cursor.input[cursor.position] == ']') {
        ++cursor.position;
        return true;
    }
    while (true) {
        if (!SkipJsonValue(cursor, depth + 1)) return false;
        cursor.SkipWhitespace();
        if (cursor.position >= cursor.input.size()) return false;
        const char separator = cursor.input[cursor.position++];
        if (separator == ']') return true;
        if (separator != ',') return false;
    }
}

[[nodiscard]] bool ConsumeLiteral(JsonCursor& cursor,
                                  std::string_view literal) noexcept {
    if (cursor.input.substr(cursor.position, literal.size()) != literal) {
        return false;
    }
    cursor.position += literal.size();
    return true;
}

[[nodiscard]] bool SkipJsonValue(JsonCursor& cursor, std::size_t depth) {
    if (depth > kMaximumJsonDepth) return false;
    cursor.SkipWhitespace();
    if (cursor.position >= cursor.input.size()) return false;
    switch (cursor.input[cursor.position]) {
        case '{':
            return SkipJsonObject(cursor, depth);
        case '[':
            return SkipJsonArray(cursor, depth);
        case '"':
            return ParseJsonString(cursor, nullptr);
        case 't':
            return ConsumeLiteral(cursor, "true");
        case 'f':
            return ConsumeLiteral(cursor, "false");
        case 'n':
            return ConsumeLiteral(cursor, "null");
        default: {
            NumberToken ignored;
            return ParseNumberToken(cursor, ignored);
        }
    }
}

struct UsageRecord {
    bool valid = true;
    std::optional<std::int64_t> input;
    std::optional<std::int64_t> cached;
    std::optional<std::int64_t> cacheWrite;
    std::optional<std::int64_t> cacheCreation;
    std::optional<std::int64_t> output;
};

struct InfoRecord {
    std::optional<std::string> model;
    std::optional<UsageRecord> lastUsage;
    std::optional<UsageRecord> totalUsage;
};

struct PayloadRecord {
    std::optional<std::string> type;
    std::optional<std::string> model;
    std::optional<InfoRecord> info;
};

struct RootRecord {
    std::optional<std::string> type;
    std::optional<std::string> timestamp;
    std::optional<PayloadRecord> payload;
};

[[nodiscard]] bool ParseOptionalString(JsonCursor& cursor,
                                       std::optional<std::string>& output) {
    output.reset();
    cursor.SkipWhitespace();
    if (cursor.position < cursor.input.size() &&
        cursor.input[cursor.position] == '"') {
        std::string value;
        if (!ParseJsonString(cursor, &value)) return false;
        output = std::move(value);
        return true;
    }
    return SkipJsonValue(cursor, 1);
}

[[nodiscard]] bool ParseIntegerField(
    JsonCursor& cursor,
    std::optional<std::int64_t>& output,
    bool& recordValid) {
    output.reset();
    cursor.SkipWhitespace();
    if (cursor.position >= cursor.input.size()) return false;
    const char first = cursor.input[cursor.position];
    if (first != '-' && (first < '0' || first > '9')) {
        recordValid = false;
        return SkipJsonValue(cursor, 1);
    }
    NumberToken token;
    if (!ParseNumberToken(cursor, token)) return false;
    std::int64_t value = 0;
    if (!token.integral ||
        !ParseInt64(cursor.input.substr(token.begin, token.end - token.begin),
                    value)) {
        recordValid = false;
        return true;
    }
    output = value;
    return true;
}

[[nodiscard]] bool ParseUsageObject(JsonCursor& cursor, UsageRecord& output) {
    if (!cursor.Consume('{')) return false;
    cursor.SkipWhitespace();
    if (cursor.position < cursor.input.size() &&
        cursor.input[cursor.position] == '}') {
        ++cursor.position;
        return true;
    }
    while (true) {
        std::string key;
        if (!ParseJsonString(cursor, &key) || !cursor.Consume(':')) {
            return false;
        }
        bool parsed = true;
        if (key == "input_tokens") {
            parsed = ParseIntegerField(cursor, output.input, output.valid);
        } else if (key == "cached_input_tokens") {
            parsed = ParseIntegerField(cursor, output.cached, output.valid);
        } else if (key == "cache_write_input_tokens") {
            parsed = ParseIntegerField(cursor, output.cacheWrite, output.valid);
        } else if (key == "cache_creation_input_tokens") {
            parsed =
                ParseIntegerField(cursor, output.cacheCreation, output.valid);
        } else if (key == "output_tokens") {
            parsed = ParseIntegerField(cursor, output.output, output.valid);
        } else {
            parsed = SkipJsonValue(cursor, 2);
        }
        if (!parsed) return false;
        cursor.SkipWhitespace();
        if (cursor.position >= cursor.input.size()) return false;
        const char separator = cursor.input[cursor.position++];
        if (separator == '}') return true;
        if (separator != ',') return false;
    }
}

[[nodiscard]] bool ParseOptionalUsage(JsonCursor& cursor,
                                      std::optional<UsageRecord>& output) {
    output.reset();
    cursor.SkipWhitespace();
    if (cursor.position < cursor.input.size() &&
        cursor.input[cursor.position] == '{') {
        UsageRecord value;
        if (!ParseUsageObject(cursor, value)) return false;
        output = std::move(value);
        return true;
    }
    return SkipJsonValue(cursor, 2);
}

[[nodiscard]] bool ParseInfoObject(JsonCursor& cursor, InfoRecord& output) {
    if (!cursor.Consume('{')) return false;
    cursor.SkipWhitespace();
    if (cursor.position < cursor.input.size() &&
        cursor.input[cursor.position] == '}') {
        ++cursor.position;
        return true;
    }
    while (true) {
        std::string key;
        if (!ParseJsonString(cursor, &key) || !cursor.Consume(':')) {
            return false;
        }
        bool parsed = true;
        if (key == "model") {
            parsed = ParseOptionalString(cursor, output.model);
        } else if (key == "last_token_usage") {
            parsed = ParseOptionalUsage(cursor, output.lastUsage);
        } else if (key == "total_token_usage") {
            parsed = ParseOptionalUsage(cursor, output.totalUsage);
        } else {
            parsed = SkipJsonValue(cursor, 2);
        }
        if (!parsed) return false;
        cursor.SkipWhitespace();
        if (cursor.position >= cursor.input.size()) return false;
        const char separator = cursor.input[cursor.position++];
        if (separator == '}') return true;
        if (separator != ',') return false;
    }
}

[[nodiscard]] bool ParseOptionalInfo(JsonCursor& cursor,
                                     std::optional<InfoRecord>& output) {
    output.reset();
    cursor.SkipWhitespace();
    if (cursor.position < cursor.input.size() &&
        cursor.input[cursor.position] == '{') {
        InfoRecord value;
        if (!ParseInfoObject(cursor, value)) return false;
        output = std::move(value);
        return true;
    }
    return SkipJsonValue(cursor, 1);
}

[[nodiscard]] bool ParsePayloadObject(JsonCursor& cursor,
                                      PayloadRecord& output) {
    if (!cursor.Consume('{')) return false;
    cursor.SkipWhitespace();
    if (cursor.position < cursor.input.size() &&
        cursor.input[cursor.position] == '}') {
        ++cursor.position;
        return true;
    }
    while (true) {
        std::string key;
        if (!ParseJsonString(cursor, &key) || !cursor.Consume(':')) {
            return false;
        }
        bool parsed = true;
        if (key == "type") {
            parsed = ParseOptionalString(cursor, output.type);
        } else if (key == "model") {
            parsed = ParseOptionalString(cursor, output.model);
        } else if (key == "info") {
            parsed = ParseOptionalInfo(cursor, output.info);
        } else {
            parsed = SkipJsonValue(cursor, 1);
        }
        if (!parsed) return false;
        cursor.SkipWhitespace();
        if (cursor.position >= cursor.input.size()) return false;
        const char separator = cursor.input[cursor.position++];
        if (separator == '}') return true;
        if (separator != ',') return false;
    }
}

[[nodiscard]] bool ParseOptionalPayload(JsonCursor& cursor,
                                        std::optional<PayloadRecord>& output) {
    output.reset();
    cursor.SkipWhitespace();
    if (cursor.position < cursor.input.size() &&
        cursor.input[cursor.position] == '{') {
        PayloadRecord value;
        if (!ParsePayloadObject(cursor, value)) return false;
        output = std::move(value);
        return true;
    }
    return SkipJsonValue(cursor, 0);
}

[[nodiscard]] bool ParseRootRecord(std::string_view line, RootRecord& output) {
    JsonCursor cursor{line};
    if (!cursor.Consume('{')) return false;
    cursor.SkipWhitespace();
    if (cursor.position < cursor.input.size() &&
        cursor.input[cursor.position] == '}') {
        ++cursor.position;
        cursor.SkipWhitespace();
        return cursor.position == cursor.input.size();
    }
    while (true) {
        std::string key;
        if (!ParseJsonString(cursor, &key) || !cursor.Consume(':')) {
            return false;
        }
        bool parsed = true;
        if (key == "type") {
            parsed = ParseOptionalString(cursor, output.type);
        } else if (key == "timestamp") {
            parsed = ParseOptionalString(cursor, output.timestamp);
        } else if (key == "payload") {
            parsed = ParseOptionalPayload(cursor, output.payload);
        } else {
            parsed = SkipJsonValue(cursor, 0);
        }
        if (!parsed) return false;
        cursor.SkipWhitespace();
        if (cursor.position >= cursor.input.size()) return false;
        const char separator = cursor.input[cursor.position++];
        if (separator == '}') break;
        if (separator != ',') return false;
    }
    cursor.SkipWhitespace();
    return cursor.position == cursor.input.size();
}

[[nodiscard]] std::int64_t NonNegative(std::int64_t value) noexcept {
    return value < 0 ? 0 : value;
}

[[nodiscard]] std::optional<CodexTokenUsage> BuildTokenTuple(
    const std::optional<UsageRecord>& raw) noexcept {
    if (!raw || !raw->valid) return std::nullopt;
    const std::int64_t input = raw->input.value_or(0);
    const std::int64_t cached = raw->cached.value_or(0);
    std::int64_t write = raw->cacheWrite.value_or(0);
    if (write == 0) write = raw->cacheCreation.value_or(0);
    const std::int64_t output = raw->output.value_or(0);
    if (input <= 0 && cached <= 0 && write <= 0 && output <= 0) {
        return std::nullopt;
    }
    return CodexTokenUsage{NonNegative(input), NonNegative(cached),
                           NonNegative(write), NonNegative(output)};
}

[[nodiscard]] CodexTokenUsage DeltaAboveWatermark(
    const CodexTokenUsage& current,
    const CodexTokenUsage& watermark) noexcept {
    const auto delta = [](std::int64_t value,
                          std::int64_t prior) noexcept -> std::int64_t {
        return value > prior ? value - prior : 0;
    };
    return CodexTokenUsage{
        delta(current.inputTokens, watermark.inputTokens),
        delta(current.cachedInputTokens, watermark.cachedInputTokens),
        delta(current.cacheWriteInputTokens,
              watermark.cacheWriteInputTokens),
        delta(current.outputTokens, watermark.outputTokens),
    };
}

[[nodiscard]] CodexTokenUsage UpdatedWatermark(
    const CodexTokenUsage& current,
    const CodexTokenUsage& watermark) noexcept {
    return CodexTokenUsage{
        std::max(current.inputTokens, watermark.inputTokens),
        std::max(current.cachedInputTokens, watermark.cachedInputTokens),
        std::max(current.cacheWriteInputTokens,
                 watermark.cacheWriteInputTokens),
        std::max(current.outputTokens, watermark.outputTokens),
    };
}

[[nodiscard]] bool HasCountableTokens(
    const CodexTokenUsage& usage) noexcept {
    return usage.inputTokens > 0 || usage.outputTokens > 0;
}

[[nodiscard]] bool IsLeapYear(int year) noexcept {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

[[nodiscard]] int DaysInMonth(int year, int month) noexcept {
    static constexpr std::array<int, 13> kDays = {
        0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };
    if (month < 1 || month > 12) return 0;
    return month == 2 && IsLeapYear(year)
               ? 29
               : kDays[static_cast<std::size_t>(month)];
}

[[nodiscard]] bool ParseFixedDigits(std::string_view text,
                                    std::size_t& position,
                                    std::size_t count,
                                    int& output) noexcept {
    if (position + count > text.size()) return false;
    int value = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const char digit = text[position++];
        if (digit < '0' || digit > '9') return false;
        value = value * 10 + static_cast<int>(digit - '0');
    }
    output = value;
    return true;
}

// Howard Hinnant's civil-date conversion, shifted to the Unix epoch.
[[nodiscard]] std::int64_t DaysFromCivil(int year,
                                         unsigned month,
                                         unsigned day) noexcept {
    year -= month <= 2U;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yearOfEra =
        static_cast<unsigned>(year - era * 400);
    const unsigned adjustedMonth = month > 2U ? month - 3U : month + 9U;
    const unsigned dayOfYear =
        (153U * adjustedMonth + 2U) /
            5U +
        day - 1U;
    const unsigned dayOfEra =
        yearOfEra * 365U + yearOfEra / 4U - yearOfEra / 100U + dayOfYear;
    return static_cast<std::int64_t>(era) * 146097LL +
           static_cast<std::int64_t>(dayOfEra) - 719468LL;
}

[[nodiscard]] std::optional<std::int64_t> ParseIso8601Timestamp(
    std::string_view text) noexcept {
    std::size_t position = 0;
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!ParseFixedDigits(text, position, 4, year) ||
        position >= text.size() || text[position++] != '-' ||
        !ParseFixedDigits(text, position, 2, month) ||
        position >= text.size() || text[position++] != '-' ||
        !ParseFixedDigits(text, position, 2, day) ||
        position >= text.size() ||
        (text[position] != 'T' && text[position] != 't')) {
        return std::nullopt;
    }
    ++position;
    if (!ParseFixedDigits(text, position, 2, hour) ||
        position >= text.size() || text[position++] != ':' ||
        !ParseFixedDigits(text, position, 2, minute) ||
        position >= text.size() || text[position++] != ':' ||
        !ParseFixedDigits(text, position, 2, second)) {
        return std::nullopt;
    }
    if (year < 1 || month < 1 || month > 12 || day < 1 ||
        day > DaysInMonth(year, month) || hour > 23 || minute > 59 ||
        second > 59) {
        return std::nullopt;
    }

    int milliseconds = 0;
    if (position < text.size() && text[position] == '.') {
        ++position;
        const std::size_t fractionStart = position;
        int digits = 0;
        while (position < text.size() && text[position] >= '0' &&
               text[position] <= '9') {
            if (digits < 3) {
                milliseconds = milliseconds * 10 +
                               static_cast<int>(text[position] - '0');
            }
            ++digits;
            ++position;
        }
        if (position == fractionStart) return std::nullopt;
        while (digits < 3) {
            milliseconds *= 10;
            ++digits;
        }
    }

    int offsetSeconds = 0;
    if (position < text.size() &&
        (text[position] == 'Z' || text[position] == 'z')) {
        ++position;
    } else if (position < text.size() &&
               (text[position] == '+' || text[position] == '-')) {
        const bool negativeOffset = text[position++] == '-';
        int offsetHour = 0;
        int offsetMinute = 0;
        if (!ParseFixedDigits(text, position, 2, offsetHour) ||
            position >= text.size() || text[position++] != ':' ||
            !ParseFixedDigits(text, position, 2, offsetMinute) ||
            offsetHour > 23 || offsetMinute > 59) {
            return std::nullopt;
        }
        offsetSeconds = offsetHour * 3600 + offsetMinute * 60;
        if (negativeOffset) offsetSeconds = -offsetSeconds;
    } else {
        return std::nullopt;
    }
    if (position != text.size()) return std::nullopt;

    const std::int64_t days =
        DaysFromCivil(year, static_cast<unsigned>(month),
                      static_cast<unsigned>(day));
    const std::int64_t seconds =
        days * 86400LL + static_cast<std::int64_t>(hour) * 3600LL +
        static_cast<std::int64_t>(minute) * 60LL + second - offsetSeconds;
    return seconds * 1000LL + milliseconds;
}

[[nodiscard]] std::uint64_t StableLineHash(std::string_view line) noexcept {
    std::uint64_t hash = kFnvOffsetBasis;
    for (const char character : line) {
        const auto value = static_cast<unsigned char>(character);
        hash ^= value;
        hash *= kFnvPrime;
    }
    return hash;
}

[[nodiscard]] std::string Fingerprint(std::uint64_t hash,
                                      std::uint64_t occurrence) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string output(16, '0');
    for (std::size_t index = 0; index < 16; ++index) {
        const unsigned shift = static_cast<unsigned>((15 - index) * 4);
        output[index] = kHex[(hash >> shift) & 0x0fU];
    }
    output.push_back('#');
    output += std::to_string(occurrence);
    return output;
}

}  // namespace

CodexCostLineParseResult ParseCodexCostJsonlLine(
    std::string_view line,
    CodexCostEventParserState& state) {
    RootRecord root;
    if (!ParseRootRecord(line, root)) {
        return {CodexCostLineDisposition::kMalformed, std::nullopt};
    }
    if (!root.type || !root.payload) {
        return {};
    }

    if (*root.type == "turn_context") {
        if (!root.payload->model || root.payload->model->empty()) return {};
        state.currentModel = NormalizeCodexCostModel(*root.payload->model);
        if (state.currentModel.empty()) state.currentModel = "unknown";
        return {CodexCostLineDisposition::kStateUpdated, std::nullopt};
    }

    if (*root.type != "event_msg" || !root.payload->type ||
        *root.payload->type != "token_count" || !root.payload->info) {
        return {};
    }
    const InfoRecord& info = *root.payload->info;
    const std::optional<CodexTokenUsage> last =
        BuildTokenTuple(info.lastUsage);
    const std::optional<CodexTokenUsage> total =
        BuildTokenTuple(info.totalUsage);

    CodexTokenUsage counted;
    bool stateUpdated = false;
    if (total) {
        counted = state.hasRawTotalsWatermark
                      ? DeltaAboveWatermark(*total,
                                            state.rawTotalsWatermark)
                      : *total;
        state.rawTotalsWatermark =
            state.hasRawTotalsWatermark
                ? UpdatedWatermark(*total, state.rawTotalsWatermark)
                : *total;
        state.hasRawTotalsWatermark = true;
        stateUpdated = true;
    } else if (last) {
        counted = *last;
    } else {
        return {};
    }

    if (!HasCountableTokens(counted) || !root.timestamp) {
        return {stateUpdated ? CodexCostLineDisposition::kStateUpdated
                             : CodexCostLineDisposition::kIgnored,
                std::nullopt};
    }
    const auto timestamp = ParseIso8601Timestamp(*root.timestamp);
    if (!timestamp) {
        return {stateUpdated ? CodexCostLineDisposition::kStateUpdated
                             : CodexCostLineDisposition::kIgnored,
                std::nullopt};
    }

    std::string model = info.model ? NormalizeCodexCostModel(*info.model)
                                   : state.currentModel;
    if (model.empty()) model = "unknown";

    const std::uint64_t hash = StableLineHash(line);
    std::uint64_t& occurrence = state.emittedOccurrences[hash];
    if (occurrence != std::numeric_limits<std::uint64_t>::max()) {
        ++occurrence;
    }
    ParsedCodexCostEvent event;
    event.fingerprint = Fingerprint(hash, occurrence);
    event.timestampUnixMilliseconds = *timestamp;
    event.model = std::move(model);
    event.usage = counted;
    return {CodexCostLineDisposition::kEvent, std::move(event)};
}

}  // namespace codex_monitor::codex
