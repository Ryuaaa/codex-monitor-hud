#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
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

bool ReadInputLine() {
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    char character = 0;
    std::size_t count = 0;
    while (count <= 1024 * 1024) {
        DWORD received = 0;
        if (!ReadFile(input, &character, 1, &received, nullptr) || received == 0) return false;
        if (character == '\n') return true;
        ++count;
    }
    return false;
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
    return L"fragmented";
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
