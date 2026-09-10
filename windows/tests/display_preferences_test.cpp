#include "display_preferences.h"
#include "localization.h"
#include "module_state.h"
#include <cstdlib>
#include <iostream>
#include <limits>

void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
int main(int argc, char** argv) {
    using namespace codex_monitor;
    hudLanguage = argc > 1 ? argv[1] : "zh-Hans";
    auto settings = DefaultSettings();
    Check(settings.displayCurrency == "CNY" && settings.displayLanguage == "zh-Hans", "safe defaults");
    for (auto language : kDisplayLanguages) for (auto currency : kDisplayCurrencies) {
        settings.displayLanguage = language;
        settings.displayCurrency = currency;
        settings.subscriptionDate = "2028-02-29";
        settings.weeklyQuotaAlert.enabled = true;
        auto restored = ParseSettings(SerializeSettings(settings));
        Check(restored.displayLanguage == language && restored.displayCurrency == currency &&
              restored.subscriptionDate == settings.subscriptionDate && restored.weeklyQuotaAlert.enabled,
              "display preferences preserve existing alert policy");
        Check(FormatDisplayMoney(1, currency).find(std::wstring(currency.begin(),currency.end())) == 0,
              "currency code shown");
    }
    Check(!IsSubscriptionDate("2027-02-29") && !IsSubscriptionDate("2026-13-01") &&
          !IsSubscriptionDate("2026-01-32") && IsSubscriptionDate(""), "date bounds and clearing");
    Check(FormatDisplayMoney(std::numeric_limits<double>::infinity(),"CNY") == L"--" &&
          FormatDisplayMoney(-1,"USD") == L"--", "invalid amounts not displayed");
    Check(FormatDisplayMoney(1,"JPY") == L"JPY 154" && FormatDisplayMoney(1,"KRW") == L"KRW 1344",
          "JPY and KRW round to integer");
    const std::wstring title = Localized(L"Settings");
    Check(title == (hudLanguage == "en" ? L"Settings" : hudLanguage == "ja" ? L"設定" :
                    hudLanguage == "ko" ? L"설정" : hudLanguage == "zh-Hant" ? L"設置" : L"设置"),
          "native language selected");
    std::cout << "display preferences pass\n";
}
