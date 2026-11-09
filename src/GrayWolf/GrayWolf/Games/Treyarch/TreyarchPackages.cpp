#include "stdafx.h"

#include "Games/Treyarch/TreyarchPackages.h"

#include "CoDFileSystem.h"
#include "CoDPackageCache.h"

#include <wolff/Packages.h>

#include <memory>
#include <mutex>

namespace TreyarchPackages
{
    namespace
    {
        CoDFileSystem* FileSystemOf(CoDPackageCache* Packages)
        {
            return Packages != nullptr ? Packages->GetFileSystem() : nullptr;
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
        const std::string Key = Install + '\n' + Format;
        if (Kept && Opened == Key) return Kept;
        std::shared_ptr<CoDPackageCache> C = Make();
        C->LoadPackageCacheAsync(Install);
        C->WaitForPackageCacheLoad();
        if (C->GetFileSystem() == nullptr) return nullptr;
        Kept = std::move(C);
        Opened = Key;
        return Kept;
    }
}
