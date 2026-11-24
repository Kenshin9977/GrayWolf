#pragma once
#include "CoDAssets.h"
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>

class XSUBCache : public CoDPackageCache
{
private:
    std::shared_mutex ReadMutex;
    // Each package's KAPI version (16 Cold War; 21 Vanguard; 23 Modern Warfare III, Black Ops 6 and 7)
    std::vector<uint16_t> PackageVersions;

    // The game's on-demand cache (<install>\xpak_cache): the packages from OnDemandFrom on are its
    // cache_<n>_<v>.xpak indexes, their objects in the .xpakdata beside each, read through Loose
    // (a folder, whatever FileSystem is). Never kept (LoadKept): the game adds to it as it plays.
    size_t OnDemandFrom = SIZE_MAX;
    std::unique_ptr<CoDFileSystem> Loose;
    void LoadOnDemandCache(const std::string& BasePath);
    CoDFileSystem* FilesOf(uint32_t Package) const { return Package >= OnDemandFrom ? Loose.get() : FileSystem.get(); }

    // The objects only the game's CDN holds (listed by the *_cdn.xpak indexes, Modern Warfare 4's
    // *_cdn_stub.xpak, offset 0), fetched
    // when cdn_images allows it (CdnObjects: the high mips of most images, the files
    // holding the low ones). Indexed on first use.
    struct RemoteObject
    {
        uint64_t Key;
        uint32_t Stored;
        uint32_t Size;
        uint16_t Version;
        uint8_t Framed;
        uint8_t Base; // RemoteBases' index
    };
    std::once_flag RemoteIndexed;
    std::vector<RemoteObject> Remote; // sorted by key
    std::vector<std::string> RemoteBases; // the *_cdn.xpak folders
    std::vector<std::string> RemoteUrls;  // their CDN's URL, found at the first download
    std::mutex RemoteBaseLock;
    std::string RemoteBase(uint8_t Index);
    void IndexRemote();
    const RemoteObject* FindRemote(uint64_t Key);
    std::unique_ptr<uint8_t[]> FetchRemote(const RemoteObject& Object, uint32_t& ResultSize);
    // its stored bytes: GrayWolf's kept copy, else downloaded (CdnObjects::Fetch)
    std::optional<std::vector<uint8_t>> StoredRemote(const RemoteObject& Object);
    bool KeptRemote(const RemoteObject& Object);

public:
    XSUBCache();
    virtual ~XSUBCache();

    virtual void LoadPackageCache(const std::string& BasePath);
    virtual bool LoadPackage(const std::string& FilePath);
    // in the install's packages, the game's xpak_cache, GrayWolf's kept CDN copies, or on the
    // game's CDN when cdn_images allows it (looked up in that order; the game's files only read)
    bool Exists(uint64_t CacheID) override;
    // the CDN objects among Keys, downloaded together (two at a time per host) before the reads
    void Prefetch(const std::vector<uint64_t>& Keys, const std::function<bool(size_t, size_t)>& Progress) override;
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
