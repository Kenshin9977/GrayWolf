#include "stdafx.h"
#include "XSUBCache.h"

#include "Compression.h"
#include "MemoryReader.h"
#include "Siren.h"

#include "CoDFileHandle.h"

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
    constexpr uint16_t FirstSizedVersion = 23;

    // A version-23 object's stored bytes (Stored) to its Size bytes; false on any inconsistency
    bool DecodeXSub23(const std::vector<uint8_t>& Stored, uint64_t Key, std::vector<uint8_t>& Object)
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
                    Done = Siren::Decompress(Src, B.CompressedSize, Dst, B.Size) == B.Size;
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

XSUBCache::XSUBCache()
{
    Siren::Initialize(L"oo2core_8_win64.dll");
}

XSUBCache::~XSUBCache()
{
    Siren::Shutdown();
}

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

    this->SetLoadedState();
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
    if (CacheObjects.find(CacheID) != CacheObjects.end())
    {
        std::lock_guard<std::shared_mutex> Gaurd(ReadMutex);

        // Uncompressed size = offset of data segment
        auto& CacheInfo = CacheObjects[CacheID];
        auto& XPAKFileName = PackageFilePaths[CacheInfo.PackageFileIndex];
        auto Reader = CoDFileHandle(FileSystem->OpenFile(XPAKFileName, "r"), FileSystem.get());
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
    if (CacheObjects.find(CacheID) != CacheObjects.end())
    {
        std::lock_guard<std::shared_mutex> Gaurd(ReadMutex);

        // Uncompressed size = offset of data segment
        auto& CacheInfo = CacheObjects[CacheID];
        auto& XPAKFileName = PackageFilePaths[CacheInfo.PackageFileIndex];
        auto Reader = CoDFileHandle(FileSystem->OpenFile(XPAKFileName, "r"), FileSystem.get());
#if _DEBUG
        printf("XSUBCache::ExtractPackageObject(): Streaming Object: 0x%llx from CASC File: %s\n", CacheID, XPAKFileName.c_str());
#endif // _DEBUG


        Reader.Seek(CacheInfo.Offset, SEEK_SET);

        if (PackageVersions[CacheInfo.PackageFileIndex] >= FirstSizedVersion)
        {
            ResultSize = 0;
            constexpr uint64_t MaxObject = 1ull << 30;
            if (CacheInfo.CompressedSize > MaxObject || CacheInfo.UncompressedSize > MaxObject)
                return nullptr;
            std::vector<uint8_t> Stored((size_t)CacheInfo.CompressedSize);
            if (Reader.Read(Stored.data(), 0, Stored.size()) != Stored.size())
                return nullptr;
            auto Result = std::make_unique<uint8_t[]>((size_t)std::max<uint64_t>(CacheInfo.UncompressedSize, 1));
            if (CacheInfo.Flags == 0)
            {
                // stored as is
                if (Stored.size() < CacheInfo.UncompressedSize)
                    return nullptr;
                std::memcpy(Result.get(), Stored.data(), (size_t)CacheInfo.UncompressedSize);
            }
            else
            {
                std::vector<uint8_t> Object((size_t)CacheInfo.UncompressedSize);
                if (!DecodeXSub23(Stored, CacheID, Object))
                    return nullptr;
                if (!Object.empty()) std::memcpy(Result.get(), Object.data(), Object.size());
            }
            ResultSize = (uint32_t)CacheInfo.UncompressedSize;
            return Result;
        }

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
                        auto Result = Siren::Decompress((const uint8_t*)DataBlock.get() + 4, (uint32_t)BlockSize - 4, (uint8_t*)Data.data() + TotalDataSize, DecompressedSize);
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
                        Siren::Decompress((const uint8_t*)DataBlock.get(), (uint32_t)BlockSize, (uint8_t*)Data.data() + TotalDataSize, RawBlockSize);
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

    return nullptr;
}