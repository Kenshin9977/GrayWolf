#include "stdafx.h"
#include "ByteView.h"
#include "XSUBCache.h"

#include "Compression.h"
#include "MemoryReader.h"
#include "Siren.h"

#include "CoDFileHandle.h"
#include "CdnObjects.h"
#include "ExportNaming.h"
#include "WinFileSystem.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>
#include <unordered_set>

#pragma pack(push, 1)
struct BOCWXSubHeader
{
    uint32_t Magic;
    uint16_t Unknown1;
    uint16_t Version;
    uint64_t Unknown;
    uint64_t Type;
    uint64_t Size;
    uint8_t UnknownHashes[1896];
    int64_t FileCount;
    int64_t DataOffset;
    int64_t DataSize;
    int64_t HashCount;
    int64_t HashOffset;
    int64_t HashSize;
    int64_t Unknown3;
    int64_t UnknownOffset;
    int64_t Unknown4;
    int64_t IndexCount;
    int64_t IndexOffset;
    int64_t IndexSize;
};
#pragma pack(pop)

#pragma pack(push, 1)
// Cold War's: PackedInfo = offset >> 7 in the high 32 bits, stored size << 1 | 1 when the object is
// framed in blocks (0: stored as is, 56 937 of the install's 2 264 185 objects, sounds among them)
struct BOCWXSubHashEntry
{
    uint64_t Key;
    uint64_t PackedInfo;
};
// Version 23 (Modern Warfare III, Black Ops 6, Black Ops 7): 20 bytes, the object's size added
struct XSub23HashEntry
{
    uint64_t Key;
    uint32_t PackedSize;   // stored size << 1 | 1 when the object is framed in blocks (0: stored as is)
    uint32_t Offset;       // >> 7
    uint32_t Size;         // the object's bytes, decompressed
};
// A version-23 object (flag 1) is one or more frames, each 128-aligned: u16 0xF01D, the u64 key, 12
// bytes, a u8 block count at 0x16, then that many block records of 21 bytes from 0x17 (Black Ops 7's
// reader 0xac470a0 of cod_dump: magic, key at 2, count at 0x16, records `imul 0x15`)
struct XSub23Block
{
    uint8_t Compression;   // 0 stored, 3 LZ4, 6 Oodle
    uint32_t CompressedSize;
    uint32_t Size;
    uint32_t Offset;       // from the frame's start
    uint32_t ObjectOffset; // where its bytes go in the object
    uint32_t Check;
};
static_assert(sizeof(XSub23Block) == 21, "a block record is 21 bytes");
#pragma pack(pop)

namespace
{
    constexpr uint16_t FrameMagic = 0xF01D;
    // Version 21 (Vanguard): Cold War's 16-byte entries, but objects framed as version 23's (the
    // frames, block records and compressions below, checked on 200 objects of core-00000.xsub);
    // the object's size is not indexed, its frames give it (XSUBCache::FramedObjectSize)
    constexpr uint16_t FirstFramedVersion = 21;
    constexpr uint16_t FirstSizedVersion = 23;
    // The games' own Oodle (Cold War, Modern Warfare III, Black Ops 6 and 7 ship oo2core_8)
    constexpr std::string_view OodleDlls[] = { "oo2core_8_win64.dll" };

