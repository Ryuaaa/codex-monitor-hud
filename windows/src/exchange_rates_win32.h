#pragma once
#include <filesystem>
#include <string>
#include <string_view>
namespace codex_monitor {
// Called on the existing serial Codex worker, never the UI thread.
void RefreshDisplayExchangeRates(const std::filesystem::path& directory) noexcept;
double DisplayExchangeRate(std::string_view currency);
std::string DisplayExchangeRateDate();
}
