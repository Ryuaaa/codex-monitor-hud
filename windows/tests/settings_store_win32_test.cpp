#include "settings_store_win32.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

bool CpuTrendEnabled(const codex_monitor::SettingsState& settings) {
    const std::size_t index =
        codex_monitor::ModuleIndex(codex_monitor::ModuleId::kCpuTrend);
    return settings.homeVisible[index] && settings.nativePageVisible[index];
}

void WriteText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

}  // namespace

int main() {
    std::error_code error;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        (L"CodexMonitorHUD-settings-store-test-" +
         std::to_wstring(GetCurrentProcessId()));
    std::filesystem::remove_all(root, error);
    error.clear();
    std::filesystem::create_directories(root, error);
    Expect(!error, "the isolated settings test directory must be created");

    const std::filesystem::path settingsPath = root / L"settings.ini";
    Expect(CpuTrendEnabled(codex_monitor::LoadSettingsFile(settingsPath)),
           "a genuinely missing settings file must use new-install defaults");

    Expect(!CpuTrendEnabled(codex_monitor::LoadSettingsFile({})),
           "an unresolved settings location must not opt into a new module");

    WriteText(settingsPath, "version=broken\n");
    Expect(!CpuTrendEnabled(codex_monitor::LoadSettingsFile(settingsPath)),
           "a malformed existing settings file must keep the new trend off");

    const std::filesystem::path oversizedPath = root / L"oversized.ini";
    {
        std::ofstream output(oversizedPath, std::ios::binary | std::ios::trunc);
        output.seekp(64 * 1024);
        output.put('x');
    }
    Expect(!CpuTrendEnabled(codex_monitor::LoadSettingsFile(oversizedPath)),
           "an oversized settings file must keep the new trend off");

    const std::filesystem::path directoryPath = root / L"settings-directory";
    error.clear();
    std::filesystem::create_directory(directoryPath, error);
    Expect(!error, "the invalid settings directory fixture must be created");
    Expect(!CpuTrendEnabled(codex_monitor::LoadSettingsFile(directoryPath)),
           "a non-file settings path must keep the new trend off");

    const codex_monitor::SettingsState explicitSettings =
        codex_monitor::DefaultSettings();
    Expect(codex_monitor::SaveSettingsFile(settingsPath, explicitSettings),
           "valid version 11 settings must save atomically");
    Expect(CpuTrendEnabled(codex_monitor::LoadSettingsFile(settingsPath)),
           "an explicitly saved version 11 trend selection must survive reload");

    error.clear();
    std::filesystem::remove_all(root, error);
    Expect(!error, "the isolated settings test directory must be removed");

    if (failures != 0) return 1;
    std::cout << "settings_store_win32_tests=pass\n";
    return 0;
}
