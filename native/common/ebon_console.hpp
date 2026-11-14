#pragma once

#include "ebon_common.hpp"
#include <cstdarg>
#include <cstdio>

namespace ebon::console {

inline SRWLOCK g_lock = SRWLOCK_INIT;
inline HANDLE g_output = nullptr;
inline std::wstring g_logPath;
inline WORD g_defaultAttributes = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;

inline std::wstring widenUtf8(const std::string& text) {
    if (text.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
    if (size <= 0) return std::wstring(text.begin(), text.end());
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), result.data(), size);
    return result;
}

inline bool initialize(const wchar_t* title = L"EbonCore Dumper") {
    AcquireSRWLockExclusive(&g_lock);
    if (GetConsoleWindow() == nullptr && !AllocConsole()) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetConsoleTitleW(title);
    HANDLE output = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }

    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (GetConsoleScreenBufferInfo(output, &info)) g_defaultAttributes = info.wAttributes;
    g_output = output;
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

// Only the standalone viewer calls initialize(). Allocating a console in the
// game changes the window picked by some platform login SDKs.
inline bool startViewer(const std::filesystem::path& executable,DWORD targetPid,DWORD* viewerPid=nullptr) {
    std::wstring command=L"\""+executable.wstring()+L"\" --log \""+g_logPath+
                         L"\" --pid "+std::to_wstring(targetPid);
    STARTUPINFOW startup{};
    startup.cb=sizeof(startup);
    startup.dwFlags=STARTF_USESHOWWINDOW;
    startup.wShowWindow=SW_SHOWNOACTIVATE;
    PROCESS_INFORMATION process{};
    if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,
                       CREATE_NEW_CONSOLE|CREATE_UNICODE_ENVIRONMENT,nullptr,
                       executable.parent_path().c_str(),&startup,&process))return false;
    if(viewerPid)*viewerPid=process.dwProcessId;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

inline WORD colorFor(const char* level);
inline void emitRaw(const std::string& line,const char* level="info") {
    const auto text=widenUtf8(line);
    AcquireSRWLockExclusive(&g_lock);
    if(g_output){
        SetConsoleTextAttribute(g_output,colorFor(level));
        DWORD written=0;
        WriteConsoleW(g_output,text.data(),(DWORD)text.size(),&written,nullptr);
        SetConsoleTextAttribute(g_output,g_defaultAttributes);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

inline void setLogFile(const std::filesystem::path& path) {
    AcquireSRWLockExclusive(&g_lock);
    g_logPath = path.wstring();
    ReleaseSRWLockExclusive(&g_lock);
}

inline WORD colorFor(const char* level) {
    if (_stricmp(level, "success") == 0 || _stricmp(level, "ok") == 0)
        return FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    if (_stricmp(level, "warn") == 0 || _stricmp(level, "warning") == 0)
        return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    if (_stricmp(level, "error") == 0)
        return FOREGROUND_RED | FOREGROUND_INTENSITY;
    if (_stricmp(level, "debug") == 0)
        return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    return FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
}

inline void log(const char* level, const std::string& message) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char prefix[96]{};
    _snprintf_s(prefix, sizeof(prefix), _TRUNCATE,
                "[%02u:%02u:%02u] [ebon] %-7s ",
                now.wHour, now.wMinute, now.wSecond, level);
    std::string line(prefix);
    line += message;
    line += "\r\n";
    OutputDebugStringA(line.c_str());

    std::wstring wideLine = widenUtf8(line);
    AcquireSRWLockExclusive(&g_lock);
    if (g_output) {
        SetConsoleTextAttribute(g_output, colorFor(level));
        DWORD written = 0;
        WriteConsoleW(g_output, wideLine.data(), (DWORD)wideLine.size(), &written, nullptr);
        SetConsoleTextAttribute(g_output, g_defaultAttributes);
    }
    if (!g_logPath.empty()) {
        HANDLE file = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, line.data(), (DWORD)line.size(), &written, nullptr);
            CloseHandle(file);
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

inline void logf(const char* level, const char* format, ...) {
    char message[2048]{};
    va_list args;
    va_start(args, format);
    _vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
    va_end(args);
    log(level, message);
}

} // namespace ebon::console
