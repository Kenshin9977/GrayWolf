#pragma once

#include <string>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "CoDPackageCache.h"

class CoDFileHandle;

class XPAKCache : public CoDPackageCache
{
public:
    XPAKCache();
    virtual ~XPAKCache();

    virtual void LoadPackageCache(const std::string& BasePath);
    virtual bool LoadPackage(const std::string& FilePath);
    // Loads every *.xpak directly inside Dir (not its subfolders), synchronously: the packages
    // beside one zone, for the offline exporters.
    void LoadPackagesIn(const std::string& Dir);
    virtual std::unique_ptr<uint8_t[]> ExtractPackageObject(uint64_t CacheID, int32_t Size, uint32_t& ResultSize);

    // An object the packages' index names ("name: ...", with its type and the size of its first
    // part): a map's baked images (sky, probes) have no key in the zone, only their name.
    struct NamedObject
    {
        uint64_t Key = 0;
        uint64_t Size = 0;   // size0
        uint32_t Parts = 0;  // how many offsetN/sizeN pairs
        std::string Type;
    };
    // Searches the index of the loaded packages whose file stem is Stem or starts with Stem + "_"
    // (each read once, on first use); false when none names it.
    bool FindNamed(const std::string& Name, const std::string& Stem, NamedObject& Out);

    static std::unique_ptr<uint8_t[]> DecompressPackageObject(uint64_t cacheID, uint8_t* buffer, size_t bufferSize, size_t decompressedSize, size_t& resultSize,
                                                                bool paddedBlocks = false);

private:
    // One open handle per .xpak, reused across calls: opening a multi-GB file per streamed image costs
    // ~15 ms each, the whole cost on a zone with thousands of them.
    //
    // A shared HANDLE has one file pointer, so concurrent export threads hitting the SAME package
    // must not seek+read it at once - but a BO4 install holds 76 separate .xpak files, so locking
    // that per FILE rather than once globally lets threads reading different packages actually
    // run in parallel. MapLock only protects the two maps themselves (brief); PackageLocks[i] is
    // held for the seek+read against package i.
    std::mutex MapLock;
    std::unordered_map<uint32_t, std::unique_ptr<CoDFileHandle>> OpenPackageHandles;
    std::unordered_map<uint32_t, std::unique_ptr<std::mutex>> PackageLocks;
    // Whether a package aligns its data blocks to 4 bytes (its version, read when its handle opens)
    std::unordered_map<uint32_t, bool> Padded;

    // FindNamed's index, per package read so far (lower-case names)
    std::mutex NamedLock;
    std::unordered_map<uint32_t, std::unordered_map<std::string, NamedObject>> Named;
};