// EbonDebugInject.exe - early injector for Neverness To Everness (HTGame.exe).
//
// Injects EbonDumper.dll into the game process before AntiCheatExpert (ACE)
// initializes.
//
// Mechanism (adopted from HKW-Dumper HKWDebugInject):
//  1. Poll for HTGame.exe every 1 ms.
//  2. When the game process appears, inspect module count via EnumProcessModules.
//  3. If ACE (ACE-Base64.dll / tersafe.dll / AntiCheat) is already loaded, window is missed.
//  4. If module count < minModules (default 24), Windows loader has not finished
//     basic runtime initialization; wait with backoff to avoid loader lock.
//  5. When module count >= minModules and ACE is not yet present, inject
//     EbonDumper.dll via VirtualAllocEx + WriteProcessMemory + CreateRemoteThread(LoadLibraryW).
//  6. Injected DLL runs reflection capture internally using InProcessReader.
//
// Usage:
//   EbonDebugInject.exe [--dumper <dll>] [--game <exe>] [--profile <ini>]
//                       [--out <dir>] [--seconds N] [--min-modules N]
//                       [--wait-ms N] [--log <path>] [--no-elevate]

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>
#include <atomic>
#include <filesystem>
#include <chrono>
#include <fstream>

namespace {

std::wstring gDumperDll;
std::wstring gGameExe = L"HTGame.exe";
std::wstring gProfile;
std::wstring gOutDir;
std::wstring gLogPath;
int         gSeconds = 900;
int         gCaptureDelay = 0;
int         gMinModules = 24;
DWORD       gInjectAfterMs = 0;
DWORD       gWaitMs = 1500;
std::atomic<int> gInjected{ 0 };
bool       gAllowNonElevated = false;

// --- logging --------------------------------------------------------------- //
void logLine(const char* tag, const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    char line[2176];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[ebon] %s %s\n", tag, buf);
    OutputDebugStringA(line);
    fputs(line, stderr);
    fflush(stderr);

    if (!gLogPath.empty()) {
        HANDLE h = CreateFileW(gLogPath.c_str(), FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            WriteFile(h, line, (DWORD)strlen(line), &w, nullptr);
            CloseHandle(h);
        }
    }
}

// --- elevation ------------------------------------------------------------- //
bool isElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION el{};
    DWORD sz = sizeof(el);
    bool ok = false;
    if (GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &sz))
        ok = (el.TokenIsElevated != 0);
    CloseHandle(tok);
    return ok;
}

bool enableDebugPrivilege() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return false;
    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid)) { CloseHandle(tok); return false; }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    BOOL ok = AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    DWORD err = GetLastError();
    CloseHandle(tok);
    return ok && (err != ERROR_NOT_ALL_ASSIGNED);
}

bool relaunchSelfElevated() {
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring all = GetCommandLineW();
    std::wstring tail = all;
    if (!tail.empty() && tail[0] == L'"') {
        size_t e = tail.find(L'"', 1);
        if (e != std::wstring::npos) tail = tail.substr(e + 1);
    } else {
        size_t sp = tail.find(L' ');
        tail = (sp == std::wstring::npos) ? std::wstring() : tail.substr(sp);
    }
    size_t nonSpace = tail.find_first_not_of(L' ');
    if (nonSpace != std::wstring::npos) tail = tail.substr(nonSpace);
    else tail.clear();

    wchar_t cwd[MAX_PATH] = {};
    GetCurrentDirectoryW(MAX_PATH, cwd);
    SHELLEXECUTEINFOW si{};
    si.cbSize = sizeof(si);
    si.fMask = SEE_MASK_NOCLOSEPROCESS;
    si.lpVerb = L"runas";
    si.lpFile = self;
    si.lpParameters = tail.c_str();
    si.lpDirectory = cwd;
    si.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&si)) return false;
    if (si.hProcess) {
        WaitForSingleObject(si.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(si.hProcess, &code);
        CloseHandle(si.hProcess);
        ExitProcess(code);
    }
    return true;
}

