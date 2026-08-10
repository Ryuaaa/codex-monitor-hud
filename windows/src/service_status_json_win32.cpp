#include "service_status_json_win32.h"

#include <winrt/base.h>

// wingdi.h defines GetObject as GetObjectW under UNICODE. Undefine it before
// the C++/WinRT projection declares IJsonValue::GetObject.
#ifdef GetObject
#undef GetObject
#endif

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

namespace codex_monitor {
namespace {

using winrt::Windows::Data::Json::IJsonValue;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValueType;

std::optional<IJsonValue> FindValue(const JsonObject& object,
                                    const wchar_t* key) {
    const winrt::hstring name(key);
    if (!object.HasKey(name)) return std::nullopt;
    return object.Lookup(name);
}

bool IsAsciiWhitespace(wchar_t character) noexcept {
    return character == L' ' || character == L'\t' || character == L'\r' ||
           character == L'\n' || character == L'\f' || character == L'\v';
}

wchar_t AsciiLower(wchar_t character) noexcept {
    if (character >= L'A' && character <= L'Z') {
        return character + (L'a' - L'A');
    }
    return character;
}

std::wstring_view TrimAsciiWhitespace(std::wstring_view value) noexcept {
    while (!value.empty() && IsAsciiWhitespace(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && IsAsciiWhitespace(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

bool EqualsAsciiCaseInsensitive(std::wstring_view left,
                                std::wstring_view right) noexcept {
    left = TrimAsciiWhitespace(left);
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (AsciiLower(left[index]) != AsciiLower(right[index])) return false;
    }
    return true;
}

std::optional<std::string> ReadRequiredString(const JsonObject& object,
                                              const wchar_t* key) {
    const std::optional<IJsonValue> value = FindValue(object, key);
    if (!value || value->ValueType() != JsonValueType::String) {
        return std::nullopt;
    }
    return winrt::to_string(value->GetString());
}

}  // namespace

std::optional<ParsedOpenAIServiceStatus> ParseOpenAIServiceStatusSummaryJson(
    std::wstring_view json) noexcept {
    try {
        JsonObject root;
        if (!JsonObject::TryParse(winrt::hstring(json), root)) {
            return std::nullopt;
        }

        const std::optional<IJsonValue> statusValue = FindValue(root, L"status");
        if (!statusValue || statusValue->ValueType() != JsonValueType::Object) {
            return std::nullopt;
        }
        const std::optional<std::string> indicator =
            ReadRequiredString(statusValue->GetObject(), L"indicator");
        if (!indicator || indicator->empty()) return std::nullopt;

        ParsedOpenAIServiceStatus parsed;
        parsed.overallIndicator = *indicator;

        const std::optional<IJsonValue> componentsValue =
            FindValue(root, L"components");
        if (!componentsValue || componentsValue->ValueType() == JsonValueType::Null) {
            return parsed;
        }
        if (componentsValue->ValueType() != JsonValueType::Array) {
            return std::nullopt;
        }

        constexpr std::wstring_view kCodexComponentName =
            L"Codex in ChatGPT Desktop";
        for (const IJsonValue& item : componentsValue->GetArray()) {
            if (item.ValueType() != JsonValueType::Object) {
                return std::nullopt;
            }
            const JsonObject component = item.GetObject();
            const std::optional<IJsonValue> nameValue =
                FindValue(component, L"name");
            if (!nameValue) continue;
            if (nameValue->ValueType() != JsonValueType::String) {
                return std::nullopt;
            }
            const winrt::hstring name = nameValue->GetString();
            if (!EqualsAsciiCaseInsensitive(
                    std::wstring_view(name.c_str(), name.size()),
                    kCodexComponentName)) {
                continue;
            }

            const std::optional<std::string> componentStatus =
                ReadRequiredString(component, L"status");
            if (!componentStatus) return std::nullopt;
            parsed.codexComponentStatus = *componentStatus;
            break;
        }
        return parsed;
    } catch (...) {
        // Keep malformed payloads and projection failures at this parse boundary.
        return std::nullopt;
    }
}

}  // namespace codex_monitor