    // A version-23 object's stored bytes (Stored) to its Size bytes; false on any inconsistency
    bool DecodeXSub23(ByteView Stored, uint64_t Key, std::vector<uint8_t>& Object)
    {
        if (Stored.size() < 0x17 || *(const uint16_t*)Stored.data() != FrameMagic || *(const uint64_t*)(Stored.data() + 2) != Key)
            return false;
        uint64_t Filled = 0;
        for (size_t Frame = 0; Frame + 0x17 <= Stored.size();)
        {
            if (*(const uint16_t*)(Stored.data() + Frame) != FrameMagic)
                return false;
            const uint8_t Count = Stored[Frame + 0x16];
            if (Frame + 0x17 + Count * sizeof(XSub23Block) > Stored.size())
                return false;
            size_t End = Frame + 0x17 + Count * sizeof(XSub23Block);
            for (uint8_t i = 0; i < Count; i++)
            {
                XSub23Block B;
                std::memcpy(&B, Stored.data() + Frame + 0x17 + i * sizeof(XSub23Block), sizeof(B));
                const uint64_t From = Frame + (uint64_t)B.Offset;
                if (From + B.CompressedSize > Stored.size() || (uint64_t)B.ObjectOffset + B.Size > Object.size())
                    return false;
                const uint8_t* Src = Stored.data() + From;
                uint8_t* Dst = Object.data() + B.ObjectOffset;
                bool Done = false;
                if (B.Compression == 0)
                    Done = B.CompressedSize >= B.Size && (std::memcpy(Dst, Src, B.Size), true);
                else if (B.Compression == 3)
                    Done = Compression::DecompressLZ4Block((const int8_t*)Src, (int8_t*)Dst, B.CompressedSize, B.Size) == (int32_t)B.Size;
                else if (B.Compression == 6)
                    Done = Siren::Decompress(OodleDlls, Src, B.CompressedSize, Dst, B.Size) == B.Size;
                if (!Done)
                    return false;
                Filled += B.Size;
                End = std::max<size_t>(End, (size_t)(From + B.CompressedSize));
            }
            Frame = (End + 0x7F) & ~(size_t)0x7F;
        }
        return Filled == Object.size();
    }
}

bool XSUBCache::FramedObjectSize(const std::function<bool(uint64_t, uint8_t*, size_t)>& ReadAt, uint64_t Stored,
                                 uint64_t Key, uint64_t& Size)
{
    Size = 0;
    uint64_t Filled = 0;
    std::vector<uint8_t> Records;
    for (uint64_t Frame = 0; Frame + 0x17 <= Stored;)
    {
        uint8_t Head[0x17];
        if (!ReadAt(Frame, Head, sizeof(Head)))
            return false;
        uint16_t Magic = 0;
        uint64_t FrameKey = 0;
        std::memcpy(&Magic, Head, 2);
        std::memcpy(&FrameKey, Head + 2, 8);
        if (Magic != FrameMagic || FrameKey != Key)
            return false;
        const uint8_t Count = Head[0x16];
        if (Frame + 0x17 + Count * sizeof(XSub23Block) > Stored)
            return false;
        Records.resize(Count * sizeof(XSub23Block));
        if (Count != 0 && !ReadAt(Frame + 0x17, Records.data(), Records.size()))
            return false;
        uint64_t End = Frame + 0x17 + Records.size();
        for (uint8_t i = 0; i < Count; i++)
        {
            XSub23Block B;
            std::memcpy(&B, Records.data() + i * sizeof(XSub23Block), sizeof(B));
            if (Frame + B.Offset + B.CompressedSize > Stored)
                return false;
            Size = std::max<uint64_t>(Size, (uint64_t)B.ObjectOffset + B.Size);
            Filled += B.Size;
            End = std::max<uint64_t>(End, Frame + B.Offset + B.CompressedSize);
        }
        Frame = (End + 0x7F) & ~(uint64_t)0x7F;
    }
    // the blocks tile the object: no gap, no overlap
    return Filled == Size;
}

XSUBCache::XSUBCache() = default;

XSUBCache::~XSUBCache() = default;

void XSUBCache::LoadPackageCache(const std::string& BasePath)
{
    CoDPackageCache::LoadPackageCache(BasePath);

    // The base leaves FileSystem unset when the path will not open (e.g. a Battle.net install whose
    // CASC container is unavailable).
    if (FileSystem == nullptr)
    {
        this->SetLoadedState();
        return;
    }

    // the tables kept (CoDPackageCache::LoadKept): Black Ops 6's 3 M objects are neither read from
    // its 154 packages nor hashed again per run (2.5 s)
    const std::string Kept = KeptFile(BasePath, ".xsk"), Key = KeptKey("*.xsub", "xsub");
    if (!LoadKept(Kept, Key, &PackageVersions))
    {
        FileSystem->EnumerateFiles("*.xsub", [this](const std::string& name, const size_t size)
        {
            try
            {
                this->LoadPackage(name);
            }
            catch (...)
            {

            }
        });
        SaveKept(Kept, Key, &PackageVersions);
    }
    LoadOnDemandCache(BasePath);

    this->SetLoadedState();
}

