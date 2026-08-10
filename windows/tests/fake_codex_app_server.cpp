#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

bool WriteAll(HANDLE handle, std::string_view bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD request = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size() - offset, 32 * 1024));
        DWORD written = 0;
        if (!WriteFile(handle, bytes.data() + offset, request, &written, nullptr) ||
            written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

bool ReadInputLine(std::string& line) {
    line.clear();
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    char character = 0;
    while (line.size() <= 1024 * 1024) {
        DWORD received = 0;
        if (!ReadFile(input, &character, 1, &received, nullptr) || received == 0) return false;
        if (character == '\n') {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }
        line.push_back(character);
    }
    return false;
}

bool ReadInputLine() {
    std::string discarded;
    return ReadInputLine(discarded);
}

std::wstring CurrentExecutablePath() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                            static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    path.resize(length);
    return path;
}

std::wstring Quote(std::wstring_view value) {
    std::wstring result = L"\"";
    result.append(value);
    result += L"\"";
    return result;
}

DWORD SpawnHangingChild() {
    const std::wstring executable = CurrentExecutablePath();
    if (executable.empty()) return 0;
    std::wstring commandLine = Quote(executable) + L" --scenario=child-hang";
    std::vector<wchar_t> mutableLine(commandLine.begin(), commandLine.end());
    mutableLine.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), mutableLine.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        return 0;
    }
    const DWORD processId = process.dwProcessId;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return processId;
}

std::wstring ScenarioFromArguments(int argc, wchar_t** argv) {
    constexpr std::wstring_view prefix = L"--scenario=";
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument.substr(0, prefix.size()) == prefix) {
            return std::wstring(argument.substr(prefix.size()));
        }
    }
    wchar_t configured[128]{};
    const DWORD length = GetEnvironmentVariableW(L"CODEX_FAKE_SCENARIO", configured,
                                                  static_cast<DWORD>(std::size(configured)));
    if (length > 0 && length < std::size(configured)) {
        return std::wstring(configured, length);
    }
    return L"fragmented";
}

bool Contains(std::string_view value, std::string_view token) {
    return value.find(token) != std::string_view::npos;
}

bool ReadAndValidateAppServerRequests() {
    std::vector<std::string> requests;
    for (int index = 0; index < 5; ++index) {
        std::string line;
        if (!ReadInputLine(line)) return false;
        requests.push_back(std::move(line));
    }
    bool initialized = false;
    bool rateLimits = false;
    bool account = false;
    bool usage = false;
    bool threads = false;
    for (const std::string& request : requests) {
        initialized = initialized ||
            (Contains(request, "\"method\":\"initialized\"") &&
             Contains(request, "\"params\":{}"));
        rateLimits = rateLimits ||
            (Contains(request, "\"id\":2") &&
             Contains(request, "\"method\":\"account/rateLimits/read\"") &&
             Contains(request, "\"params\":null"));
        account = account ||
            (Contains(request, "\"id\":3") &&
             Contains(request, "\"method\":\"account/read\"") &&
             Contains(request, "\"refreshToken\":false"));
        usage = usage ||
            (Contains(request, "\"id\":4") &&
             Contains(request, "\"method\":\"account/usage/read\"") &&
             Contains(request, "\"params\":null"));
        threads = threads ||
            (Contains(request, "\"id\":5") &&
             Contains(request, "\"method\":\"thread/list\"") &&
             Contains(request, "\"limit\":5") &&
             Contains(request, "\"sortKey\":\"recency_at\"") &&
             Contains(request, "\"sortDirection\":\"desc\"") &&
             Contains(request, "\"useStateDbOnly\":true"));
    }
    return initialized && rateLimits && account && usage && threads;
}

constexpr std::string_view kRateLimitsResponse =
    R"json({"id":2,"result":{"rateLimits":{"planType":"pro","primary":{"usedPercent":25,"windowDurationMins":300,"resetsAt":1786320000},"secondary":{"usedPercent":40,"windowDurationMins":10080,"resetsAt":1786924800}}}})json";
constexpr std::string_view kAccountResponse =
    R"json({"id":3,"result":{"account":{"planType":"pro","email":"must-not-survive@example.com"}}})json";
constexpr std::string_view kUsageResponse =
    R"json({"id":4,"result":{"dailyUsageBuckets":[{"startDate":"2026-08-10","tokens":321}],"summary":{"lifetimeTokens":90000,"currentStreakDays":3}}})json";
constexpr std::string_view kThreadsResponse =
    R"json({"id":5,"result":{"data":[{"id":"secret-id","name":"Process-scoped task","recencyAt":1786320123,"status":{"type":"active"},"preview":"SECRET PREVIEW","cwd":"C:\\SecretProject","path":"C:\\SecretProject\\rollout.jsonl"}]}})json";

bool WriteLine(HANDLE output, std::string_view line) {
    return WriteAll(output, std::string(line) + "\n");
}

