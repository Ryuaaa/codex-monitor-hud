#include "codex/codex_cost_model.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace codex_monitor::codex {
namespace {

struct Pricing {
    std::string_view model;
    double input;
    double cachedInput;
    double output;
    double cacheWrite;
    double longInput;
    double longCachedInput;
    double longOutput;
    double longCacheWrite;
    std::int64_t longContextThreshold;
};

constexpr double kUseInputRate = -1.0;
constexpr double kNoLongContextRate = 0.0;

// USD per one million tokens. This intentionally mirrors the frozen macOS 1.0
// table in overlay/CodexCostHistory.m, including cache-write fallback pricing.
constexpr std::array<Pricing, 17> kPrices{{
    {"gpt-5", 1.25, 0.125, 10.0, kUseInputRate,
     kNoLongContextRate, 0.0, 0.0, 0.0, 0},
    {"gpt-5-mini", 0.25, 0.025, 2.0, kUseInputRate,
     kNoLongContextRate, 0.0, 0.0, 0.0, 0},
    {"gpt-5-nano", 0.05, 0.005, 0.4, kUseInputRate,
     kNoLongContextRate, 0.0, 0.0, 0.0, 0},
    {"gpt-5-pro", 15.0, 15.0, 120.0, kUseInputRate,
     kNoLongContextRate, 0.0, 0.0, 0.0, 0},
    {"gpt-5.1", 1.25, 0.125, 10.0, kUseInputRate,
     kNoLongContextRate, 0.0, 0.0, 0.0, 0},
    {"gpt-5.2", 1.75, 0.175, 14.0, kUseInputRate,
     kNoLongContextRate, 0.0, 0.0, 0.0, 0},
    {"gpt-5.2-pro", 21.0, 21.0, 168.0, kUseInputRate,
     kNoLongContextRate, 0.0, 0.0, 0.0, 0},
    {"gpt-5.3-codex", 1.75, 0.175, 14.0, kUseInputRate,
     kNoLongContextRate, 0.0, 0.0, 0.0, 0},
    {"gpt-5.4", 2.5, 0.25, 15.0, kUseInputRate,
     5.0, 0.5, 22.5, kUseInputRate, 272000},
    {"gpt-5.4-mini", 0.75, 0.075, 4.5, kUseInputRate,
     kNoLongContextRate, 0.0, 0.0, 0.0, 0},
    {"gpt-5.4-nano", 0.2, 0.02, 1.25, kUseInputRate,
     kNoLongContextRate, 0.0, 0.0, 0.0, 0},
    {"gpt-5.4-pro", 30.0, 30.0, 180.0, kUseInputRate,
     60.0, 60.0, 270.0, kUseInputRate, 272000},
    {"gpt-5.5", 5.0, 0.5, 30.0, kUseInputRate,
     10.0, 1.0, 45.0, kUseInputRate, 272000},
    {"gpt-5.5-pro", 30.0, 30.0, 180.0, kUseInputRate,
     60.0, 60.0, 270.0, kUseInputRate, 272000},
    {"gpt-5.6-sol", 5.0, 0.5, 30.0, 6.25,
     10.0, 1.0, 45.0, 12.5, 272000},
    {"gpt-5.6-terra", 2.0, 0.2, 12.0, 2.5,
     4.0, 0.4, 18.0, 5.0, 272000},
    {"gpt-5.6-luna", 0.2, 0.02, 1.2, 0.25,
     0.4, 0.04, 1.8, 0.5, 272000},
}};

[[nodiscard]] bool IsAsciiWhitespace(char character) noexcept {
    return character == ' ' || character == '\t' || character == '\n' ||
           character == '\r' || character == '\f' || character == '\v';
}

[[nodiscard]] bool IsDateSuffix(std::string_view value) noexcept {
    if (value.size() < 11) {
        return false;
    }
    const std::size_t start = value.size() - 11;
    if (value[start] != '-' || value[start + 5] != '-' ||
        value[start + 8] != '-') {
        return false;
    }
    for (std::size_t index = 1; index < 11; ++index) {
        if (index == 5 || index == 8) {
            continue;
        }
        const char character = value[start + index];
        if (character < '0' || character > '9') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] const Pricing* FindPricing(std::string_view model) noexcept {
    const auto match = std::find_if(
        kPrices.begin(), kPrices.end(),
        [model](const Pricing& pricing) { return pricing.model == model; });
    return match == kPrices.end() ? nullptr : &*match;
}

[[nodiscard]] std::int64_t NonNegative(std::int64_t value) noexcept {
    return value < 0 ? 0 : value;
}

}  // namespace

std::string NormalizeCodexCostModel(std::string_view rawModel) {
    while (!rawModel.empty() && IsAsciiWhitespace(rawModel.front())) {
        rawModel.remove_prefix(1);
    }
    while (!rawModel.empty() && IsAsciiWhitespace(rawModel.back())) {
        rawModel.remove_suffix(1);
    }
    constexpr std::string_view kOpenAIPrefix = "openai/";
    if (rawModel.substr(0, kOpenAIPrefix.size()) == kOpenAIPrefix) {
        rawModel.remove_prefix(kOpenAIPrefix.size());
    }
    if (rawModel == "gpt-5.6") {
        return "gpt-5.6-sol";
    }
    if (IsDateSuffix(rawModel)) {
        rawModel.remove_suffix(11);
    }

    struct Alias {
        std::string_view from;
        std::string_view to;
    };
    static constexpr std::array<Alias, 5> kAliases{{
        {"gpt-5-codex", "gpt-5"},
        {"gpt-5.1-codex", "gpt-5.1"},
        {"gpt-5.1-codex-max", "gpt-5.1"},
        {"gpt-5.1-codex-mini", "gpt-5-mini"},
        {"gpt-5.2-codex", "gpt-5.2"},
    }};
    for (const Alias& alias : kAliases) {
        if (rawModel == alias.from) {
            return std::string(alias.to);
        }
    }
    return std::string(rawModel);
}

CodexCostEstimate EstimateCodexApiEquivalentCost(
    std::string_view model,
    const CodexTokenUsage& usage) noexcept {
    CodexCostEstimate result;
    try {
        result.normalizedModel = NormalizeCodexCostModel(model);
        const Pricing* const pricing = FindPricing(result.normalizedModel);
        if (pricing == nullptr) {
            return result;
        }

        const std::int64_t input = NonNegative(usage.inputTokens);
        const std::int64_t cached =
            std::min(input, NonNegative(usage.cachedInputTokens));
        const std::int64_t remainingAfterCache = input - cached;
        const std::int64_t cacheWrite = std::min(
            remainingAfterCache, NonNegative(usage.cacheWriteInputTokens));
        const std::int64_t uncached = remainingAfterCache - cacheWrite;
        const std::int64_t output = NonNegative(usage.outputTokens);

        result.longContext = pricing->longContextThreshold > 0 &&
                             input > pricing->longContextThreshold;
        const double inputRate =
            result.longContext ? pricing->longInput : pricing->input;
        const double cachedRate = result.longContext
                                      ? pricing->longCachedInput
                                      : pricing->cachedInput;
        const double configuredWriteRate = result.longContext
                                               ? pricing->longCacheWrite
                                               : pricing->cacheWrite;
        const double writeRate = configuredWriteRate == kUseInputRate
                                     ? inputRate
                                     : configuredWriteRate;
        const double outputRate =
            result.longContext ? pricing->longOutput : pricing->output;

        const long double tokenDollarUnits =
            static_cast<long double>(uncached) * inputRate +
            static_cast<long double>(cached) * cachedRate +
            static_cast<long double>(cacheWrite) * writeRate +
            static_cast<long double>(output) * outputRate;
        const long double dollars = tokenDollarUnits / 1000000.0L;
        if (!std::isfinite(dollars) ||
            dollars > static_cast<long double>(
                          std::numeric_limits<double>::max())) {
            result.longContext = false;
            return result;
        }

        result.estimatedUsd = static_cast<double>(dollars);
        if (!std::isfinite(result.estimatedUsd) || result.estimatedUsd < 0.0) {
            result.estimatedUsd = 0.0;
            result.longContext = false;
            return result;
        }
        result.available = true;
        result.pricingVersion = std::string(kCodexCostPricingVersion);
        return result;
    } catch (...) {
        // The public API is noexcept so malformed or enormous caller-owned
        // strings cannot terminate the monitoring process on allocation error.
        return CodexCostEstimate{};
    }
}

}  // namespace codex_monitor::codex
