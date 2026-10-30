#include "stdafx.h"

#include "CoDPackageCache.h"
#include "FileSystems.h"

#include "WinFileSystem.h"
#include "CascFileSystem.h"
#include "ExportNaming.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

CoDPackageCache::CoDPackageCache()
{
    HasLoaded = false;
    CacheLoading = false;
}

CoDPackageCache::~CoDPackageCache()
{
    CacheObjects.clear();
    PackageFilePaths.clear();
    PackageFilePaths.shrink_to_fit();
}

bool CoDPackageCache::IsCacheLoading()
{
    std::lock_guard<std::mutex> Lock(CacheMutex);

    return CacheLoading;
}

void CoDPackageCache::LoadPackageCache(const std::string& BasePath)
{
    std::error_code Ec;
    std::lock_guard<std::mutex> Lock(CacheMutex);

#ifdef _DEBUG
    printf("LoadPackageCache(): Begin loading...\n");
#endif

    CacheLoading = true;

    // Open the file system, check for build info, if build info exists
    // we'll use Casc, otherwise use raw directory.
    if (std::filesystem::is_regular_file(BasePath + "\\.build.info", Ec))
    {
        FileSystem = std::make_unique<CascFileSystem>(BasePath);
    }
    else
    {
        FileSystem = std::make_unique<WinFileSystem>(BasePath);
    }

    if (!FileSystem->IsValid())
    {
        // Say so: otherwise every later extraction silently returns nothing (Black Ops 4's assets
        // live in a Battle.net CASC container, not in loose files).
        printf("LoadPackageCache(): could not open '%s' - packaged assets will be unavailable\n",
               BasePath.c_str());

        FileSystem = nullptr;
    }
}

bool CoDPackageCache::LoadPackage(const std::string& FilePath)
{
    // Nothing, default, but ensure status is loading
    CacheLoading = true;
    HasLoaded = false;
    return true;
}

void CoDPackageCache::SetLoadedState()
{
    std::lock_guard<std::mutex> Lock(CacheMutex);

#ifdef _DEBUG
    printf("SetLoadedState(): Cache loaded %zu objects.\n", CacheObjects.size());
#endif

    HasLoaded = true;
    CacheLoading = false;
    CacheLoaded.notify_all();
}

CoDFileSystem* CoDPackageCache::GetFileSystem()
{
    return FileSystem.get();
}

void CoDPackageCache::LoadPackageCacheAsync(const std::string& BasePath)
{
    // Flag the load here rather than on the worker. Setting it once the thread is
    // running leaves a window where WaitForPackageCacheLoad sees an idle cache and
    // returns immediately, so the export runs against an index that has not been built
    // yet, extracts nothing, and still reports every asset as exported.
    BeginLoadingState();

    std::thread LoadThread([BasePath, this]
    {
        this->LoadPackageCache(BasePath);
    });

    LoadThread.detach();
}

void CoDPackageCache::LoadPackageAsync(const std::string& FilePath)
{
    // Same race as above, flag before the thread exists
    BeginLoadingState();

    std::thread LoadThread([FilePath, this]
    {
        this->LoadPackage(FilePath);
        // Set the loaded state, this is only called on single entries
        this->SetLoadedState();
    });

    LoadThread.detach();
}

void CoDPackageCache::BeginLoadingState()
{
    std::lock_guard<std::mutex> Lock(CacheMutex);

    // Mark as loading before any waiter can observe us
    CacheLoading = true;
    HasLoaded = false;
}

void CoDPackageCache::WaitForPackageCacheLoad()
{
    std::unique_lock<std::mutex> Lock(CacheMutex);
    CacheLoaded.wait(Lock, [this] { return !CacheLoading; });
}

bool CoDPackageCache::Find(uint64_t CacheID, PackageCacheObject& Out) const
{
    if (KeptRows != nullptr)
    {
        const KeptPackageRow* End = KeptRows + KeptCount;
        const KeptPackageRow* It = std::lower_bound(KeptRows, End, CacheID, [](const KeptPackageRow& R, uint64_t K) { return R.Key < K; });
        if (It != End && It->Key == CacheID)
        {
            Out = It->Object;
            return true;
        }
    }
    const auto It = CacheObjects.find(CacheID);
    if (It == CacheObjects.end()) return false;
    Out = It->second;
    return true;
}

namespace
{
    // bumped when what a kept table file holds changes
    constexpr char KeptMagic[8] = { 'G', 'W', 'P', 'K', 'G', '2', '\n', 0 };

    template <class T>
    bool Take(const uint8_t*& At, const uint8_t* End, T& Out)
    {
        if ((size_t)(End - At) < sizeof(T)) return false;
        std::memcpy(&Out, At, sizeof(T));
        At += sizeof(T);
        return true;
    }
}

std::string CoDPackageCache::KeptFile(const std::string& BasePath, const char* Extension)
{
    const std::string Where = ExportNaming::Lower(std::filesystem::absolute(BasePath).string());
    return (std::filesystem::temp_directory_path() / "greyhound_packages" / (ExportNaming::Hex16(ExportNaming::Fnv1a(Where)) + Extension)).string();
}