// --- process helpers ------------------------------------------------------- //
DWORD findByName(const wchar_t* name) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD found = 0;
    if (Process32FirstW(s, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) { found = pe.th32ProcessID; break; }
        } while (Process32NextW(s, &pe));
    }
    CloseHandle(s);
    return found;
}

std::wstring imageOf(DWORD pid) {
    std::wstring out;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return out;
    wchar_t buf[MAX_PATH];
    DWORD sz = MAX_PATH;
    if (QueryFullProcessImageNameW(h, 0, buf, &sz)) out = buf;
    CloseHandle(h);
    return out;
}

bool makeAbsolutePath(std::wstring& path) {
    wchar_t full[32768] = {};
    DWORD n = GetFullPathNameW(path.c_str(), (DWORD)(sizeof(full) / sizeof(full[0])), full, nullptr);
    if (!n || n >= sizeof(full) / sizeof(full[0])) return false;
    path.assign(full, n);
    return true;
}

int moduleCount(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) return -1;
    HMODULE mods[1024];
    DWORD need = 0;
    int n = 0;
    if (EnumProcessModules(h, mods, sizeof(mods), &need))
        n = (int)(need / sizeof(HMODULE));
    CloseHandle(h);
    return n;
}

bool aceLoaded(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) return false;
    HMODULE mods[2048];
    DWORD need = 0;
    bool found = false;
    if (EnumProcessModules(h, mods, sizeof(mods), &need)) {
        DWORD count = need / sizeof(HMODULE);
        for (DWORD i = 0; i < count && !found; ++i) {
            wchar_t path[MAX_PATH];
            if (GetModuleFileNameExW(h, mods[i], path, MAX_PATH)) {
                if (_wcsicmp(path + (wcslen(path) >= 4 ? wcslen(path) - 4 : 0), L".dll") == 0 &&
                    (wcsstr(path, L"ACE") || wcsstr(path, L"AntiCheat") || wcsstr(path, L"tersafe")))
                    found = true;
            }
        }
    }
    CloseHandle(h);
    return found;
}

enum class InjectResult { Ok, TooEarly, Failed };

InjectResult injectInto(HANDLE h, const std::wstring& dll, DWORD waitMs) {
    SIZE_T n = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(h, nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        logLine("dbg", "VirtualAllocEx failed %lu", GetLastError());
        return InjectResult::Failed;
    }
    SIZE_T w = 0;
    if (!WriteProcessMemory(h, remote, dll.c_str(), n, &w)) {
        logLine("dbg", "WriteProcessMemory failed %lu", GetLastError());
        VirtualFreeEx(h, remote, 0, MEM_RELEASE);
        return InjectResult::Failed;
    }
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto pLLW = (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryW");
    HANDLE th = CreateRemoteThread(h, nullptr, 0, pLLW, remote, 0, nullptr);
    if (!th) {
        logLine("dbg", "CreateRemoteThread failed %lu", GetLastError());
        VirtualFreeEx(h, remote, 0, MEM_RELEASE);
        return InjectResult::Failed;
    }

    DWORD wr = WaitForSingleObject(th, waitMs);
    DWORD code = 0;
    GetExitCodeThread(th, &code);
    CloseHandle(th);
    VirtualFreeEx(h, remote, 0, MEM_RELEASE);

    if (wr == WAIT_TIMEOUT || code == STILL_ACTIVE) {
        logLine("dbg", "LoadLibraryW still running after %lums", (unsigned long)waitMs);
        return InjectResult::TooEarly;
    }
    if (code == 0) {
        logLine("dbg", "LoadLibraryW returned NULL (dll load error)");
        return InjectResult::Failed;
    }
    logLine("dbg", "LoadLibraryW exit=0x%08X (module base -> DllMain ran)", code);
    return InjectResult::Ok;
}

InjectResult attempt(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                           PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                           PROCESS_VM_READ, FALSE, pid);
    if (!h) {
        logLine("dbg", "OpenProcess(%lu) failed %lu", pid, GetLastError());
        return InjectResult::Failed;
    }

    int mods = moduleCount(pid);
    bool ace = aceLoaded(pid);
    logLine("dbg", "game pid=%lu modules=%d aceLoaded=%d", pid, mods, ace ? 1 : 0);

    InjectResult res = InjectResult::Failed;
    if (ace) {
        logLine("dbg", "SKIP: ACE already loaded, window missed");
        res = InjectResult::TooEarly;
    } else if (mods < gMinModules) {
        logLine("dbg", "SKIP: only %d modules (< %d)", mods, gMinModules);
        res = InjectResult::TooEarly;
    } else {
        res = injectInto(h, gDumperDll, gWaitMs);
    }

    CloseHandle(h);
    return res;
}

