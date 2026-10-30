#pragma once

#include <string>
#include <vector>

#include "Languages.h"

// The Call of Duty games installed on this machine, for the Load Game panel and the CLI. One table
// of titles (GameInstalls.cpp) says, per game, how it is recognised (Steam app ids, an exe in its
// folder) and which disk reader (DiskGame id) reads it.
namespace GameInstalls
{
    struct Install
    {
        std::string Path;       // the install folder
        std::string Display;    // "World at War"
        std::string Label;      // Display, plus the drive and folder when another install has the same Display
        int Year = 0;           // release year, the list's order
        std::string DiskId;     // the disk reader (DiskGame::Find), "" when none
        std::string Source;     // "Steam", "Battle.net" or "installer" (uninstall keys), "Xbox" (Game Pass), "folder" (found on disk, no launcher)
        // the language it is played in and those installed, as its launcher's files say
        // (Languages::OfInstall: Battle.net, Steam); none known for the others
        Languages::Detected Language;
    };

    // Steam's app manifests first (every library in libraryfolders.vdf: the app id names the game,
    // fully installed only), then the uninstall keys (Battle.net, installers), the Xbox app's
    // packages (XboxPackages) and a shallow sweep of the usual folders for copies no launcher
    // manages, all but Steam by an exe in the folder. One entry per
    // folder (the final path, case-insensitive), sorted by release year.
    std::vector<Install> Detect();
    // The game's own exe for a folder picked to read (its icon is the game's): a marker exe of the
    // table in the folder or in one of its two parents (an install given as its zone or cod24
    // folder); "" when none is there
    std::string GameExe(const std::string& Folder);}