std::string CoDPackageCache::KeptKey(const char* Pattern, const char* Format) const
{
    using ExportNaming::Fnv1a;
    uint64_t H = Fnv1a(std::string(KeptMagic, sizeof(KeptMagic)) + Format);
    if (auto* Casc = dynamic_cast<CascFileSystem*>(FileSystem.get()))
        Casc->EnumerateFilesWithKey(Pattern, [&](const std::string& Name, const size_t Size, const std::string& CKey) {
            H = Fnv1a(Name + '\0' + std::to_string(Size) + '\0' + CKey + '\n', H);
        });
    else if (FileSystem != nullptr)
        FileSystem->EnumerateFiles(Pattern, [&](const std::string& Name, const size_t Size) {
            std::error_code Ec;
            const auto Time = std::filesystem::last_write_time(Name, Ec).time_since_epoch().count();
            H = Fnv1a(Name + '\0' + std::to_string(Size) + '\0' + std::to_string(Time) + '\n', H);
        });
    return ExportNaming::Hex16(H);
}

bool CoDPackageCache::LoadKept(const std::string& File, const std::string& Key, std::vector<uint16_t>* Extra)
{
    MappedFile Map;
    if (!Map.Open(File)) return false;
    // magic, key, the packages' paths, the u16s, the rows (8-aligned)
    const uint8_t* const Begin = Map.data();
    const uint8_t* At = Begin;
    const uint8_t* End = Begin + Map.size();
    std::vector<std::string> Paths;
    std::vector<uint16_t> Shorts;
    uint64_t Count = 0;
    const bool Ok = [&] {
        char Magic[sizeof(KeptMagic)];
        uint32_t KeyLength = 0, PathCount = 0, ShortCount = 0;
        if (!Take(At, End, Magic) || std::memcmp(Magic, KeptMagic, sizeof(Magic)) != 0 || !Take(At, End, KeyLength) ||
            (size_t)(End - At) < KeyLength || std::string((const char*)At, KeyLength) != Key)
            return false;
        At += KeyLength;
        if (!Take(At, End, PathCount) || PathCount > (End - At) / 4) return false;
        Paths.resize(PathCount);
        for (auto& P : Paths)
        {
            uint32_t Length = 0;
            if (!Take(At, End, Length) || (size_t)(End - At) < Length) return false;
            P.assign((const char*)At, Length);
            At += Length;
        }
        if (!Take(At, End, ShortCount) || ShortCount > (End - At) / 2) return false;
        Shorts.resize(ShortCount);
        for (auto& S : Shorts)
            if (!Take(At, End, S)) return false;
        At = Begin + (((At - Begin) + 7) & ~(ptrdiff_t)7);
        if (At > End || !Take(At, End, Count)) return false;
        return Count <= (uint64_t)(End - At) / sizeof(KeptPackageRow);
    }();
    if (!Ok || (Extra != nullptr && Shorts.size() != Paths.size())) return false;
    Kept = std::move(Map);
    KeptRows = reinterpret_cast<const KeptPackageRow*>(At);
    KeptCount = (size_t)Count;
    PackageFilePaths = std::move(Paths);
    if (Extra != nullptr) *Extra = std::move(Shorts);
    return true;
}

void CoDPackageCache::SaveKept(const std::string& File, const std::string& Key, const std::vector<uint16_t>* Extra) const
{
    std::vector<KeptPackageRow> Rows;
    Rows.reserve(CacheObjects.size());
    for (const auto& [K, Object] : CacheObjects) Rows.push_back({ K, Object });
    std::sort(Rows.begin(), Rows.end(), [](const KeptPackageRow& A, const KeptPackageRow& B) { return A.Key < B.Key; });
    // another process may read the same table meanwhile
    FileSystems::WriteReplacing(File, [&](std::ostream& Out) {
        const auto Put = [&](const void* P, size_t N) { Out.write(static_cast<const char*>(P), (std::streamsize)N); };
        Put(KeptMagic, sizeof(KeptMagic));
        const uint32_t KeyLength = (uint32_t)Key.size();
        Put(&KeyLength, 4);
        Put(Key.data(), Key.size());
        const uint32_t PathCount = (uint32_t)PackageFilePaths.size();
        Put(&PathCount, 4);
        for (const auto& P : PackageFilePaths)
        {
            const uint32_t Length = (uint32_t)P.size();
            Put(&Length, 4);
            Put(P.data(), P.size());
        }
        const uint32_t ShortCount = Extra != nullptr ? (uint32_t)Extra->size() : 0;
        Put(&ShortCount, 4);
        if (ShortCount != 0) Put(Extra->data(), ShortCount * sizeof(uint16_t));
        const uint64_t Padding = 0;
        Put(&Padding, (size_t)((8 - (uint64_t)Out.tellp() % 8) % 8));
        const uint64_t Count = Rows.size();
        Put(&Count, 8);
        Put(Rows.data(), Rows.size() * sizeof(KeptPackageRow));
    });
}