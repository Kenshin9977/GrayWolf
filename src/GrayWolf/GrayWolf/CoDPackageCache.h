#pragma once

#include "MappedFile.h"

#include <string>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include <unordered_map>

class CoDFileSystem;

// A structure that represents a package cache object, this is generic and is used in all of the assets
struct PackageCacheObject
{
    uint64_t Offset;
    uint64_t CompressedSize;
    uint64_t UncompressedSize;

    uint32_t PackageFileIndex;
    // A format's own bits (an .xsub of version 23: 1 when the object is framed in blocks, 0 stored)
    uint32_t Flags = 0;
};

// A row of a kept table (CoDPackageCache::LoadKept): an object and its key
#pragma pack(push, 1)
struct KeptPackageRow
{
    uint64_t Key;
    PackageCacheObject Object;
};
#pragma pack(pop)

// A class that handles reading and indexing game package files for later extraction of data
class CoDPackageCache
{
public:
    CoDPackageCache();
    virtual ~CoDPackageCache();

    bool IsCacheLoading();

    void LoadPackageCacheAsync(const std::string& BasePath);
    void LoadPackageAsync(const std::string& FilePath);

    // Waits for the package cache to load, blocking the current thread
    void WaitForPackageCacheLoad();
    // An object's place: the kept table's (LoadKept) or CacheObjects'
    bool Find(uint64_t CacheID, PackageCacheObject& Out) const;
    virtual bool Exists(uint64_t CacheID)
    {
        PackageCacheObject O;
        return Find(CacheID, O);
    }
    // Where an object lies (package file, offset in it): lets a caller issue its reads in disk order.
    bool Locate(uint64_t CacheID, uint32_t& Package, uint64_t& Offset) const
    {
        PackageCacheObject O;
        if (!Find(CacheID, O)) return false;
        Package = O.PackageFileIndex;
        Offset = O.Offset;
        return true;
    }
    // Returns the literal cache object with no /decompression applied (nullptr if not found)
    virtual std::unique_ptr<uint8_t[]> ExtractPackageObjectRaw(uint64_t CacheID, uint32_t& ResultSize) { return nullptr; }
    // Returns a cache object (nullptr if not found)
    virtual std::unique_ptr<uint8_t[]> ExtractPackageObject(uint64_t CacheID, uint32_t& ResultSize) { return ExtractPackageObject(CacheID, -1, ResultSize); }
    // Returns a cache object (nullptr if not found)
    virtual std::unique_ptr<uint8_t[]> ExtractPackageObject(uint64_t CacheID, int32_t Size, uint32_t& ResultSize) { return nullptr; }
    // Returns a cache object (nullptr if not found)
    virtual std::unique_ptr<uint8_t[]> ExtractPackageObject(const std::string& PackageName, uint64_t AssetOffset, uint64_t AssetSize, size_t& ResultSize) { return nullptr; }

    // ExtractPackageObject in two halves: the read now, on the calling thread (a caller reading in
    // disk order), and what the returned function does, any thread, once (decompress: what a
    // package does without its file). By default both are done now and the function hands the result.
    using PackageObjectFinish = std::function<std::unique_ptr<uint8_t[]>(uint32_t& ResultSize)>;
    virtual PackageObjectFinish ReadPackageObject(uint64_t CacheID, int32_t Size)
    {
        uint32_t ReadSize = 0;
        auto Data = std::make_shared<std::unique_ptr<uint8_t[]>>(ExtractPackageObject(CacheID, Size, ReadSize));
        return [Data, ReadSize](uint32_t& ResultSize) {
            ResultSize = *Data ? ReadSize : 0;
            return std::move(*Data);
        };
    }

    // Hashes a cache object id if needed
    virtual uint64_t HashPackageID(const std::string& Value) { return 0; }

    std::string GetPackagesPath() { return PackageFilesPath; }

    virtual void SetLoadedState();

    virtual CoDFileSystem* GetFileSystem();

protected:

    // Marks the cache as loading, called before the loading thread is spawned so that a
    // waiter can never observe the cache as idle in between
    void BeginLoadingState();

    // Loads the package cache (This should be called from LoadPackageCacheAsync only!)
    virtual void LoadPackageCache(const std::string& BasePath);
    virtual bool LoadPackage(const std::string& FilePath);    

    std::unordered_map<uint64_t, PackageCacheObject> CacheObjects;
    std::vector<std::string> PackageFilePaths;

    // A path of packages, used for games that don't require a cache
    std::string PackageFilesPath;
    
    std::unique_ptr<CoDFileSystem> FileSystem;

    std::string SubDirectory;

    // The packages' tables kept on disk (%TEMP%\greyhound_packages\<install><Extension>) under Key,
    // which names the packages' contents (KeptKey): their paths, a u16 per package (Extra: an
    // .xsub's version) and the objects sorted by key, read in place (Find). Millions of objects are
    // then neither read from the packages nor hashed again per run. false when the file is missing,
    // of another format or under another key (a game update).
    bool LoadKept(const std::string& File, const std::string& Key, std::vector<uint16_t>* Extra = nullptr);
    void SaveKept(const std::string& File, const std::string& Key, const std::vector<uint16_t>* Extra = nullptr) const;
    static std::string KeptFile(const std::string& BasePath, const char* Extension);
    // The packages matching Pattern: their names and sizes with their content keys (CASC) or
    // their write times (a folder)
    std::string KeptKey(const char* Pattern, const char* Format) const;

private:

    std::mutex CacheMutex;
    std::condition_variable CacheLoaded;

    bool HasLoaded;
    bool CacheLoading;

    // the kept table read in place (LoadKept): its view and its rows, sorted by key
    MappedFile Kept;
    const KeptPackageRow* KeptRows = nullptr;
    size_t KeptCount = 0;
};