#pragma once
#include "CoDAssets.h"
#include <functional>
#include <shared_mutex>

class XSUBCache : public CoDPackageCache
{
private:
    std::shared_mutex ReadMutex;
    // Each package's KAPI version (16 Cold War; 21 Vanguard; 23 Modern Warfare III, Black Ops 6 and 7)
    std::vector<uint16_t> PackageVersions;
public:
    XSUBCache();
    virtual ~XSUBCache();

    virtual void LoadPackageCache(const std::string& BasePath);
    virtual bool LoadPackage(const std::string& FilePath);
    virtual std::unique_ptr<uint8_t[]> ExtractPackageObjectRaw(uint64_t CacheID, uint32_t& ResultSize);
    virtual std::unique_ptr<uint8_t[]> ExtractPackageObject(uint64_t CacheID, uint32_t& ResultSize);
    // the sized form the IW live readers call (Modern Warfare III's): an object's size is its own here
    std::unique_ptr<uint8_t[]> ExtractPackageObject(uint64_t CacheID, int32_t Size, uint32_t& ResultSize) override
    {
        return ExtractPackageObject(CacheID, ResultSize);
    }

    // A framed object's size, from its frames' block records (read through ReadAt(offset in the
    // object's Stored bytes, destination, count), true when read): what a version-21 package
    // (Vanguard) does not index, its entries holding only the stored size. False when the frames
    // do not parse.
    static bool FramedObjectSize(const std::function<bool(uint64_t, uint8_t*, size_t)>& ReadAt, uint64_t Stored,
                                 uint64_t Key, uint64_t& Size);
};
