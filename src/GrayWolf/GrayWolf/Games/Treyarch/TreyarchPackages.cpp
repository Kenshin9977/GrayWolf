#include "stdafx.h"

#include "Games/Treyarch/TreyarchPackages.h"

#include "CoDFileSystem.h"
#include "CoDPackageCache.h"

#include <wolff/Packages.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <regex>

namespace TreyarchPackages
{
    namespace
    {
        CoDFileSystem* FileSystemOf(CoDPackageCache* Packages)
        {
            return Packages != nullptr ? Packages->GetFileSystem() : nullptr;
        }

        // A title's folder of a Call of Duty HQ install (cod24, cod26, sp26...) whose packages are
        // also shared from the HQ folder above it: that folder, else Install as it is
        std::string PackageRoot(const std::string& Install)
        {
            namespace fs = std::filesystem;
            const fs::path P = fs::path(Install).lexically_normal();
            const fs::path Dir = P.has_filename() ? P : P.parent_path();
            if (!std::regex_match(Dir.filename().string(), std::regex("(cod|sp)\\d\\d", std::regex::icase))) return Install;
            std::error_code Ec;
            for (const auto& E : fs::directory_iterator(Dir.parent_path(), fs::directory_options::skip_permission_denied, Ec))
                if (E.is_regular_file(Ec) && E.path().extension() == ".xsub") return Dir.parent_path().string();
            return Install;
        }
    }

    std::vector<std::string> ListFastFiles(CoDPackageCache* Packages)
    {
        return wolff::Packages::ListFastFiles(FileSystemOf(Packages));
    }

    std::vector<std::string> ExtractFastFiles(CoDPackageCache* Packages, const std::string& Pattern,
                                              const std::string& CacheDir, std::string& Error, const char* PatchExtension)
    {
        return wolff::Packages::ExtractFastFiles(FileSystemOf(Packages), Pattern, CacheDir, Error, PatchExtension);
    }

    bool ExtractZones(CoDPackageCache* Packages, const std::string& Install, const std::vector<std::string>& Names,
                      const std::string& CacheDir, std::vector<std::string>& Out, std::string& Error, const char* PatchExtension)
    {
        return wolff::Packages::ExtractZones(FileSystemOf(Packages), Install, Names, CacheDir, Out, Error, PatchExtension);
    }

    std::shared_ptr<CoDPackageCache> Open(const std::string& Install, const char* Format, Factory Make)
    {
        // not CoDAssets' package cache: that one is the served zone's, set under the session lock
        // (this runs on the index's workers too, while a served zone's readers read it)
        static std::mutex Lock;
        static std::string Opened;
        static std::shared_ptr<CoDPackageCache> Kept;
        std::lock_guard<std::mutex> Guard(Lock);
        const std::string Root = PackageRoot(Install);
        const std::string Key = Root + '\n' + Format;
        if (Kept && Opened == Key) return Kept;
        std::shared_ptr<CoDPackageCache> C = Make();
        C->LoadPackageCacheAsync(Root);
        C->WaitForPackageCacheLoad();
        if (C->GetFileSystem() == nullptr) return nullptr;
        Kept = std::move(C);
        Opened = Key;
        return Kept;
    }
}
