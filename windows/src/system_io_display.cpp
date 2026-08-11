#include "system_io_display.h"

#include <cmath>
#include <iomanip>
#include <iterator>
#include <sstream>

namespace codex_monitor {
namespace {

bool IsUsableRate(const std::optional<double>& rate) {
    return rate && std::isfinite(*rate) && *rate >= 0.0;
}

bool HasAnyUsableRate(const SystemIoRates& rates) {
    return IsUsableRate(rates.networkReceiveBytesPerSecond) ||
           IsUsableRate(rates.networkSendBytesPerSecond) ||
           IsUsableRate(rates.diskReadBytesPerSecond) ||
           IsUsableRate(rates.diskWriteBytesPerSecond);
}

}  // namespace

std::wstring FormatSystemIoByteRate(
    const std::optional<double>& bytesPerSecond,
    bool needsBaseline) {
    if (!bytesPerSecond) {
        return needsBaseline ? L"正在建立基线" : L"不可用";
    }
    if (!std::isfinite(*bytesPerSecond) || *bytesPerSecond < 0.0) {
        return L"不可用";
    }

    constexpr double kUnitStep = 1024.0;
    constexpr const wchar_t* kUnits[] = {
        L"B/s", L"KB/s", L"MB/s", L"GB/s", L"TB/s"};
    double value = *bytesPerSecond;
    std::size_t unit = 0;
    while (value >= kUnitStep && unit + 1 < std::size(kUnits)) {
        value /= kUnitStep;
        ++unit;
    }

    std::wostringstream output;
    output << std::fixed
           << std::setprecision(unit == 0 || value >= 100.0 ? 0 : 1)
           << value << L' ' << kUnits[unit];
    return output.str();
}

std::wstring BuildSystemIoThroughputCardText(const SystemIoRates& rates) {
    std::wostringstream output;
    output << L"网络与磁盘实时速度\r\n"
           << L"下载："
           << FormatSystemIoByteRate(
                  rates.networkReceiveBytesPerSecond,
                  rates.networkNeedsBaseline)
           << L"  |  上传："
           << FormatSystemIoByteRate(
                  rates.networkSendBytesPerSecond,
                  rates.networkNeedsBaseline)
           << L"\r\n磁盘读："
           << FormatSystemIoByteRate(
                  rates.diskReadBytesPerSecond,
                  rates.diskNeedsBaseline)
           << L"  |  磁盘写："
           << FormatSystemIoByteRate(
                  rates.diskWriteBytesPerSecond,
                  rates.diskNeedsBaseline);

    if (!HasAnyUsableRate(rates) && !rates.networkNeedsBaseline &&
        !rates.diskNeedsBaseline) {
        output << L"\r\n当前未取得网络或磁盘计数";
    } else {
        output << L"\r\n复用现有 5 秒性能采样";
    }
    return output.str();
}

}  // namespace codex_monitor
