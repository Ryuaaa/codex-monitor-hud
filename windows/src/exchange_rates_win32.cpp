#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "exchange_rates_win32.h"
#include "display_preferences.h"
#include <windows.h>
#include <winhttp.h>
#include <winrt/base.h>
#ifdef GetObject
#undef GetObject
#endif
#include <winrt/Windows.Data.Json.h>
#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <mutex>

namespace codex_monitor {
namespace {
std::mutex rateMutex;
std::array<double,5> rates{6.7063,1,.86088,154.18,1343.8};
std::string date(kReferenceExchangeDate);
std::int64_t lastAttempt = 0;
bool loaded = false;
struct InternetHandle {
    HINTERNET value;
    ~InternetHandle(){ if(value) WinHttpCloseHandle(value); }
};
bool ValidRates(const std::array<double,5>& values) {
    for (double value : values) if (!std::isfinite(value) || value <= 0 || value > 1e6) return false;
    return values[1] == 1;
}
void Save(const std::filesystem::path& path) {
    auto temporary = path; temporary += ".tmp";
    std::ofstream out(temporary,std::ios::binary|std::ios::trunc);
    if (!out) return;
    out.imbue(std::locale::classic());
    out << "version=1\n" << lastAttempt << '\n' << date << '\n' << std::setprecision(17);
    for (double rate : rates) out << rate << '\n';
    out.close();
    if (out) MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
}
}
double DisplayExchangeRate(std::string_view currency) {
    std::lock_guard<std::mutex> guard(rateMutex);
    for (std::size_t i=0;i<5;++i) if(currency == kDisplayCurrencies[i]) return rates[i];
    return rates[0];
}
std::string DisplayExchangeRateDate() {
    std::lock_guard<std::mutex> guard(rateMutex); return date;
}
void RefreshDisplayExchangeRates(const std::filesystem::path& directory) noexcept {
    try {
        if(directory.empty()) return;
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const auto path = directory / "display-exchange-rates.txt";
        {
            std::lock_guard<std::mutex> guard(rateMutex);
            if(!loaded) {
                loaded = true;
                std::error_code error;
                const auto size = std::filesystem::file_size(path,error);
                if(!error && size <= 1024) {
                    std::ifstream in(path); in.imbue(std::locale::classic());
                    std::string version, candidateDate;
                    std::int64_t attempted=0;
                    std::array<double,5> candidate{};
                    std::getline(in,version);
                    in >> attempted >> candidateDate;
                    for(auto& rate : candidate) in >> rate;
                    if(in && version=="version=1" && attempted>=0 && attempted<=now &&
                       IsSubscriptionDate(candidateDate) && !candidateDate.empty() && ValidRates(candidate)) {
                        lastAttempt=attempted; date=candidateDate; rates=candidate;
                    }
                }
            }
            if(lastAttempt > 0 && now-lastAttempt < 86400) return;
            lastAttempt=now;
            std::error_code error;
            std::filesystem::create_directories(directory,error);
            if(error) return;
            Save(path); // Failures also get daily backoff across restarts.
        }
        InternetHandle session{WinHttpOpen(L"CodexMonitorHUD/1.3.0",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0)};
        if(!session.value) return;
        WinHttpSetTimeouts(session.value,4000,4000,4000,4000);
        InternetHandle connection{WinHttpConnect(session.value,L"api.frankfurter.dev",INTERNET_DEFAULT_HTTPS_PORT,0)};
        if(!connection.value) return;
        InternetHandle request{WinHttpOpenRequest(connection.value,L"GET",
            L"/v1/latest?base=USD&symbols=CNY,EUR,JPY,KRW",nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE)};
        if(!request.value) return;
        DWORD disable=WINHTTP_DISABLE_COOKIES|WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(request.value,WINHTTP_OPTION_DISABLE_FEATURE,&disable,sizeof(disable));
        DWORD autologon=WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
        WinHttpSetOption(request.value,WINHTTP_OPTION_AUTOLOGON_POLICY,&autologon,sizeof(autologon));
        if(!WinHttpSendRequest(request.value,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0) ||
           !WinHttpReceiveResponse(request.value,nullptr)) return;
        DWORD status=0, length=sizeof(status);
        if(!WinHttpQueryHeaders(request.value,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,&status,&length,WINHTTP_NO_HEADER_INDEX) || status!=200) return;
        std::string body;
        for(;;) {
            char chunk[4096]; DWORD read=0;
            if(!WinHttpReadData(request.value,chunk,sizeof(chunk),&read)) return;
            if(!read) break;
            if(body.size()+read>16384) return;
            body.append(chunk,read);
        }
        using namespace winrt::Windows::Data::Json;
        const auto json=JsonObject::Parse(winrt::to_hstring(body));
        if(json.GetNamedString(L"base")!=L"USD") return;
        const std::string candidateDate=winrt::to_string(json.GetNamedString(L"date"));
        if(candidateDate.empty() || !IsSubscriptionDate(candidateDate)) return;
        SYSTEMTIME stamp{};
        stamp.wYear=static_cast<WORD>(std::stoi(candidateDate.substr(0,4)));
        stamp.wMonth=static_cast<WORD>(std::stoi(candidateDate.substr(5,2)));
        stamp.wDay=static_cast<WORD>(std::stoi(candidateDate.substr(8,2)));
        FILETIME fileTime{};
        if(!SystemTimeToFileTime(&stamp,&fileTime)) return;
        ULARGE_INTEGER ticks{}; ticks.LowPart=fileTime.dwLowDateTime; ticks.HighPart=fileTime.dwHighDateTime;
        const auto timestamp=static_cast<std::int64_t>(ticks.QuadPart/10000000ULL)-11644473600LL;
        if(timestamp > now+86400 || now-timestamp > 14*86400) return;
        const auto object=json.GetNamedObject(L"rates");
        std::array<double,5> candidate{0,1,0,0,0};
        for(std::size_t i=0;i<5;++i) if(i!=1) {
            const auto key=winrt::to_hstring(std::string(kDisplayCurrencies[i]));
            const auto value=object.GetNamedValue(key);
            if(value.ValueType()!=JsonValueType::Number) return;
            candidate[i]=value.GetNumber();
        }
        if(!ValidRates(candidate)) return;
        std::lock_guard<std::mutex> guard(rateMutex);
        date=candidateDate; rates=candidate; Save(path);
    } catch(...) { /* Preserve last valid public snapshot, never clear ledger. */ }
}
}
