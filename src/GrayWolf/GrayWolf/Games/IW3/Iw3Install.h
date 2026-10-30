#pragma once

// An install of the IW3 family's layout (CoD4, World at War, Black Ops, Black Ops II):
// zone\<folder>\x.ff, the .iwd archives in main\.

#include <memory>
#include <string>

class IWDCache;

namespace Iw3Family
{
    // zone\<folder>\x.ff: the install is two folders above the zone's folder (or Given)
    std::string InstallOf(const std::string& FastFile, const std::string& Given);

    // mods\<mod>\x.ff: the mod's folder, which holds its own .iwd archives; empty for any other zone
    std::string ModFolderOf(const std::string& FastFile);

    // The install's .iwd archives (main\*.iwd), after a mod's own (Mod: its folder) as the game
    // reads them, an image both hold being the mod's; indexed once per install and mod for the run
    std::shared_ptr<IWDCache> IwdPackagesOf(const std::string& Install, const std::string& Mod = {});
}