void XSUBCache::LoadOnDemandCache(const std::string& BasePath)
{
    // the game's (Modern Warfare III, Black Ops 6 and 7: "xpak_cache" beside the exe, or above its
    // folder): cache_<n>_2.xpak, a version-23 index (header type 1) whose objects lie in the
    // .xpakdata of the same name, framed as the .xsub's (8 007 of 8 007 checked, Call of Duty HQ)
    if (CdnObjects::Offline()) return;
    std::error_code Ec;
    std::filesystem::path Base(BasePath);
    if (!Base.has_filename()) Base = Base.parent_path();
    std::filesystem::path Dir = Base / "xpak_cache";
    // (an install named by its game's folder, HQ's cod25: the exes' "..\xpak_cache")
    if (!std::filesystem::is_directory(Dir, Ec)) Dir = Base.parent_path() / "xpak_cache";
    if (!std::filesystem::is_directory(Dir, Ec)) return;
    std::vector<std::filesystem::path> Indexes;
    for (const auto& E : std::filesystem::directory_iterator(Dir, Ec))
        if (E.path().extension() == ".xpak" && std::filesystem::is_regular_file(E.path().parent_path() / (E.path().stem().string() + ".xpakdata"), Ec))
            Indexes.push_back(E.path());
    if (Indexes.empty()) return;
    std::sort(Indexes.begin(), Indexes.end());
    Loose = std::make_unique<WinFileSystem>(Dir.string());
    OnDemandFrom = PackageFilePaths.size();
    for (const auto& Index : Indexes)
    {
        FILE* F = nullptr;
        if (_wfopen_s(&F, Index.wstring().c_str(), L"rb") != 0 || F == nullptr) continue;
        BOCWXSubHeader Header{};
        std::vector<XSub23HashEntry> Table;
        const bool Read = std::fread(&Header, sizeof(Header), 1, F) == 1 && Header.Magic == 0x4950414b && Header.Version >= FirstSizedVersion
            && Header.HashCount > 0 && Header.HashCount < (1 << 24) && _fseeki64(F, Header.HashOffset, SEEK_SET) == 0
            && (Table.resize((size_t)Header.HashCount), std::fread(Table.data(), sizeof(XSub23HashEntry), Table.size(), F) == Table.size());
        std::fclose(F);
        if (!Read) continue;
        const auto Package = (uint32_t)PackageFilePaths.size();
        for (const auto& Entry : Table)
        {
            // an object the installed packages hold is read from them
            PackageCacheObject Held;
            if (Entry.Size == 0 || Find(Entry.Key, Held)) continue;
            PackageCacheObject NewObject;
            NewObject.Offset = (uint64_t)Entry.Offset << 7;
            NewObject.CompressedSize = Entry.PackedSize >> 1;
            NewObject.UncompressedSize = Entry.Size;
            NewObject.PackageFileIndex = Package;
            NewObject.Flags = Entry.PackedSize & 1;
            CacheObjects.insert(std::make_pair(Entry.Key, NewObject));
        }
        PackageFilePaths.push_back((Index.parent_path() / (Index.stem().string() + ".xpakdata")).string());
        PackageVersions.push_back(Header.Version);
    }
}

namespace
{
    // the CDN a *_cdn.xpak's objects come from: the URL the game's exe holds in clear (its folder's
    // <folder>-cod.exe, or HQ's cod.exe above it), else by the folder, as the shipped exes say
    // (cod23-cod.exe and sp24-cod.exe "pc/iw9_2"; cod24-cod.exe and HQ's cod.exe "pc/iw9_3")
    std::string CdnBaseOf(const std::filesystem::path& Folder)
    {
        const std::string Name = Folder.filename().string();
        for (const auto& Exe : { Folder / (Name + "-cod.exe"), Folder.parent_path() / (Name + "-cod.exe"), Folder.parent_path() / "cod.exe", Folder / "cod.exe" })
            if (std::string Url = CdnObjects::BaseInExe(Exe); !Url.empty()) return Url;
        const std::string Lower = ExportNaming::Lower(Name);
        return std::string("http://cod-assets.cdn.callofduty.com/pc/") + (Lower == "cod23" || Lower == "sp24" ? "iw9_2" : "iw9_3");
    }
}