DWORD run() {
    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)gSeconds * 1000;
    int reported = 0;
    DWORD lastPid = 0;
    ULONGLONG firstSeen = 0;
    while (GetTickCount64() < deadline) {
        DWORD pid = findByName(gGameExe.c_str());
        if (!pid) {
            lastPid = 0;
            reported = 0;
            Sleep(1);
            continue;
        }

        if (pid != lastPid) {
            lastPid = pid;
            firstSeen = GetTickCount64();
            std::wstring img = imageOf(pid);
            logLine("dbg", "game appeared pid=%lu (%ls)", pid, img.c_str());
        }

        ULONGLONG age = GetTickCount64() - firstSeen;
        if (age < gInjectAfterMs) {
            ++reported;
            Sleep(50);
            continue;
        }
        if (reported != 0) {
            logLine("dbg", "injecting now (+%llums, modules=%d, ace=%d)",
                    (unsigned long long)age, moduleCount(pid),
                    aceLoaded(pid) ? 1 : 0);
        }
        reported = -1000;

        InjectResult r = attempt(pid);
        if (r == InjectResult::Ok) {
            ++gInjected;
            logLine("dbg", "DONE after %llums. Injected dumper runs inside the game.",
                    (unsigned long long)(GetTickCount64() - firstSeen));
            return pid;
        }

        if (reported > -1000 && reported < 30) {
            ++reported;
            logLine("dbg", "attempt %d: too early (modules=%d, +%llums since start)",
                    reported, moduleCount(pid),
                    (unsigned long long)(GetTickCount64() - firstSeen));
        }
        Sleep(r == InjectResult::TooEarly ? 50 : 300);
    }
    return 0;
}

