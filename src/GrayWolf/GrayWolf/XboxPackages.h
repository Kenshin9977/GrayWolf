#pragma once

#include <string>
#include <vector>

// The games the Xbox app (PC Game Pass, Microsoft Store) installed: MSIXVC packages, each a folder
// with a MicrosoftGame.config (the GDK's manifest). A game's DLC is a package of its own, in its own
// folder, whose config names the game's package (DesktopRegistration/MainPackageDependency); a
// Black Ops III copy has its multiplayer maps in such a package ("MP Starter Pack"), beside the
// base game's folder. A package's files are plain files, readable without the Xbox app.
namespace XboxPackages
{
    struct Package
    {
        std::string Root;          // the package's folder (the one holding MicrosoftGame.config)
        std::string Name;          // its identity name, "38985CA0.CallofDutyBlackOps3PCMS"
        std::string MainPackage;   // a DLC: the identity name of the game it belongs to, else ""
    };

    // Every package installed on this machine: the Gaming Services repository (where the Xbox app
    // records each package's folder, WindowsApps or an XboxGames folder on any drive), then the
    // XboxGames folders each drive's .GamingRoot names (<root>\<game>\Content), for copies the
    // repository no longer lists. One entry per folder; folders without a MicrosoftGame.config
    // are left out.
    std::vector<Package> Installed();

    // The package a folder is in (the folder itself or one of its parents holds its
    // MicrosoftGame.config); false outside a package
    bool Containing(const std::string& Folder, Package& Out);

    // A game's DLC packages: those naming Root's package as their main package ([] when Root is
    // not a package)
    std::vector<Package> Dlc(const std::string& Root);

    // The same folder in the other packages of a game (its main package and every DLC), for a
    // folder inside one of them: a DLC zone's packages (.xpak) beside the base game's. Only the
    // folders that exist; [] outside a package.
    std::vector<std::string> SameFolderInGame(const std::string& Folder);
}
