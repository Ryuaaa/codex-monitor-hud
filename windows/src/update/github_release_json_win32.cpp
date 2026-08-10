#include "update/github_release_json_win32.h"

#include <winrt/base.h>

#ifdef GetObject
#undef GetObject
#endif

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <optional>
#include <string>
#include <utility>

namespace codex_monitor::update {
namespace {

using winrt::Windows::Data::Json::IJsonValue;
using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValueType;

constexpr std::size_t kMaximumReleaseCount = 20;
constexpr std::size_t kMaximumAssetsPerRelease = 128;
constexpr std::size_t kMaximumTagBytes = 128;
constexpr std::size_t kMaximumAssetNameBytes = 256;
constexpr std::size_t kMaximumUrlBytes = 2048;

std::optional<IJsonValue> FindValue(const JsonObject& object,
                                    const wchar_t* key) {
    const winrt::hstring name(key);
    if (!object.HasKey(name)) return std::nullopt;
    return object.Lookup(name);
}

bool IsSafeText(const std::string& value,
                std::size_t maximumBytes,
                bool allowEmpty) noexcept {
    if ((!allowEmpty && value.empty()) || value.size() > maximumBytes) {
        return false;
    }
    for (const unsigned char character : value) {
        if (character == 0 || character < 0x20 || character == 0x7f) {
            return false;
        }
    }
    return true;
}

bool ReadRequiredString(const JsonObject& object,
                        const wchar_t* key,
                        std::size_t maximumBytes,
                        std::string& output) {
    const std::optional<IJsonValue> value = FindValue(object, key);
    if (!value || value->ValueType() != JsonValueType::String) return false;
    output = winrt::to_string(value->GetString());
    return IsSafeText(output, maximumBytes, false);
}

bool ReadRequiredBoolean(const JsonObject& object,
                         const wchar_t* key,
                         bool& output) {
    const std::optional<IJsonValue> value = FindValue(object, key);
    if (!value || value->ValueType() != JsonValueType::Boolean) return false;
    output = value->GetBoolean();
    return true;
}

bool ParseAsset(const IJsonValue& value, GitHubReleaseAsset& output) {
    if (value.ValueType() != JsonValueType::Object) return false;
    const JsonObject object = value.GetObject();
    return ReadRequiredString(object, L"name", kMaximumAssetNameBytes,
                              output.name) &&
           ReadRequiredString(object, L"browser_download_url",
                              kMaximumUrlBytes, output.browserDownloadUrl);
}

bool ParseRelease(const IJsonValue& value,
                  GitHubReleaseCandidate& output) {
    if (value.ValueType() != JsonValueType::Object) return false;
    const JsonObject object = value.GetObject();
    if (!ReadRequiredString(object, L"tag_name", kMaximumTagBytes,
                            output.tagName) ||
        !ReadRequiredBoolean(object, L"draft", output.draft) ||
        !ReadRequiredBoolean(object, L"prerelease", output.prerelease)) {
        return false;
    }
    const std::optional<IJsonValue> assets = FindValue(object, L"assets");
    if (!assets || assets->ValueType() != JsonValueType::Array) return false;
    const JsonArray array = assets->GetArray();
    if (array.Size() > kMaximumAssetsPerRelease) return false;
    output.assets.reserve(array.Size());
    for (const IJsonValue& item : array) {
        GitHubReleaseAsset asset;
        if (!ParseAsset(item, asset)) return false;
        output.assets.push_back(std::move(asset));
    }
    return true;
}

}  // namespace

std::optional<std::vector<GitHubReleaseCandidate>>
ParseGitHubReleaseListJson(std::wstring_view json) noexcept {
    try {
        JsonArray root;
        if (!JsonArray::TryParse(winrt::hstring(json), root) ||
            root.Size() > kMaximumReleaseCount) {
            return std::nullopt;
        }
        std::vector<GitHubReleaseCandidate> releases;
        releases.reserve(root.Size());
        for (const IJsonValue& item : root) {
            GitHubReleaseCandidate release;
            if (!ParseRelease(item, release)) return std::nullopt;
            releases.push_back(std::move(release));
        }
        return releases;
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace codex_monitor::update