void writeDumperIni(const std::filesystem::path& dllPath) {
    auto iniPath = dllPath.parent_path() / "EbonDumper.ini";
    std::wofstream ini(iniPath, std::ios::trunc);
    if (!ini) return;
    ini << L"# Generated by EbonDebugInject\n";
    ini << L"auto_dump=1\n";
    if (!gProfile.empty()) ini << L"profile=" << gProfile << L"\n";
    if (!gOutDir.empty())  ini << L"output=" << gOutDir << L"\n";
    if (!gLogPath.empty()) ini << L"log=" << gLogPath << L"\n";
    ini << L"max_wait_seconds=180\n";
    ini << L"capture_delay_seconds=" << gCaptureDelay << L"\n";
    ini << L"network_watch_seconds=900\n";
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);

    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::filesystem::path selfPath(self);
    std::filesystem::path rootDir = selfPath.parent_path().parent_path();

    gDumperDll = (selfPath.parent_path() / "EbonDumper.dll").wstring();
    gLogPath = (rootDir / "out" / "ebon_inject.log").wstring();
    gProfile = (rootDir / "profiles" / "nte-ue5.ini").wstring();

    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return (i + 1 < argc) ? argv[++i] : std::wstring(); };
        if (a == L"--dumper") gDumperDll = next();
        else if (a == L"--game") gGameExe = next();
        else if (a == L"--profile") gProfile = next();
        else if (a == L"--out") gOutDir = next();
        else if (a == L"--seconds") gSeconds = _wtoi(next().c_str());
        else if (a == L"--capture-delay") gCaptureDelay = _wtoi(next().c_str());
        else if (a == L"--min-modules") gMinModules = _wtoi(next().c_str());
        else if (a == L"--wait-ms") gWaitMs = (DWORD)_wtoi(next().c_str());
        else if (a == L"--inject-after-ms") gInjectAfterMs = (DWORD)_wtoi(next().c_str());
        else if (a == L"--log") gLogPath = next();
        else if (a == L"--no-elevate") gAllowNonElevated = true;
        else if (a == L"--help" || a == L"-h") {
            fwprintf(stderr, L"EbonDebugInject [--dumper <dll>] [--game <exe>] [--profile <ini>]\n"
                             L"                [--out <dir>] [--seconds N] [--min-modules N]\n"
                             L"                [--wait-ms N] [--capture-delay seconds] [--log <path>] [--no-elevate]\n");
            return 0;
        } else {
            fwprintf(stderr, L"Unknown argument: %ls\n", a.c_str());
            return 1;
        }
    }

    if(gCaptureDelay<0||gCaptureDelay>300){fwprintf(stderr,L"capture-delay must be in 0..300 seconds\n");return 2;}
    if (gOutDir.empty()) {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        struct tm tmBuf {};
        localtime_s(&tmBuf, &t);
        char timeStr[64] = {};
        std::strftime(timeStr, sizeof(timeStr), "%Y%m%d_%H%M%S", &tmBuf);
        gOutDir = (rootDir / "out" / "reflection" / ("injected_" + std::string(timeStr))).wstring();
    }

    if (!gLogPath.empty()) makeAbsolutePath(gLogPath);
    std::filesystem::path outDirPath(gOutDir);
    std::filesystem::create_directories(outDirPath.parent_path());
    std::filesystem::path logDirPath = std::filesystem::path(gLogPath).parent_path();
    std::filesystem::create_directories(logDirPath);

    DeleteFileW(gLogPath.c_str());

    if (!isElevated() && !gAllowNonElevated) {
        logLine("dbg", "not elevated: auto-elevating via UAC runas...");
        if (relaunchSelfElevated()) return 0;
        fwprintf(stderr, L"ERROR: UAC elevation required and was refused.\n");
        return 5;
    }

    bool privOk = enableDebugPrivilege();
    logLine("dbg", "privilege: SeDebugPrivilege %s", privOk ? "enabled" : "failed/unavailable");

    if (!makeAbsolutePath(gDumperDll)) {
        fwprintf(stderr, L"ERROR: cannot canonicalize dumper dll path: %ls\n", gDumperDll.c_str());
        return 2;
    }
    if (GetFileAttributesW(gDumperDll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        fwprintf(stderr, L"ERROR: dumper dll not found: %ls\n", gDumperDll.c_str());
        return 2;
    }

    if (!gProfile.empty()) makeAbsolutePath(gProfile);
    if (!gOutDir.empty()) makeAbsolutePath(gOutDir);
    writeDumperIni(std::filesystem::path(gDumperDll));

    logLine("dbg", "watching for %ls", gGameExe.c_str());
    logLine("dbg", "dumper   = %ls", gDumperDll.c_str());
    logLine("dbg", "profile  = %ls", gProfile.c_str());
    logLine("dbg", "out      = %ls", gOutDir.c_str());
    logLine("dbg", "min-mods = %d wait-ms = %lu", gMinModules, (unsigned long)gWaitMs);
    logLine("dbg", "elevated = %s", isElevated() ? "yes" : "no (--no-elevate)");

    fwprintf(stderr, L"\nStart the game from the launcher.\nLog: %ls\n\n", gLogPath.c_str());

    DWORD pid = run();
    if (!pid) {
        logLine("dbg", "gave up after %d seconds without injection", gSeconds);
        fwprintf(stderr, L"No injection happened. Check %ls\n", gLogPath.c_str());
        return 3;
    }
    fwprintf(stderr, L"Injected PID %lu successfully.\n", pid);
    return 0;
}