int RunAppServerScenario(const std::wstring& scenario,
                         int argc,
                         wchar_t** argv,
                         HANDLE output) {
    if (argc != 3 || std::wstring_view(argv[1]) != L"app-server" ||
        std::wstring_view(argv[2]) != L"--stdio") {
        return 20;
    }
    std::string initialize;
    if (!ReadInputLine(initialize) ||
        !Contains(initialize, "\"id\":1") ||
        !Contains(initialize, "\"method\":\"initialize\"") ||
        !Contains(initialize, "\"clientInfo\"") ||
        !Contains(initialize, "\"name\":\"codex-monitor-hud\"") ||
        !Contains(initialize, "\"title\":\"Codex Monitor HUD\"") ||
        !Contains(initialize, "\"version\":\"test-version\"")) {
        return 21;
    }
    if (scenario == L"app-init-error") {
        return WriteLine(output,
                         R"json({"id":1,"error":{"code":-32000,"message":"PRIVATE INITIALIZE ERROR"}})json")
                   ? 0
                   : 22;
    }
    if (scenario == L"app-line-flood") {
        for (int index = 0; index < 513; ++index) {
            if (!WriteLine(output, "not-json")) return 30;
            // Let the client drain its deliberately smaller transport queue so
            // this scenario reaches the protocol layer's independent line cap.
            if (index % 16 == 15) Sleep(2);
        }
        Sleep(INFINITE);
        return 0;
    }
    if (!WriteLine(output, R"json({"id":1,"result":{"server":"fake"}})json")) {
        return 23;
    }
    if (!ReadAndValidateAppServerRequests()) return 24;

    if (scenario == L"app-cancel") {
        if (SpawnHangingChild() == 0) return 29;
        Sleep(INFINITE);
        return 0;
    }

    if (scenario == L"app-out-of-order") {
        if (!WriteLine(output,
                       R"json({"method":"turn/started","params":{"preview":"PRIVATE NOTIFICATION"}})json") ||
            !WriteLine(output, kThreadsResponse) ||
            !WriteLine(output, R"json({"id":99,"result":{"preview":"PRIVATE UNKNOWN"}})json") ||
            !WriteLine(output, kAccountResponse) ||
            !WriteLine(output, kRateLimitsResponse) ||
            !WriteLine(output, kUsageResponse)) {
            return 25;
        }
        return 0;
    }
    if (scenario == L"app-single-error") {
        if (!WriteLine(output, kRateLimitsResponse) ||
            !WriteLine(output,
                       R"json({"id":3,"error":{"code":-32001,"message":"PRIVATE ACCOUNT ERROR"}})json") ||
            !WriteLine(output, kUsageResponse) ||
            !WriteLine(output, kThreadsResponse)) {
            return 26;
        }
        return 0;
    }
    if (scenario == L"app-malformed") {
        if (!WriteLine(output, "not-json") ||
            !WriteLine(output, R"json({"method":123})json") ||
            !WriteLine(output, R"json({"id":"unknown-id","result":{}})json") ||
            !WriteLine(output, R"json({"id":777,"result":{"preview":"PRIVATE UNKNOWN"}})json") ||
            !WriteLine(output,
                       R"json({"id":4,"result":{},"error":{"code":-1}})json") ||
            !WriteLine(output, kUsageResponse) ||
            !WriteLine(output, kThreadsResponse) ||
            !WriteLine(output, kRateLimitsResponse) ||
            !WriteLine(output, kAccountResponse)) {
            return 27;
        }
        return 0;
    }

    if (!WriteLine(output, kRateLimitsResponse) ||
        !WriteLine(output, kAccountResponse) ||
        !WriteLine(output, kUsageResponse) ||
        !WriteLine(output, kThreadsResponse)) {
        return 28;
    }
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const std::wstring scenario = ScenarioFromArguments(argc, argv);
    if (scenario == L"child-hang") {
        Sleep(INFINITE);
        return 0;
    }

    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
    if (scenario.rfind(L"app-", 0) == 0) {
        return RunAppServerScenario(scenario, argc, argv, output);
    }
    if (scenario == L"fragmented") {
        if (!ReadInputLine()) return 2;
        if (!WriteAll(output, "{\"fragmented\":")) return 3;
        Sleep(25);
        if (!WriteAll(output,
                      "true}\r\n{\"second\":2}\n{\"third\":3}\r\n")) return 4;
        return 0;
    }
    if (scenario == L"oversized") {
        std::string line(1024 * 1024 + 1, 'x');
        line.push_back('\n');
        return WriteAll(output, line) ? 0 : 5;
    }
    if (scenario == L"stderr") {
        const std::string noise(64 * 1024, 'e');
        if (!WriteAll(error, noise)) return 6;
        return WriteAll(output, "{\"stderr\":true}\n") ? 0 : 7;
    }
    if (scenario == L"hang") {
        const DWORD child = SpawnHangingChild();
        if (child == 0) return 8;
        const std::string line = "{\"childPid\":" + std::to_string(child) + "}\n";
        if (!WriteAll(output, line)) return 9;
        Sleep(INFINITE);
        return 0;
    }
    return 10;
}
