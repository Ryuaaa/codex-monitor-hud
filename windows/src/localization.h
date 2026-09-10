#pragma once
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include "localization_catalog.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace codex_monitor {
// Set exactly once before worker startup. No translation network requests.
inline std::string hudLanguage = "zh-Hans";
inline const wchar_t* Localized(const wchar_t* source) {
    static std::mutex mutex;
    static std::map<std::wstring, std::wstring> cache;
    std::lock_guard<std::mutex> guard(mutex);
    const auto found = cache.find(source);
    if (found != cache.end()) return found->second.c_str();
    std::wstring result(source);
    // Translate developer-owned literals only, never task titles or other data.
    for (const auto& row : kHudTranslations) {
        if (result == row.source) {
            result = hudLanguage == "en" ? row.en : hudLanguage == "ja" ? row.ja :
                     hudLanguage == "ko" ? row.ko : row.zh;
            break;
        }
    }
#ifdef _WIN32
    if (hudLanguage == "zh-Hant" && !result.empty()) {
        const int count = LCMapStringEx(L"zh-TW", LCMAP_TRADITIONAL_CHINESE,
            result.data(), static_cast<int>(result.size()), nullptr, 0, nullptr, nullptr, 0);
        if (count > 0) {
            std::wstring converted(static_cast<std::size_t>(count), L'\0');
            if (LCMapStringEx(L"zh-TW",LCMAP_TRADITIONAL_CHINESE,result.data(),
                static_cast<int>(result.size()),converted.data(),count,nullptr,nullptr,0)) result = std::move(converted);
        }
    }
#endif
    return cache.emplace(source,std::move(result)).first->second.c_str();
}
} // namespace codex_monitor
