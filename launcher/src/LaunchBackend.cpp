#include "LaunchBackend.hpp"
#include <psapi.h>
#include <chrono>
#include <cstdio>
#include <sstream>
#include <vector>

namespace launcher {

void AppendLog(const std::string& path, const std::string& line) {
    HANDLE h = CreateFileA(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char head[64];
    snprintf(head, sizeof(head), "[%04u-%02u-%02u %02u:%02u:%02u] ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    DWORD w = 0;
    WriteFile(h, head, (DWORD)strlen(head), &w, nullptr);
    WriteFile(h, line.c_str(), (DWORD)line.size(), &w, nullptr);
    const char nl = '\n';
    WriteFile(h, &nl, 1, &w, nullptr);
    CloseHandle(h);
}

DisplayProfile LaunchBackend::EffDisplay(const LauncherConfig& c, const GameEntry& g) {
    return g.useGlobalDisplay ? c.display : g.display;
}
InputProfile LaunchBackend::EffInput(const LauncherConfig& c, const GameEntry& g) {
    return g.useGlobalInput ? c.input : g.input;
}

static std::string ExeOf(const GameEntry& g) { return g.path + "\\app.exe"; }

static bool PidIsOurGame(DWORD pid, const GameEntry& g) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    char path[MAX_PATH] = {};
    DWORD n = sizeof(path);
    bool ok = QueryFullProcessImageNameA(h, 0, path, &n) != 0;
    CloseHandle(h);
    if (!ok) return false;
    std::string exe = ExeOf(g);
    return _stricmp(path, exe.c_str()) == 0;
}

struct FindCtx {
    const GameEntry* g;
    HWND found = nullptr;
    DWORD wantedPid = 0;
};

static BOOL CALLBACK EnumWin(HWND hwnd, LPARAM lp) {
    FindCtx* ctx = reinterpret_cast<FindCtx*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (ctx->wantedPid ? (pid != ctx->wantedPid) : !PidIsOurGame(pid, *ctx->g)) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    char cls[64] = {};
    GetClassNameA(hwnd, cls, sizeof(cls));
    if (strcmp(cls, "SDL_app") != 0) {
        // SDL window class; still accept any visible top-level of the process.
    }
    ctx->found = hwnd;
    return FALSE;
}

bool LaunchBackend::FindGameWindow(const GameEntry& g, HWND& out) {
    FindCtx ctx{&g};
    EnumWindows(EnumWin, reinterpret_cast<LPARAM>(&ctx));
    out = ctx.found;
    return out != nullptr;
}

bool LaunchBackend::IsGameRunning(const GameEntry& g) {
    if (proc_ && runningId_ == g.id) {
        if (WaitForSingleObject(proc_, 0) == WAIT_TIMEOUT) return true;
    }
    // External check: any app.exe of this game alive? (EnumProcesses, UNICODE-safe)
    DWORD pids[1024] = {};
    DWORD needed = 0;
    if (!EnumProcesses(pids, sizeof(pids), &needed)) return false;
    std::string exe = ExeOf(g);
    for (DWORD i = 0; i < needed / sizeof(DWORD); ++i) {
        if (!pids[i]) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pids[i]);
        if (!h) continue;
        char path[MAX_PATH] = {};
        DWORD n = sizeof(path);
        BOOL ok = QueryFullProcessImageNameA(h, 0, path, &n);
        CloseHandle(h);
        if (ok && _stricmp(path, exe.c_str()) == 0) return true;
    }
    return false;
}

bool LaunchBackend::FocusGame(const GameEntry& g) {
    HWND w = nullptr;
    if (!FindGameWindow(g, w)) return false;
    if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
    SetForegroundWindow(w);
    return true;
}

bool LaunchBackend::ReadGameFps(const GameEntry& g, double& fps, std::string& title) {
    HWND w = nullptr;
    if (!FindGameWindow(g, w)) return false;
    char buf[512] = {};
    GetWindowTextA(w, buf, sizeof(buf));
    title = buf;
    auto pos = title.find("FPS:");
    if (pos == std::string::npos) return false;
    fps = atof(title.c_str() + pos + 4);
    return fps > 0.0;
}

static std::string EnvBlock(const DisplayProfile& d, const InputProfile& in) {
    // Build extra env vars; CreateProcess merges via lpEnvironment=NULL + SetEnvironment?
    // We set them process-wide temporarily under a lock instead: simplest and
    // reliable for a launcher (single launch at a time).
    (void)d; (void)in;
    return {};
}

bool LaunchBackend::Launch(LauncherConfig& cfg, GameEntry& g) {
    if (IsGameRunning(g)) {
        FocusGame(g);
        log_("launch: already running, focused");
        return false;
    }
    return spawn(cfg, g);
}

struct SpawnCtx {
    LaunchBackend* self;
    LauncherConfig cfg;
    GameEntry game;
};

bool LaunchBackend::spawn(LauncherConfig& cfg, GameEntry& g) {
    DisplayProfile d = EffDisplay(cfg, g);
    InputProfile in = EffInput(cfg, g);
    // Apply as process env for the child (inherited). Save previous to restore.
    auto setEnv = [](const char* k, const std::string& v) { SetEnvironmentVariableA(k, v.c_str()); };
    char prevMode[64] = {}, prevW[32] = {}, prevH[32] = {}, prevS[32] = {}, prevGpu[128] = {}, prevDz[32] = {}, prevRum[8] = {}, prevSwap[8] = {};
    GetEnvironmentVariableA("ANYPS5_WINDOW_MODE", prevMode, sizeof(prevMode));
    GetEnvironmentVariableA("ANYPS5_WINDOW_W", prevW, sizeof(prevW));
    GetEnvironmentVariableA("ANYPS5_WINDOW_H", prevH, sizeof(prevH));
    GetEnvironmentVariableA("ANYPS5_SCALING", prevS, sizeof(prevS));
    GetEnvironmentVariableA("ANYPS5_GPU", prevGpu, sizeof(prevGpu));
    GetEnvironmentVariableA("ANYPS5_STICK_DEADZONE", prevDz, sizeof(prevDz));
    GetEnvironmentVariableA("ANYPS5_RUMBLE", prevRum, sizeof(prevRum));
    GetEnvironmentVariableA("ANYPS5_SWAP_AB", prevSwap, sizeof(prevSwap));
    setEnv("ANYPS5_WINDOW_MODE", d.windowMode);
    setEnv("ANYPS5_WINDOW_W", std::to_string(d.width));
    setEnv("ANYPS5_WINDOW_H", std::to_string(d.height));
    setEnv("ANYPS5_SCALING", d.scaling);
    setEnv("ANYPS5_GPU", d.gpu);
    setEnv("ANYPS5_STICK_DEADZONE", std::to_string(in.deadzone));
    setEnv("ANYPS5_RUMBLE", in.rumble ? "1" : "0");
    setEnv("ANYPS5_SWAP_AB", in.swapAB ? "1" : "0");

    std::string exe = ExeOf(g);
    std::string logDir = LauncherConfig::ConfigDir() + "\\logs";
    CreateDirectoryA(logDir.c_str(), nullptr);
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    lastRunLog_ = logDir + "\\run-" + g.id + "-" + stamp + ".log";

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rOut = nullptr, wOut = nullptr;
    CreatePipe(&rOut, &wOut, &sa, 0);
    SetHandleInformation(rOut, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wOut;
    si.hStdError = wOut;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::string cmd = "\"" + exe + "\"";
    BOOL ok = CreateProcessA(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
        CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT, nullptr, g.path.c_str(), &si, &pi);
    // Restore our env immediately (child has its copy).
    auto restore = [&](const char* k, const char* prev) {
        if (prev[0]) SetEnvironmentVariableA(k, prev);
        else SetEnvironmentVariableA(k, nullptr);
    };
    restore("ANYPS5_WINDOW_MODE", prevMode);
    restore("ANYPS5_WINDOW_W", prevW);
    restore("ANYPS5_WINDOW_H", prevH);
    restore("ANYPS5_SCALING", prevS);
    restore("ANYPS5_GPU", prevGpu);
    restore("ANYPS5_STICK_DEADZONE", prevDz);
    restore("ANYPS5_RUMBLE", prevRum);
    restore("ANYPS5_SWAP_AB", prevSwap);
    CloseHandle(wOut);

    if (!ok) {
        DWORD err = GetLastError();
        char msg[256];
        snprintf(msg, sizeof(msg), "launch: CreateProcess failed err=%lu exe=%s", (unsigned long)err, exe.c_str());
        log_(msg);
        AppendLog(LauncherConfig::LogPath(), msg);
        CloseHandle(rOut);
        return false;
    }
    CloseHandle(pi.hThread);
    proc_ = pi.hProcess;
    pid_ = pi.dwProcessId;
    runningId_ = g.id;
    {
        char msg[256];
        snprintf(msg, sizeof(msg), "launch: started %s pid=%lu mode=%s %dx%d scaling=%s", exe.c_str(),
            (unsigned long)pid_, d.windowMode.c_str(), d.width, d.height, d.scaling.c_str());
        log_(msg);
        AppendLog(LauncherConfig::LogPath(), msg);
    }
    // Pump child stdout to the run log on a worker thread.
    HANDLE hLog = CreateFileA(lastRunLog_.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE src = rOut;
    thread_ = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        auto* ctx = static_cast<std::pair<HANDLE, HANDLE>*>(p);
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(ctx->first, buf, sizeof(buf), &n, nullptr) && n) {
            DWORD w = 0;
            WriteFile(ctx->second, buf, n, &w, nullptr);
        }
        CloseHandle(ctx->first);
        CloseHandle(ctx->second);
        delete ctx;
        return 0;
    }, new std::pair<HANDLE, HANDLE>(src, hLog), 0, nullptr);
    cfg.selectedGame = g.id;
    cfg.Save();
    return true;
}

void LaunchBackend::StopMonitor() {
    if (thread_) {
        // The logger thread exits when the child closes its stdout (process end).
        WaitForSingleObject(thread_, 3000);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    if (proc_) {
        DWORD code = 0;
        bool running = WaitForSingleObject(proc_, 0) == WAIT_TIMEOUT;
        if (!running) {
            GetExitCodeProcess(proc_, &code);
            char msg[128];
            snprintf(msg, sizeof(msg), "launch: process ended code=%lu", (unsigned long)code);
            log_(msg);
            AppendLog(LauncherConfig::LogPath(), msg);
        }
        CloseHandle(proc_);
        proc_ = nullptr;
        pid_ = 0;
        runningId_.clear();
    }
}

} // namespace launcher
