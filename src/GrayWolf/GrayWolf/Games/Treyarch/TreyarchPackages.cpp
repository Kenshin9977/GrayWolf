#include "stdafx.h"

#include "Games/Treyarch/TreyarchPackages.h"

#include "CoDFileSystem.h"
#include "CoDPackageCache.h"
#include "ExportNaming.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace fs = std::filesystem;

namespace TreyarchPackages
{
    using ExportNaming::Lower;

    std::vector<std::string> ListFastFiles(CoDPackageCache* Packages)
    {
        std::vector<std::string> Out;
        CoDFileSystem* FS = Packages ? Packages->GetFileSystem() : nullptr;
        if (FS == nullptr) return Out;
        FS->EnumerateFiles("*.ff", [&](const std::string& Name, const size_t) {
            Out.push_back(fs::path(Name).filename().string());
        });
        std::sort(Out.begin(), Out.end());
        Out.erase(std::unique(Out.begin(), Out.end()), Out.end());
        return Out;
    }

    std::vector<std::string> ExtractFastFiles(CoDPackageCache* Packages, const std::string& Pattern,
                                              const std::string& CacheDir, std::string& Error, const char* PatchExtension)
    {
        std::vector<std::string> Out;
        CoDFileSystem* FS = Packages ? Packages->GetFileSystem() : nullptr;
        if (FS == nullptr)
        {
            Error = "no package file system";
            return Out;
        }
        std::error_code Ec;
        fs::create_directories(CacheDir, Ec);
        std::vector<std::pair<std::string, size_t>> Files;
        // a pattern is matched against the whole package path; a bare name (no wildcard) means that file
        const bool Exact = Pattern.find_first_of("*?") == std::string::npos;
        FS->EnumerateFiles(Exact ? "*.ff" : Pattern, [&](const std::string& Name, const size_t Size) {
            if (!Exact || Lower(fs::path(Name).filename().string()) == Lower(Pattern)) Files.emplace_back(Name, Size);
        });
        auto Pull = [&](const std::string& Name, size_t Size) -> std::string {
            const fs::path Local = fs::path(CacheDir) / fs::path(Name).filename();
            if (fs::exists(Local, Ec) && fs::file_size(Local, Ec) == Size) return Local.string();
            HANDLE H = FS->OpenFile(Name, "r");
            if (H == NULL) return "";
            auto Buf = std::make_unique<uint8_t[]>(Size);
            FS->Read(H, Buf.get(), 0, Size);
            FS->CloseHandle(H);
            std::ofstream F(Local, std::ios::binary | std::ios::trunc);
            F.write(reinterpret_cast<const char*>(Buf.get()), static_cast<std::streamsize>(Size));
            return F ? Local.string() : "";
        };
        // the patches, by file stem: a pattern is matched against the whole package path
        std::unordered_map<std::string, std::pair<std::string, size_t>> Patches;
        FS->EnumerateFiles(std::string("*") + PatchExtension, [&](const std::string& Name, const size_t Size) {
            Patches[Lower(fs::path(Name).stem().string())] = {Name, Size};
        });
        for (const auto& [Name, Size] : Files)
        {
            if (Lower(fs::path(Name).extension().string()) != ".ff" || Size == 0) continue;
            const std::string Local = Pull(Name, Size);
            if (Local.empty()) continue;
            const auto Patch = Patches.find(Lower(fs::path(Name).stem().string()));
            if (Patch != Patches.end() && Patch->second.second != 0)
                Pull(Patch->second.first, Patch->second.second);
            Out.push_back(Local);
        }
        return Out;
    }

    bool ExtractZones(CoDPackageCache* Packages, const std::string& Install, const std::vector<std::string>& Names,
                      const std::string& CacheDir, std::vector<std::string>& Out, std::string& Error, const char* PatchExtension)
    {
        for (const auto& N : Names)
        {
            const auto Paths = ExtractFastFiles(Packages, N, CacheDir, Error, PatchExtension);
            Out.insert(Out.end(), Paths.begin(), Paths.end());
        }
        if (Out.empty() && Error.empty()) Error = "no picked zone in " + Install;
        return !Out.empty();
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
