#pragma once
// Game library: scan for relinked titles (dir with app.exe), add/remove,
// per-game status. No fake games: only entries verified on disk are listed.
#include "Config.hpp"
#include <string>
#include <vector>

namespace launcher {

struct GameStatus {
    bool exePresent = false;
    int prxCount = 0;
    bool registryPresent = false;
    std::string title; // from window title / folder
};

class GameLibrary {
public:
    explicit GameLibrary(LauncherConfig& cfg) : cfg_(cfg) {}
    void Rescan(); // scan gamesDir (depth 2) for app.exe, merge with cfg_.games
    GameStatus Status(const GameEntry& g);
    bool AddFolder(const std::string& folder); // verify app.exe inside
    void Remove(const std::string& id);
    static std::string BaseName(const std::string& path);

private:
    LauncherConfig& cfg_;
};

} // namespace launcher
