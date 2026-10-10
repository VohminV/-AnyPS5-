#pragma once
// Launcher configuration: validated INI with defaults + corrupt backup.
// Location: %APPDATA%\AnyPS5Launcher\launcher.ini
#include <cstdint>
#include <string>
#include <vector>

namespace launcher {

struct DisplayProfile {
    std::string windowMode = "windowed"; // windowed | borderless | fullscreen
    int width = 1920;
    int height = 1080;
    std::string scaling = "fit"; // fit | fill | integer
    std::string gpu;             // substring for ANYPS5_GPU, empty = auto
};

struct InputProfile {
    int deadzone = 10;   // 0..64
    bool rumble = true;
    bool swapAB = false;
};

struct GameEntry {
    std::string id;      // folder name
    std::string name;    // display name
    std::string path;    // game dir with app.exe
    DisplayProfile display;
    InputProfile input;
    bool useGlobalDisplay = true;
    bool useGlobalInput = true;
};

struct LauncherConfig {
    std::string gamesDir = "D:\\PS5_GAMES";
    std::string selectedGame;
    DisplayProfile display;
    InputProfile input;
    std::vector<GameEntry> games;

    static std::string ConfigDir();
    static std::string ConfigPath();
    static std::string LogPath();
    bool Load();   // false = defaults used (also backs up corrupt file)
    bool Save();
    void SetDefaults();
    GameEntry* FindGame(const std::string& id);
};

bool IsValidWindowMode(const std::string& m);
bool IsValidScaling(const std::string& s);
int ClampInt(int v, int lo, int hi);

} // namespace launcher
