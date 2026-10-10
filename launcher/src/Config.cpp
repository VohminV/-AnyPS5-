// Minimal INI config with validation, defaults and corrupt-file backup.
// No third-party JSON: format is key=value + [game:<id>] sections.
#include "Config.hpp"
#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace launcher {
namespace {
std::string trim(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}
} // namespace

bool IsValidWindowMode(const std::string& m) {
    return m == "windowed" || m == "borderless" || m == "fullscreen";
}
bool IsValidScaling(const std::string& s) {
    return s == "fit" || s == "fill" || s == "integer";
}
int ClampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

std::string LauncherConfig::ConfigDir() {
    char path[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, path))) {
        return std::string(path) + "\\AnyPS5Launcher";
    }
    return ".\\AnyPS5Launcher";
}
std::string LauncherConfig::ConfigPath() { return ConfigDir() + "\\launcher.ini"; }
std::string LauncherConfig::LogPath() { return ConfigDir() + "\\launcher.log"; }

void LauncherConfig::SetDefaults() {
    gamesDir = "D:\\PS5_GAMES";
    selectedGame.clear();
    display = DisplayProfile{};
    input = InputProfile{};
    games.clear();
}

bool LauncherConfig::Load() {
    SetDefaults();
    const std::string path = ConfigPath();
    std::ifstream f(path);
    if (!f) return false; // no file yet: defaults
    GameEntry* cur = nullptr;
    std::string line;
    bool corrupt = false;
    int lines = 0;
    while (std::getline(f, line)) {
        ++lines;
        if (lines == 1 && line.size() >= 3 && (unsigned char)line[0] == 0xEF) line.erase(0, 3);
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            std::string sec = line.substr(1, line.size() - 2);
            if (sec.rfind("game:", 0) == 0) {
                games.push_back(GameEntry{});
                cur = &games.back();
                cur->id = sec.substr(5);
                cur->name = cur->id;
            } else {
                cur = nullptr;
            }
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) { corrupt = true; continue; }
        std::string k = trim(line.substr(0, eq));
        std::string v = trim(line.substr(eq + 1));
        auto setDisplay = [&](DisplayProfile& d, const std::string& key, const std::string& val) -> bool {
            if (key == "windowMode") { if (IsValidWindowMode(val)) { d.windowMode = val; return true; } return false; }
            if (key == "width") { d.width = ClampInt(atoi(val.c_str()), 320, 7680); return true; }
            if (key == "height") { d.height = ClampInt(atoi(val.c_str()), 180, 4320); return true; }
            if (key == "scaling") { if (IsValidScaling(val)) { d.scaling = val; return true; } return false; }
            if (key == "gpu") { d.gpu = val.substr(0, 64); return true; }
            return true;
        };
        auto setInput = [&](InputProfile& p, const std::string& key, const std::string& val) -> bool {
            if (key == "deadzone") { p.deadzone = ClampInt(atoi(val.c_str()), 0, 64); return true; }
            if (key == "rumble") { p.rumble = !(val == "0" || val == "false"); return true; }
            if (key == "swapAB") { p.swapAB = (val == "1" || val == "true"); return true; }
            return true;
        };
        bool ok = true;
        if (cur) {
            if (k == "name") cur->name = v.substr(0, 128);
            else if (k == "path") cur->path = v.substr(0, 260);
            else if (k == "useGlobalDisplay") cur->useGlobalDisplay = !(v == "0" || v == "false");
            else if (k == "useGlobalInput") cur->useGlobalInput = !(v == "0" || v == "false");
            else if (!setDisplay(cur->display, k, v)) ok = false;
            else if (!setInput(cur->input, k, v)) ok = false;
        } else {
            if (k == "gamesDir") { if (!v.empty()) gamesDir = v.substr(0, 260); }
            else if (k == "selectedGame") selectedGame = v.substr(0, 128);
            else if (!setDisplay(display, k, v)) ok = false;
            else if (!setInput(input, k, v)) ok = false;
        }
        if (!ok) corrupt = true;
    }
    if (corrupt) {
        // Back up the broken file, keep running on safe defaults for bad keys.
        std::string bak = path + ".corrupt.bak";
        CopyFileA(path.c_str(), bak.c_str(), FALSE);
    }
    return true;
}

bool LauncherConfig::Save() {
    CreateDirectoryA(ConfigDir().c_str(), nullptr);
    CreateDirectoryA((ConfigDir() + "\\logs").c_str(), nullptr);
    const std::string path = ConfigPath();
    const std::string tmp = path + ".tmp";
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) return false;
    f << "# AnyPS5 Launcher configuration (key=value, [game:<id>] sections)\n";
    f << "gamesDir=" << gamesDir << "\n";
    f << "selectedGame=" << selectedGame << "\n";
    f << "windowMode=" << display.windowMode << "\n";
    f << "width=" << display.width << "\nheight=" << display.height << "\n";
    f << "scaling=" << display.scaling << "\n";
    f << "gpu=" << display.gpu << "\n";
    f << "deadzone=" << input.deadzone << "\n";
    f << "rumble=" << (input.rumble ? 1 : 0) << "\n";
    f << "swapAB=" << (input.swapAB ? 1 : 0) << "\n";
    for (auto& g : games) {
        f << "\n[game:" << g.id << "]\n";
        f << "name=" << g.name << "\npath=" << g.path << "\n";
        f << "useGlobalDisplay=" << (g.useGlobalDisplay ? 1 : 0) << "\n";
        f << "useGlobalInput=" << (g.useGlobalInput ? 1 : 0) << "\n";
        f << "windowMode=" << g.display.windowMode << "\n";
        f << "width=" << g.display.width << "\nheight=" << g.display.height << "\n";
        f << "scaling=" << g.display.scaling << "\n";
        f << "gpu=" << g.display.gpu << "\n";
        f << "deadzone=" << g.input.deadzone << "\n";
        f << "rumble=" << (g.input.rumble ? 1 : 0) << "\n";
        f << "swapAB=" << (g.input.swapAB ? 1 : 0) << "\n";
    }
    f.close();
    if (!f) return false;
    MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    return true;
}

GameEntry* LauncherConfig::FindGame(const std::string& id) {
    for (auto& g : games)
        if (g.id == id) return &g;
    return nullptr;
}

} // namespace launcher
