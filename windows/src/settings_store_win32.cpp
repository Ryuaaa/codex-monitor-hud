#include "settings_store_win32.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <system_error>

namespace codex_monitor {
namespace {

constexpr std::uintmax_t kMaximumSettingsBytes = 64 * 1024;

}  // namespace

std::filesystem::path DefaultSettingsPath() {
    PWSTR localAppData = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT,
                                                nullptr, &localAppData);
    if (FAILED(result) || !localAppData) {
        if (localAppData) CoTaskMemFree(localAppData);
        return {};
    }
    const std::filesystem::path path =
        std::filesystem::path(localAppData) / L"CodexMonitorHUD" / L"settings.ini";
    CoTaskMemFree(localAppData);
    return path;
}

SettingsState LoadSettingsFile(const std::filesystem::path& path) {
    if (path.empty()) return DefaultSettings();
    std::error_code sizeError;
    const std::uintmax_t size = std::filesystem::file_size(path, sizeError);
    if (sizeError || size > kMaximumSettingsBytes) return DefaultSettings();
    std::ifstream input(path, std::ios::binary);
    if (!input) return DefaultSettings();
    const std::string contents((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    return ParseSettings(contents);
}

bool SaveSettingsFile(const std::filesystem::path& path, const SettingsState& settings) {
    if (path.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;

    std::filesystem::path temporary = path;
    temporary += L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output << SerializeSettings(settings);
        output.flush();
        if (!output) return false;
    }

    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

}  // namespace codex_monitor
