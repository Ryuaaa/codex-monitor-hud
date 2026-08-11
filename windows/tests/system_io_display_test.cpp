#include "system_io_display.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {

void Expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void ExpectEqual(const std::wstring& actual,
                 const std::wstring& expected,
                 const char* message) {
    Expect(actual == expected, message);
}

void TestFixedRateFormattingSamples() {
    ExpectEqual(codex_monitor::FormatSystemIoByteRate(std::nullopt, true),
                L"正在建立基线",
                "a first or reset sample must say that it is establishing a baseline");
    ExpectEqual(codex_monitor::FormatSystemIoByteRate(std::nullopt, false),
                L"不可用",
                "a missing native source must stay unavailable");
    ExpectEqual(codex_monitor::FormatSystemIoByteRate(0.0, false),
                L"0 B/s",
                "a valid idle rate must be shown as zero rather than unavailable");
    ExpectEqual(codex_monitor::FormatSystemIoByteRate(512.0, false),
                L"512 B/s",
                "sub-kilobyte rates must retain byte units");
    ExpectEqual(codex_monitor::FormatSystemIoByteRate(1536.0, false),
                L"1.5 KB/s",
                "kilobyte rates must use a compact binary scale");
    ExpectEqual(codex_monitor::FormatSystemIoByteRate(2.0 * 1024.0 * 1024.0,
                                                       false),
                L"2.0 MB/s",
                "megabyte rates must remain readable at HUD size");
    ExpectEqual(codex_monitor::FormatSystemIoByteRate(
                    std::numeric_limits<double>::infinity(), false),
                L"不可用",
                "non-finite rates must not reach the UI");
    ExpectEqual(codex_monitor::FormatSystemIoByteRate(-1.0, false),
                L"不可用",
                "negative rates must not be presented as real throughput");
}

void TestCompleteCardUsesAllFourRates() {
    codex_monitor::SystemIoRates rates{};
    rates.networkReceiveBytesPerSecond = 1536.0;
    rates.networkSendBytesPerSecond = 512.0;
    rates.diskReadBytesPerSecond = 2.0 * 1024.0 * 1024.0;
    rates.diskWriteBytesPerSecond = 0.0;
    const std::wstring card =
        codex_monitor::BuildSystemIoThroughputCardText(rates);
    Expect(card.find(L"下载：1.5 KB/s") != std::wstring::npos,
           "the card must display network receive as download");
    Expect(card.find(L"上传：512 B/s") != std::wstring::npos,
           "the card must display network send as upload");
    Expect(card.find(L"磁盘读：2.0 MB/s") != std::wstring::npos,
           "the card must display disk reads");
    Expect(card.find(L"磁盘写：0 B/s") != std::wstring::npos,
           "the card must preserve a valid idle disk-write rate");
    Expect(card.find(L"复用现有 5 秒性能采样") != std::wstring::npos,
           "the card must state the existing low-burden cadence");
}

void TestBaselineAndUnavailableCardsAreHonest() {
    codex_monitor::SystemIoRates baseline{};
    baseline.networkNeedsBaseline = true;
    baseline.diskNeedsBaseline = true;
    const std::wstring baselineCard =
        codex_monitor::BuildSystemIoThroughputCardText(baseline);
    Expect(baselineCard.find(L"下载：正在建立基线") != std::wstring::npos &&
               baselineCard.find(L"磁盘读：正在建立基线") != std::wstring::npos,
           "first and reset samples must expose both baseline transitions");

    const std::wstring unavailable =
        codex_monitor::BuildSystemIoThroughputCardText({});
    Expect(unavailable.find(L"下载：不可用") != std::wstring::npos &&
               unavailable.find(L"磁盘写：不可用") != std::wstring::npos &&
               unavailable.find(L"当前未取得网络或磁盘计数") !=
                   std::wstring::npos,
           "an entirely unavailable sample must not look idle or healthy");

    codex_monitor::SystemIoRates invalid{};
    invalid.networkReceiveBytesPerSecond =
        std::numeric_limits<double>::quiet_NaN();
    const std::wstring invalidCard =
        codex_monitor::BuildSystemIoThroughputCardText(invalid);
    Expect(invalidCard.find(L"当前未取得网络或磁盘计数") !=
               std::wstring::npos,
           "invalid numeric samples must remain fully unavailable");
}

}  // namespace

int main() {
    TestFixedRateFormattingSamples();
    TestCompleteCardUsesAllFourRates();
    TestBaselineAndUnavailableCardsAreHonest();
    std::cout << "system_io_display_tests=pass\n";
    return 0;
}
