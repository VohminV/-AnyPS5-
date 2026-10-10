#include "GameLibrary.hpp"
#include <windows.h>
#include <algorithm>

namespace launcher {

std::string GameLibrary::BaseName(const std::string& path) {
    std::string p = path;
    while (!p.empty() && (p.back() == '\\' || p.back() == '/')) p.pop_back();
    auto pos = p.find_last_of("\\/");
    std::string base = (pos == std::string::npos) ? p : p.substr(pos + 1);
    return base.empty() ? path : base;
}

static bool FileExists(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

void GameLibrary::Rescan() {
    // Merge: keep configured entries that still exist; add newly found dirs.
    for (auto it = cfg_.games.begin(); it != cfg_.games.end();) {
        if (!FileExists(it->path + "\\app.exe")) it = cfg_.games.erase(it);
        else ++it;
    }
    std::string root = cfg_.gamesDir;
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((root + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::string dir = root + "\\" + name;
        auto hasExe = [&](const std::string& d) { return FileExists(d + "\\app.exe"); };
        std::string found;
        if (hasExe(dir)) {
            found = dir;
        } else {
            // depth 2: e.g. D:\PS5_GAMES\Title\build\app.exe
            WIN32_FIND_DATAA fd2{};
            HANDLE h2 = FindFirstFileA((dir + "\\*").c_str(), &fd2);
            if (h2 != INVALID_HANDLE_VALUE) {
                do {
                    std::string n2 = fd2.cFileName;
                    if (n2 == "." || n2 == "..") continue;
                    if (!(fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                    std::string d2 = dir + "\\" + n2;
                    if (hasExe(d2)) { found = d2; break; }
                } while (FindNextFileA(h2, &fd2));
                FindClose(h2);
            }
        }
        if (!found.empty() && !cfg_.FindGame(BaseName(found))) {
            GameEntry e;
            e.id = BaseName(found);
            e.name = e.id;
            e.path = found;
            cfg_.games.push_back(e);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(cfg_.games.begin(), cfg_.games.end(),
        [](const GameEntry& a, const GameEntry& b) { return a.name < b.name; });
}

GameStatus GameLibrary::Status(const GameEntry& g) {
    GameStatus s;
    s.exePresent = FileExists(g.path + "\\app.exe");
    s.registryPresent = FileExists(g.path + "\\app.registry.json");
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((g.path + "\\libs\\*.prx").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { ++s.prxCount; } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    s.title = g.name;
    return s;
}

bool GameLibrary::AddFolder(const std::string& folder) {
    if (!FileExists(folder + "\\app.exe")) return false;
    std::string id = BaseName(folder);
    if (cfg_.FindGame(id)) return true;
    GameEntry e;
    e.id = id;
    e.name = id;
    e.path = folder;
    cfg_.games.push_back(e);
    return true;
}

void GameLibrary::Remove(const std::string& id) {
    cfg_.games.erase(std::remove_if(cfg_.games.begin(), cfg_.games.end(),
        [&](const GameEntry& g) { return g.id == id; }), cfg_.games.end());
}

} // namespace launcher
