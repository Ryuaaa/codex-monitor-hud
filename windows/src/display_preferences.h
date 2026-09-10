#pragma once

#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

namespace codex_monitor {
inline constexpr std::array<std::string_view, 5> kDisplayLanguages{
    "zh-Hans", "zh-Hant", "en", "ja", "ko"};
inline constexpr std::array<std::string_view, 5> kDisplayCurrencies{
    "CNY", "USD", "EUR", "JPY", "KRW"};
template <std::size_t N>
inline bool IsDisplayChoice(std::string_view value,
                            const std::array<std::string_view, N>& choices) {
    for (auto choice : choices) if (value == choice) return true;
    return false;
}
inline bool IsSubscriptionDate(std::string_view value) {
    if (value.empty()) return true;
    if (value.size() != 10 || value[4] != '-' || value[7] != '-') return false;
    for (std::size_t i = 0; i < value.size(); ++i)
        if (i != 4 && i != 7 && (value[i] < '0' || value[i] > '9')) return false;
    const int year = std::stoi(std::string(value.substr(0, 4)));
    const int month = std::stoi(std::string(value.substr(5, 2)));
    const int day = std::stoi(std::string(value.substr(8, 2)));
    if (year < 2000 || year > 2200 || month < 1 || month > 12) return false;
    constexpr int days[]{31,28,31,30,31,30,31,31,30,31,30,31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    return day >= 1 && day <= days[month - 1] + (month == 2 && leap ? 1 : 0);
}
// Public reference snapshot, USD base. Never changes the underlying token/USD ledger.
inline constexpr std::string_view kReferenceExchangeDate = "2026-09-10";
inline std::wstring FormatDisplayMoney(double usd, std::string_view currency,
                                       double overrideRate = 0) {
    if (!std::isfinite(usd) || usd < 0) return L"--";
    if (!IsDisplayChoice(currency, kDisplayCurrencies)) currency = "CNY";
    const double rate = overrideRate > 0 && std::isfinite(overrideRate) ? overrideRate :
        currency == "CNY" ? 6.7063 : currency == "EUR" ? 0.86088 :
        currency == "JPY" ? 154.18 : currency == "KRW" ? 1343.8 : 1.0;
    const double amount = usd * rate;
    if (!std::isfinite(amount)) return L"--";
    std::wostringstream output;
    output.imbue(std::locale::classic());
    output << std::wstring(currency.begin(), currency.end()) << L' ' << std::fixed
           << std::setprecision(amount > 0 && amount < .01 ? 4 :
                                currency == "JPY" || currency == "KRW" ? 0 : 2)
           << amount;
    return output.str();
}
} // namespace codex_monitor
