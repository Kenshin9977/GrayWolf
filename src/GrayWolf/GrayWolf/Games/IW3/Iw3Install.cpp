#include "stdafx.h"

#include "Games/IW3/Iw3Install.h"

#include "ExportNaming.h"
#include "IWDCache.h"

#include <filesystem>
#include <map>
#include <mutex>

namespace fs = std::filesystem;

namespace Iw3Family
{
    std::string InstallOf(const std::string& FastFile, const std::string& Given)
    {
        if (!Given.empty()) return Given;
        const fs::path P = fs::path(FastFile).parent_path();
        if (ExportNaming::Lower(P.filename().string()) == "zone") return P.parent_path().string();
        return P.parent_path().parent_path().string();
    }

    std::string ModFolderOf(const std::string& FastFile)
    {
        const fs::path Dir = fs::path(FastFile).parent_path();
        return ExportNaming::Lower(Dir.parent_path().filename().string()) == "mods" ? Dir.string() : std::string();
    }

    std::shared_ptr<IWDCache> IwdPackagesOf(const std::string& Install, const std::string& Mod)
    {
        static std::mutex Lock;
        static std::map<std::pair<std::string, std::string>, std::shared_ptr<IWDCache>> Packages;
        std::lock_guard<std::mutex> G(Lock);
        auto& P = Packages[{ Install, Mod }];
        if (!P)
        {
            P = std::make_shared<IWDCache>();
            if (!Mod.empty()) P->LoadArchives(Mod);
            P->LoadPackageCache((fs::path(Install) / "main").string());
        }
        return P;
    }
}