std::string XSUBCache::RemoteBase(uint8_t Index)
{
    std::lock_guard<std::mutex> Lock(RemoteBaseLock);
    if (RemoteUrls.size() != RemoteBases.size()) RemoteUrls.resize(RemoteBases.size());
    if (RemoteUrls[Index].empty()) RemoteUrls[Index] = CdnBaseOf(RemoteBases[Index]);
    return RemoteUrls[Index];
}

void XSUBCache::IndexRemote()
{
    if (FileSystem == nullptr || dynamic_cast<WinFileSystem*>(FileSystem.get()) == nullptr) return;
    std::vector<std::string> Indexes;
    // (Modern Warfare 4 names them *_cdn_stub.xpak: cod26's 13, sp26's 3, the same version-23
    // index, every entry offset 0)
    for (const char* Pattern : { "*_cdn.xpak", "*_cdn_stub.xpak" })
        FileSystem->EnumerateFiles(Pattern, [&](const std::string& Name, const size_t) { Indexes.push_back(Name); });
    std::sort(Indexes.begin(), Indexes.end());
    Indexes.erase(std::unique(Indexes.begin(), Indexes.end()), Indexes.end());
    for (const auto& Name : Indexes)
    {
        auto Reader = CoDFileHandle(FileSystem->OpenFile(Name, "r"), FileSystem.get());
        if (Reader.Size() < sizeof(BOCWXSubHeader)) continue;
        const auto Header = Reader.Read<BOCWXSubHeader>();
        if (Header.Magic != 0x4950414b || Header.Version < FirstSizedVersion || Header.HashCount <= 0 || Header.HashCount >= (1 << 24)
            || Header.HashOffset + Header.HashCount * (int64_t)sizeof(XSub23HashEntry) > (int64_t)Reader.Size())
            continue;
        // (the folder: its CDN's URL is looked up at the first download, RemoteBase)
        const std::string Base = std::filesystem::path(Name).parent_path().string();
        auto At = std::find(RemoteBases.begin(), RemoteBases.end(), Base);
        if (At == RemoteBases.end()) At = RemoteBases.insert(RemoteBases.end(), Base);
        const auto BaseIndex = (uint8_t)(At - RemoteBases.begin());
        std::vector<XSub23HashEntry> Table((size_t)Header.HashCount);
        Reader.Seek(Header.HashOffset, SEEK_SET);
        if (Reader.Read(reinterpret_cast<uint8_t*>(Table.data()), 0, Table.size() * sizeof(XSub23HashEntry)) != Table.size() * sizeof(XSub23HashEntry))
            continue;
        // a CDN object: no offset (nothing in the file), its stored and decoded sizes
        for (const auto& Entry : Table)
            if (Entry.Offset == 0 && (Entry.PackedSize >> 1) != 0 && Entry.Size != 0)
                Remote.push_back({ Entry.Key, Entry.PackedSize >> 1, Entry.Size, Header.Version, (uint8_t)(Entry.PackedSize & 1), BaseIndex });
    }
    std::sort(Remote.begin(), Remote.end(), [](const RemoteObject& A, const RemoteObject& B) { return A.Key < B.Key; });
}

const XSUBCache::RemoteObject* XSUBCache::FindRemote(uint64_t Key)
{
    // (off: still indexed, a copy fetched earlier is read)
    if (CdnObjects::Offline()) return nullptr;
    std::call_once(RemoteIndexed, [this] { IndexRemote(); });
    const auto It = std::lower_bound(Remote.begin(), Remote.end(), Key, [](const RemoteObject& R, uint64_t K) { return R.Key < K; });
    return It != Remote.end() && It->Key == Key ? &*It : nullptr;
}

bool XSUBCache::KeptRemote(const RemoteObject& Object)
{
    return CdnObjects::IsKept(RemoteBase(Object.Base), Object.Key);
}

