#include "stdafx.h"

#include "IwInstall.h"

#include "ExportNaming.h"
#include "ZipInflate.h"
#include <miniz/miniz.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace IW12
{
struct Install::Archive
{
    mz_zip_archive Zip;
    bool Open = false;
    std::mutex Lock;   // a miniz archive reads through one file handle
    Archive() { std::memset(&Zip, 0, sizeof(Zip)); }
    ~Archive()
    {
        if (Open) mz_zip_reader_end(&Zip);
    }
};

std::string NormalPath(std::string Path)
{
    std::replace(Path.begin(), Path.end(), '\\', '/');
    return ExportNaming::Lower(std::move(Path));
}

std::vector<std::string> Install::Archives(const std::string& Folder, const std::string& Extension, const std::string& Language)
{
    std::vector<std::string> Out;
    std::error_code Ec;
    const auto Scan = [&](const fs::path& Dir) {
        for (const auto& E : fs::directory_iterator(Dir, Ec))
            if (E.is_regular_file(Ec) && ExportNaming::Lower(E.path().extension().string()) == Extension)
                Out.push_back(E.path().string());
    };
    Scan(Folder);
    if (Out.empty()) Scan(fs::path(Folder) / "main");
    // by rank (other languages' localized archives, the base, the chosen language's), then by name;
    // a later archive's copy of a path is the one read
    const std::string Own = "localized_" + ExportNaming::Lower(Language) + "_";
    const auto Rank = [&](const std::string& Path) {
        const std::string Name = ExportNaming::Lower(fs::path(Path).filename().string());
        if (Name.compare(0, 10, "localized_") != 0) return 1;
        return Name.compare(0, Own.size(), Own) == 0 ? 2 : 0;
    };
    std::sort(Out.begin(), Out.end(), [&](const std::string& A, const std::string& B) {
        const int Ra = Rank(A), Rb = Rank(B);
        if (Ra != Rb) return Ra < Rb;
        return ExportNaming::Lower(fs::path(A).filename().string()) < ExportNaming::Lower(fs::path(B).filename().string());
    });
    return Out;
}

std::shared_ptr<Install> Install::Open(const std::string& Folder, const std::string& Extension, const std::string& Language,
                                       std::string& Error)
{
    static std::mutex CacheLock;
    static std::map<std::string, std::shared_ptr<Install>> Cache;
    const std::vector<std::string> Paths = Archives(Folder, Extension, Language);
    if (Paths.empty())
    {
        Error = "no " + Extension + " archive in " + Folder;
        return nullptr;
    }
    const std::string Main = fs::path(Paths.front()).parent_path().string();
    const std::string Key = NormalPath(Main) + "|" + Extension + "|" + ExportNaming::Lower(Language);
    std::lock_guard<std::mutex> Guard(CacheLock);
    if (auto It = Cache.find(Key); It != Cache.end() && It->second->Paths == Paths) return It->second;

    auto I = std::shared_ptr<Install>(new Install());
    I->MainFolder = Main;
    I->Paths = Paths;
    I->Owned.resize(Paths.size());
    for (size_t A = 0; A < Paths.size(); A++)
    {
        auto Z = std::make_unique<Archive>();
        if (!mz_zip_reader_init_file(&Z->Zip, Paths[A].c_str(), 0))
        {
            Error = "not a zip: " + Paths[A];
            return nullptr;
        }
        Z->Open = true;
        const mz_uint Count = mz_zip_reader_get_num_files(&Z->Zip);
        for (mz_uint F = 0; F < Count; F++)
        {
            if (mz_zip_reader_is_file_a_directory(&Z->Zip, F)) continue;
            mz_zip_archive_file_stat Stat;
            if (!mz_zip_reader_file_stat(&Z->Zip, F, &Stat)) continue;
            const std::string Name = NormalPath(Stat.m_filename);
            I->Winner[Name] = (int)A;
            I->WinnerIndex[Name] = F;
        }
        I->Zips.push_back(std::move(Z));
    }
    for (const auto& [Name, A] : I->Winner) I->Owned[A].push_back(Name);
    Cache[Key] = I;
    return I;
}

Install::~Install() = default;

int Install::Owner(const std::string& Path) const
{
    const auto It = Winner.find(Path);
    return It == Winner.end() ? -1 : It->second;
}

int Install::ArchiveOf(const std::string& ArchivePath) const
{
    const std::string Want = ExportNaming::Lower(fs::path(ArchivePath).filename().string());
    for (size_t A = 0; A < Paths.size(); A++)
        if (ExportNaming::Lower(fs::path(Paths[A]).filename().string()) == Want) return (int)A;
    return -1;
}

uint64_t Install::Size(const std::string& Path) const
{
    const auto It = Winner.find(Path);
    if (It == Winner.end()) return 0;
    Archive& Z = *Zips[It->second];
    std::lock_guard<std::mutex> Guard(Z.Lock);
    mz_zip_archive_file_stat Stat;
    return mz_zip_reader_file_stat(&Z.Zip, WinnerIndex.at(Path), &Stat) ? Stat.m_uncomp_size : 0;
}

bool Install::Read(const std::string& Path, std::vector<uint8_t>& Out) const
{
    const auto It = Winner.find(Path);
    if (It == Winner.end()) return false;
    Archive& Z = *Zips[It->second];
    // the archive read under its lock, inflated beside the other threads' reads
    return ZipInflate::Extract(&Z.Zip, WinnerIndex.at(Path), Out, Z.Lock);
}
}
