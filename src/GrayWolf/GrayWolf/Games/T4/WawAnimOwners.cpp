#include "stdafx.h"

#include "WawAnimOwners.h"

#include "AnimOwners.h"
#include "ExportNaming.h"
#include "Games/Treyarch/GscAnimOwners.h"
#include "Games/Treyarch/TreyarchGscOwners.h"

#include <filesystem>

namespace fs = std::filesystem;

namespace WawAnimOwners
{
    namespace
    {
        // the zones the game loads with Zone, the one overriding the others first: <zone>_patch, patch,
        // common; multiplayer patch_mp, common_mp
        std::vector<fs::path> SharedZones(const fs::path& Zone)
        {
            const std::string Stem = ExportNaming::Lower(Zone.stem().string());
            const bool Mp = Stem.rfind("mp_", 0) == 0 || (Stem.size() > 3 && Stem.compare(Stem.size() - 3, 3, "_mp") == 0);
            const std::vector<std::string> Names = Mp ? std::vector<std::string>{ "patch_mp", "common_mp" }
                                                      : std::vector<std::string>{ Stem + "_patch", "patch", "common" };
            std::vector<fs::path> Out;
            for (const auto& N : Names) Out.push_back(Zone.parent_path() / (N + ".ff"));
            return Out;
        }
    }

    std::vector<std::string> Documents()
    {
        return GscAnimOwners::Documents();
    }

    void Fill(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& Options,
              const std::vector<DiskGame::IDiskGame::AnimZone>& Zones, AnimOwners::Book& Out)
    {
        // World at War's rules and options are the helper's defaults
        TreyarchGscOwners::Setup S;
        S.Shared = SharedZones;
        TreyarchGscOwners::Fill(Game, Options, Zones, Out, S);
    }
}