std::optional<std::vector<uint8_t>> XSUBCache::StoredRemote(const RemoteObject& Object)
{
    constexpr uint32_t MaxObject = 1u << 30;
    if (Object.Stored > MaxObject || Object.Size > MaxObject) return std::nullopt;
    // what a framed object's first frame starts with: the magic and its key
    const auto Whole = [&](const std::vector<uint8_t>& Stored) {
        return Stored.size() == Object.Stored
            && (!Object.Framed || (Stored.size() >= 10 && *(const uint16_t*)Stored.data() == FrameMagic && *(const uint64_t*)(Stored.data() + 2) == Object.Key));
    };
    // the game's request: <base>/<version>/<key's low byte>/<key>_<stored size>_<framed>
    char Name[64];
    std::snprintf(Name, sizeof(Name), "/%u/%02x/%016llx_%08x_%u", Object.Version, (unsigned)(uint8_t)Object.Key,
                  (unsigned long long)Object.Key, Object.Stored, (unsigned)Object.Framed);
    return CdnObjects::Fetch(RemoteBase(Object.Base), Name, Object.Key, Whole);
}

void XSUBCache::Prefetch(const std::vector<uint64_t>& Keys, const std::function<bool(size_t, size_t)>& Progress)
{
    std::vector<const RemoteObject*> Todo;
    std::unordered_set<uint64_t> Seen;
    for (const uint64_t Key : Keys)
    {
        PackageCacheObject O;
        if (!Seen.insert(Key).second || Find(Key, O)) continue;
        const RemoteObject* R = FindRemote(Key);
        if (R != nullptr && !KeptRemote(*R)) Todo.push_back(R);
    }
    if (Todo.empty()) return;
    // asked once (the GUI), else counted as skipped by the reads
    if (!CdnObjects::Allowed()) return;
    std::atomic<size_t> Next = 0, Done = 0;
    std::atomic<bool> Stop = false;
    const auto Work = [&] {
        for (size_t I; !Stop && (I = Next++) < Todo.size();)
        {
            StoredRemote(*Todo[I]);
            Done++;
        }
    };
    // CdnObjects lets two requests per host run at once
    std::vector<std::thread> Workers;
    for (int W = 0; W < 2; W++) Workers.emplace_back(Work);
    while (Done < Todo.size() && !Stop)
    {
        if (Progress && !Progress(Done, Todo.size())) Stop = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    for (auto& W : Workers) W.join();
    if (Progress) Progress(Todo.size(), Todo.size());
}

std::unique_ptr<uint8_t[]> XSUBCache::FetchRemote(const RemoteObject& Object, uint32_t& ResultSize)
{
    ResultSize = 0;
    const auto Stored = StoredRemote(Object);
    if (!Stored) return nullptr;
    auto Result = std::make_unique<uint8_t[]>(std::max<size_t>(Object.Size, 1));
    if (!Object.Framed)
    {
        if (Stored->size() < Object.Size) return nullptr;
        std::memcpy(Result.get(), Stored->data(), Object.Size);
    }
    else
    {
        std::vector<uint8_t> Decoded(Object.Size);
        if (!DecodeXSub23(*Stored, Object.Key, Decoded)) return nullptr;
        std::memcpy(Result.get(), Decoded.data(), Decoded.size());
    }
    ResultSize = Object.Size;
    return Result;
}

bool XSUBCache::Exists(uint64_t CacheID)
{
    PackageCacheObject O;
    if (Find(CacheID, O)) return true;
    const RemoteObject* R = FindRemote(CacheID);
    if (R == nullptr) return false;
    if (KeptRemote(*R) || CdnObjects::Allowed()) return true;
    CdnObjects::NoteSkipped(CacheID);
    return false;
}

bool XSUBCache::LoadPackage(const std::string& FilePath)
{
#ifdef _DEBUG
    std::cout << "XSUBCache::LoadPackage(): Parsing " << FilePath << "\n";
#endif

    CoDPackageCache::LoadPackage(FilePath);

    auto PackageIndex = (uint32_t)PackageFilePaths.size();

    // TODO: Implement read, seek, etc. right in this handle class.
    auto Reader = CoDFileHandle(FileSystem->OpenFile(FilePath, "r"), FileSystem.get());

    auto Header = Reader.Read<BOCWXSubHeader>();

    // Check if we have data (Type 3 is data, others are just metadata and refs)
    if (Header.Type != 3)
        return true;


    if (Header.Magic == 0x4950414b && Header.Version >= FirstSizedVersion && Header.HashOffset < Reader.Size())
    {
        Reader.Seek(Header.HashOffset, SEEK_SET);
        std::vector<XSub23HashEntry> Table((size_t)Header.HashCount);
        Reader.Read(reinterpret_cast<uint8_t*>(Table.data()), 0, Table.size() * sizeof(XSub23HashEntry));
        for (const auto& Entry : Table)
        {
            PackageCacheObject NewObject;
            NewObject.Offset = (uint64_t)Entry.Offset << 7;
            NewObject.CompressedSize = Entry.PackedSize >> 1;
            NewObject.UncompressedSize = Entry.Size;
            NewObject.PackageFileIndex = PackageIndex;
            NewObject.Flags = Entry.PackedSize & 1;
            CacheObjects.insert(std::make_pair(Entry.Key, NewObject));
        }
        PackageFilePaths.push_back(FilePath);
        PackageVersions.push_back(Header.Version);
        return true;
    }

    if (Header.Magic == 0x4950414b && Header.HashOffset < Reader.Size())
    {
        Reader.Seek(Header.HashOffset, SEEK_SET);
        uint64_t HashResult = 0;
        auto Buffer = Reader.Read(Header.HashCount * sizeof(BOCWXSubHashEntry));
        auto HashData = MemoryReader((int8_t*)Buffer.release(), Header.HashCount * sizeof(BOCWXSubHashEntry));

        for (int64_t i = 0; i < Header.HashCount; i++)
        {
            auto Entry = HashData.Read<BOCWXSubHashEntry>();

            PackageCacheObject NewObject;
            NewObject.Offset = (Entry.PackedInfo >> 32) << 7;
            NewObject.CompressedSize = (Entry.PackedInfo >> 1) & 0x3FFFFFFF;
            NewObject.UncompressedSize = 0;
            NewObject.PackageFileIndex = PackageIndex;
            NewObject.Flags = (uint32_t)(Entry.PackedInfo & 1);

            CacheObjects.insert(std::make_pair(Entry.Key, NewObject));
        }

        PackageFilePaths.push_back(FilePath);
        PackageVersions.push_back(Header.Version);

#ifdef _DEBUG
        std::cout << "XSUBCache::LoadPackage(): Added " << FilePath << " to cache.\n";
#endif

        return true;
    }

#ifdef _DEBUG
    std::cout << "XSUBCache::LoadPackage(): Failed to parse package.\n";
#endif

    return false;
}

std::unique_ptr<uint8_t[]> XSUBCache::ExtractPackageObjectRaw(uint64_t CacheID, uint32_t & ResultSize)
{
    if (PackageCacheObject CacheInfo; Find(CacheID, CacheInfo))
    {
        std::lock_guard<std::shared_mutex> Gaurd(ReadMutex);

        auto& XPAKFileName = PackageFilePaths[CacheInfo.PackageFileIndex];
        auto Reader = CoDFileHandle(FilesOf(CacheInfo.PackageFileIndex)->OpenFile(XPAKFileName, "r"), FilesOf(CacheInfo.PackageFileIndex));
        Reader.Seek(CacheInfo.Offset, SEEK_SET);

        // Allocate just the raw size of the buffer, that's all we're returning
        auto ResultBuffer = Reader.Read(CacheInfo.CompressedSize);

        ResultSize = (uint32_t)CacheInfo.CompressedSize;
        return ResultBuffer;
    }

    ResultSize = 0;
    return nullptr;
}

std::unique_ptr<uint8_t[]> XSUBCache::ExtractPackageObject(uint64_t CacheID, uint32_t& ResultSize)
{
    if (PackageCacheObject CacheInfo; Find(CacheID, CacheInfo))
    {
        auto& XPAKFileName = PackageFilePaths[CacheInfo.PackageFileIndex];
        const uint16_t Version = PackageVersions[CacheInfo.PackageFileIndex];
        if (Version >= FirstFramedVersion)
        {
            ResultSize = 0;
            constexpr uint64_t MaxObject = 1ull << 30;
            if (CacheInfo.CompressedSize > MaxObject || CacheInfo.UncompressedSize > MaxObject)
                return nullptr;
            std::vector<uint8_t> Stored((size_t)CacheInfo.CompressedSize);
            {
                // only the read holds the lock: the decode runs beside the other threads' reads
                std::lock_guard<std::shared_mutex> Guard(ReadMutex);
                auto Reader = CoDFileHandle(FilesOf(CacheInfo.PackageFileIndex)->OpenFile(XPAKFileName, "r"), FilesOf(CacheInfo.PackageFileIndex));
                Reader.Seek(CacheInfo.Offset, SEEK_SET);
                if (Reader.Read(Stored.data(), 0, Stored.size()) != Stored.size())
                    return nullptr;
            }
            // version 21 does not index the size: an unframed object is its stored bytes, a framed
            // one as big as its blocks make it
            uint64_t Size = CacheInfo.UncompressedSize;
            if (Version < FirstSizedVersion)
            {
                if (CacheInfo.Flags == 0)
                    Size = Stored.size();
                else if (!FramedObjectSize([&](uint64_t At, uint8_t* Dst, size_t N) {
                                               if (At + N > Stored.size()) return false;
                                               std::memcpy(Dst, Stored.data() + At, N);
                                               return true;
                                           }, Stored.size(), CacheID, Size) || Size > MaxObject)
                    return nullptr;
            }
            auto Result = std::make_unique<uint8_t[]>((size_t)std::max<uint64_t>(Size, 1));
            if (CacheInfo.Flags == 0)
            {
                // stored as is
                if (Stored.size() < Size)
                    return nullptr;
                std::memcpy(Result.get(), Stored.data(), (size_t)Size);
            }
            else
            {
                std::vector<uint8_t> Object((size_t)Size);
                if (!DecodeXSub23(Stored, CacheID, Object))
                    return nullptr;
                if (!Object.empty()) std::memcpy(Result.get(), Object.data(), Object.size());
            }
            ResultSize = (uint32_t)Size;
            return Result;
        }

        std::lock_guard<std::shared_mutex> Gaurd(ReadMutex);
        auto Reader = CoDFileHandle(FilesOf(CacheInfo.PackageFileIndex)->OpenFile(XPAKFileName, "r"), FilesOf(CacheInfo.PackageFileIndex));
#if _DEBUG
        printf("XSUBCache::ExtractPackageObject(): Streaming Object: 0x%llx from CASC File: %s\n", CacheID, XPAKFileName.c_str());
#endif // _DEBUG

        Reader.Seek(CacheInfo.Offset, SEEK_SET);

        // Cold War's object stored as is (no block framing): its bytes
        if (CacheInfo.Flags == 0)
        {
            ResultSize = 0;
            if (CacheInfo.CompressedSize == 0 || CacheInfo.CompressedSize > (1ull << 30)) return nullptr;
            auto Result = std::make_unique<uint8_t[]>((size_t)CacheInfo.CompressedSize);
            if (Reader.Read(Result.get(), 0, (size_t)CacheInfo.CompressedSize) != CacheInfo.CompressedSize) return nullptr;
            ResultSize = (uint32_t)CacheInfo.CompressedSize;
            return Result;
        }

        uint64_t DataRead = 0;
        uint64_t TotalDataSize = 0;
        uint64_t DecompressedSize = CacheInfo.UncompressedSize;

        // The object's bytes, grown as its blocks are read. A block's declared size past what an
        // object can hold (a corrupt or unknown block) ends the read with nothing, rather than a
        // failed allocation copied from: realloc returned null on such a size (a Cold War map's
        // texture, mp/zm maps' streamed districts placing more models)
        constexpr uint64_t MaxObject = 1ull << 30;
        std::vector<int8_t> Data;
        const auto Room = [&](uint64_t Needed) {
            if (TotalDataSize + Needed > MaxObject) return false;
            if (Data.size() < TotalDataSize + Needed) Data.resize((size_t)(TotalDataSize + Needed));
            return true;
        };

        while (DataRead < CacheInfo.CompressedSize)
        {
            auto Count = Reader.Read<uint32_t>();
            auto Offset = Reader.Read<uint32_t>();
            // I think there are uncompressed blocks with no info
            // for now skip them, very rare, need to see how to check for them
            if (Count > 256)
                break;
            uint32_t Commands[256];
            Reader.Read((uint8_t*)&Commands, 0, Count <= 30 ? 120 : Count * 4);

            for (uint32_t i = 0; i < Count; i++)
            {
                uint64_t BlockSize = (Commands[i] & 0xFFFFFF);
                uint32_t CompressedFlag = (Commands[i] >> 24);

                uint64_t CurrentPosition = Reader.Tell();

                // Check the block type (3 = compressed (lz4), 8 = compressed (oodle), 0 = raw data, anything else = skip over!)
                if (CompressedFlag == 0x3)
                {
                    auto DataBlock = Reader.Read(BlockSize);

                    if (DataBlock != nullptr)
                    {
                        if (!Room(0x2400000)) return ResultSize = 0, nullptr;
                        auto Result = Compression::DecompressLZ4Block((const int8_t*)DataBlock.get(), Data.data() + TotalDataSize, (uint32_t)BlockSize, 0x2400000);

                        TotalDataSize += Result;
                    }
                }
                else if (CompressedFlag == 0x8 || CompressedFlag == 0x9)
                {
                    auto DataBlock = Reader.Read(BlockSize);

                    if (DataBlock != nullptr && BlockSize >= 4)
                    {
                        // Read oodle decompressed size
                        uint32_t DecompressedSize = *(uint32_t*)(DataBlock.get());
                        if (!Room(DecompressedSize)) return ResultSize = 0, nullptr;
                        auto Result = Siren::Decompress(OodleDlls, (const uint8_t*)DataBlock.get() + 4, (uint32_t)BlockSize - 4, (uint8_t*)Data.data() + TotalDataSize, DecompressedSize);
                        TotalDataSize += Result;
                    }
                }
                else if (CompressedFlag == 0x6)
                {
                    auto DataBlock = Reader.Read(BlockSize);
                    // Check if we're at the end of the block/less than the max block size, if so, use that
                    uint64_t RawBlockSize = DecompressedSize < 262112 ? DecompressedSize : 262112;
                    DecompressedSize -= RawBlockSize;

                    if (DataBlock != nullptr)
                    {
                        if (!Room(RawBlockSize)) return ResultSize = 0, nullptr;
                        Siren::Decompress(OodleDlls, (const uint8_t*)DataBlock.get(), (uint32_t)BlockSize, (uint8_t*)Data.data() + TotalDataSize, RawBlockSize);
                        TotalDataSize += RawBlockSize;
                    }
                }
                else if (CompressedFlag == 0x0)
                {
                    auto DataBlock = Reader.Read(BlockSize);
                    DecompressedSize -= BlockSize;

                    if (DataBlock != nullptr)
                    {
                        if (!Room(BlockSize)) return ResultSize = 0, nullptr;
                        std::memcpy(Data.data() + TotalDataSize, DataBlock.get(), BlockSize);
                        TotalDataSize += BlockSize;
                    }
                }
                else
                {
                    // As far as we care, any other flag value is padding (0xCF is one of them)
                    Reader.Seek(BlockSize, SEEK_CUR);
                }

                // We must append the block size and pad it properly (If it's the last block)
                uint64_t NextSegmentOffset = 0;
                uint64_t TotalBlockSize = 0;

                if ((i + 1) < Count)
                {
                    TotalBlockSize = BlockSize;
                    NextSegmentOffset = (CurrentPosition + BlockSize);
                }
                else
                {
                    NextSegmentOffset = (((CurrentPosition + BlockSize) + 0x7F) & 0xFFFFFFFFFFFFF80);
                    TotalBlockSize += (NextSegmentOffset - CurrentPosition);
                }

                // Append to data read (Cus it includes padding)
                DataRead += TotalBlockSize;

                Reader.Seek(NextSegmentOffset, SEEK_SET);
            }

            // We must append the size of a header
            DataRead += Count <= 30 ? 128 : 8 + (4 * Count);
        }

        auto ResultBuffer = std::make_unique<uint8_t[]>((size_t)TotalDataSize);
        if (TotalDataSize) std::memcpy(ResultBuffer.get(), Data.data(), (size_t)TotalDataSize);

        ResultSize = (uint32_t)TotalDataSize;

        return ResultBuffer;
    }

    ResultSize = 0;
    // in no file: on the CDN (cdn_images on)
    if (const RemoteObject* R = FindRemote(CacheID)) return FetchRemote(*R, ResultSize);

    return nullptr;
}