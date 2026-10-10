#pragma once
// Launch backend / Relink adapter: single-instance guard, CreateProcess with
// ANYPS5_* env, stdout/stderr capture, exit monitoring, live FPS via title.
#include "Config.hpp"
#include <windows.h>
#include <string>
#include <functional>

namespace launcher {

struct RunResult {
    DWORD exitCode = 0;
    double seconds = 0.0;
    bool crashed = false;
};

class LaunchBackend {
public:
    using LogFn = std::function<void(const std::string&)>;
    explicit LaunchBackend(LogFn log) : log_(log) {}
    ~LaunchBackend() { StopMonitor(); }

    // Effective profile for a game (global or per-game override).
    static DisplayProfile EffDisplay(const LauncherConfig& c, const GameEntry& g);
    static InputProfile EffInput(const LauncherConfig& c, const GameEntry& g);

    bool IsGameRunning(const GameEntry& g);
    bool FocusGame(const GameEntry& g);
    // Returns false if already running (and focuses it) or on spawn error.
    bool Launch(LauncherConfig& cfg, GameEntry& g);
    void StopMonitor();
    // Live FPS parsed from the game window title ("FPS: 12.34").
    static bool ReadGameFps(const GameEntry& g, double& fps, std::string& title);
    static bool FindGameWindow(const GameEntry& g, HWND& out);
    std::string LastRunLog() const { return lastRunLog_; }

private:
    bool spawn(LauncherConfig& cfg, GameEntry& g);
    static void CALLBACK FpsTimerCb(HWND, UINT, UINT_PTR, DWORD);

    LogFn log_;
    HANDLE proc_ = nullptr;
    DWORD pid_ = 0;
    HANDLE thread_ = nullptr;
    UINT_PTR fpsTimer_ = 0;
    std::string lastRunLog_;
    std::string runningId_;
};

void AppendLog(const std::string& path, const std::string& line);

} // namespace launcher
