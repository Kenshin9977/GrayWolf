#include "stdafx.h"

#include "XPAKCache.h"

#include "FileSystems.h"
#include "Compression.h"
#include "MemoryReader.h"
#include "Siren.h"

#include "DBGameFiles.h"


#include <filesystem>

#include "CoDFileHandle.h"
#include "WinFileSystem.h"
#include "CascFileSystem.h"
#include "ExportNaming.h"

#include <algorithm>
#include <fstream>
#include <vector>

XPAKCache::XPAKCache()
{
    Siren::Initialize(L"oo2core_6_win64.dll");
}

XPAKCache::~XPAKCache()
{
    Siren::Shutdown();
}

void XPAKCache::LoadPackageCache(const std::string& BasePath)
{
    CoDPackageCache::LoadPackageCache(BasePath);

    // The base leaves FileSystem null when it can't open the path (e.g. a CASC container that fails
    // to open): bail out before enumerating against it.
    if (FileSystem == nullptr)
    {
        this->SetLoadedState();
        return;
    }

    // A CASC install's tables are kept (CoDPackageCache::LoadKept): reading them through CASC takes
    // seconds. A game update changes the key: the tables are read again.
    const bool Casc = dynamic_cast<CascFileSystem*>(FileSystem.get()) != nullptr;
    std::string Kept, Key;
    if (Casc)
    {
        Kept = KeptFile(BasePath, ".xpk");
        Key = KeptKey("*.xpak", "xpak");
        if (LoadKept(Kept, Key))
        {
            this->SetLoadedState();
            return;
        }
    }

    FileSystem->EnumerateFiles("*.xpak", [this](const std::string& name, const size_t size)
    {
        this->LoadPackage(name);
    });
    if (Casc) SaveKept(Kept, Key);

    this->SetLoadedState();
}

void XPAKCache::LoadPackagesIn(const std::string& Dir)
{
    std::error_code Ec;
    if (!std::filesystem::is_directory(Dir, Ec))
        return;
    for (const auto& E : std::filesystem::directory_iterator(Dir, Ec))
    {
        if (Ec) break;
        auto Ext = E.path().extension().string();
        for (auto& C : Ext) C = (char)tolower((unsigned char)C);
        if (Ext == ".xpak")
            LoadPackage(E.path().string());
    }
}

bool XPAKCache::LoadPackage(const std::string& FilePath)
{
    CoDPackageCache::LoadPackage(FilePath);

    auto PackageIndex = (uint32_t)PackageFilePaths.size();

	if (FileSystem == nullptr)
		FileSystem = std::make_unique<WinFileSystem>(FileSystems::GetDirectoryName(FilePath));

    auto Reader = CoDFileHandle(FileSystem->OpenFile(FilePath, "r"), FileSystem.get());

    auto Header = Reader.Read<BO3XPakHeader>();

    // If MW4 we need to skip the new bytes
    if (Header.Version == 0xD)
    {
        Reader.Seek(0, SEEK_SET);
        Reader.Read((uint8_t*)&Header, 0, 24);
        Reader.Seek(288, SEEK_CUR);
        Reader.Read((uint8_t*)&Header + 24, 0, 96);
    }

    if (Header.Magic == 0x4950414b && Header.HashOffset < Reader.Size())
    {
        Reader.Seek(Header.HashOffset, SEEK_SET);

        // The whole table in one read (an entry at a time was a file system read each, a million for BO4
        // through CASC). A short read leaves the rest zero.
        std::vector<BO3XPakHashEntry> Entries((size_t)Header.HashCount);
        Reader.Read(reinterpret_cast<uint8_t*>(Entries.data()), Entries.size() * sizeof(BO3XPakHashEntry));

        for (const auto& Entry : Entries)
        {

            PackageCacheObject NewObject{};
            NewObject.Offset = Header.DataOffset + Entry.Offset;
            NewObject.CompressedSize = Entry.Size & 0xFFFFFFFFFFFFFF; // 0x80 in last 8 bits in some entries in new XPAKs
            NewObject.UncompressedSize = 0;
            NewObject.PackageFileIndex = PackageIndex;
            CacheObjects.insert(std::make_pair(Entry.Key, NewObject));
        }

        PackageFilePaths.push_back(FilePath);

        return true;
    }

    return false;
}

std::unique_ptr<uint8_t[]> XPAKCache::ExtractPackageObject(uint64_t CacheID, int32_t Size, uint32_t& ResultSize)
{
    if (PackageCacheObject CacheInfo; Find(CacheID, CacheInfo))
    {
        std::vector<uint8_t> payload;
        bool PaddedBlocks = false;
        {
            // Get-or-create this package's handle and its own mutex: only map bookkeeping, never the I/O,
            // so it never blocks a thread working on a different package.
            std::mutex* PackageLock;
            CoDFileHandle* Reader;
            {
                std::lock_guard<std::mutex> Guard(MapLock);
                auto It = OpenPackageHandles.find(CacheInfo.PackageFileIndex);
                if (It == OpenPackageHandles.end())
                {
                    const auto& XPAKFileName = PackageFilePaths[CacheInfo.PackageFileIndex];
                    auto NewHandle = std::make_unique<CoDFileHandle>(FileSystem->OpenFile(XPAKFileName, "r"), FileSystem.get());
                    // A version 0xD package (Modern Warfare 2019) aligns each block to 4 bytes
                    Padded[CacheInfo.PackageFileIndex] = NewHandle->IsValid() && NewHandle->Read<BO3XPakHeader>().Version == 0xD;
                    It = OpenPackageHandles.emplace(CacheInfo.PackageFileIndex, std::move(NewHandle)).first;
                    PackageLocks.emplace(CacheInfo.PackageFileIndex, std::make_unique<std::mutex>());
                }
                Reader = It->second.get();
                PackageLock = PackageLocks[CacheInfo.PackageFileIndex].get();
                PaddedBlocks = Padded[CacheInfo.PackageFileIndex];
            }

            // Seek+Read together, locked per PACKAGE FILE: a shared HANDLE has one file pointer,
            // so two threads racing the same package would silently read each other's offsets -
            // but two threads on different packages take different locks and run concurrently.
            std::lock_guard<std::mutex> Guard(*PackageLock);

            if (!Reader->IsValid())
            {
                ResultSize = 0;
                return nullptr;
            }

#if _DEBUG
            const auto& XPAKFileName = PackageFilePaths[CacheInfo.PackageFileIndex];
            printf("XPAKCache::ExtractPackageObject(): Streaming Object: 0x%llx from File: %s\n", CacheID, XPAKFileName.c_str());
#endif // _DEBUG

            Reader->Seek(CacheInfo.Offset, SEEK_SET);

            auto raw = Reader->Read(CacheInfo.CompressedSize);
            payload.assign(raw.get(), raw.get() + CacheInfo.CompressedSize);
        }

        const uint64_t DecompressedSize = Size == -1 ? CacheInfo.UncompressedSize : Size;
        size_t ResultSizeSizeT = 0;

        auto outputBuffer = DecompressPackageObject(CacheID, payload.data(), CacheInfo.CompressedSize, DecompressedSize, ResultSizeSizeT, PaddedBlocks);

        // TODO: Switch to size_t in package class
        ResultSize = (uint32_t)ResultSizeSizeT;
        return outputBuffer;
    }

    ResultSize = 0;

    return nullptr;
}

bool XPAKCache::FindNamed(const std::string& Name, const std::string& Stem, NamedObject& Out)
{
    const std::string Want = ExportNaming::Lower(Name), Prefix = ExportNaming::Lower(Stem);
    std::lock_guard<std::mutex> Guard(NamedLock);
    for (uint32_t P = 0; P < (uint32_t)PackageFilePaths.size(); P++)
    {
        const std::string File = ExportNaming::Lower(std::filesystem::path(PackageFilePaths[P]).stem().string());
        if (File != Prefix && File.rfind(Prefix + "_", 0) != 0)
            continue;
        auto It = Named.find(P);
        if (It == Named.end())
        {
            It = Named.emplace(P, std::unordered_map<std::string, NamedObject>()).first;
            if (FileSystem == nullptr)
                continue;
            // the index: records of { key u64, length u64, text }, the text "field: value" lines
            CoDFileHandle Reader(FileSystem->OpenFile(PackageFilePaths[P], "r"), FileSystem.get());
            if (!Reader.IsValid())
                continue;
            const auto Header = Reader.Read<BO3XPakHeader>();
            if (Header.Magic != 0x4950414b || Header.Version == 0xD || Header.IndexSize == 0 ||
                Header.IndexOffset + Header.IndexSize > (uint64_t)Reader.Size() || Header.IndexSize > (1ull << 30))
                continue;
            Reader.Seek(Header.IndexOffset, SEEK_SET);
            std::vector<uint8_t> Raw((size_t)Header.IndexSize);
            Reader.Read(Raw.data(), Raw.size());
            for (size_t At = 0; At + 16 <= Raw.size();)
            {
                uint64_t Key = 0, Length = 0;
                std::memcpy(&Key, Raw.data() + At, 8);
                std::memcpy(&Length, Raw.data() + At + 8, 8);
                if (Length > Raw.size() - At - 16)
                    break;
                const std::string Text((const char*)Raw.data() + At + 16, (size_t)Length);
                At += 16 + (size_t)Length;
                NamedObject Object;
                Object.Key = Key;
                std::string ObjectName;
                size_t Line = 0;
                while (Line < Text.size())
                {
                    size_t End = Text.find('\n', Line);
                    if (End == std::string::npos) End = Text.size();
                    const std::string L = Text.substr(Line, End - Line);
                    Line = End + 1;
                    const size_t Colon = L.find(": ");
                    if (Colon == std::string::npos) continue;
                    const std::string Field = L.substr(0, Colon), Value = L.substr(Colon + 2);
                    if (Field == "name") ObjectName = ExportNaming::Lower(Value);
                    else if (Field == "type") Object.Type = Value;
                    else if (Field == "size0") Object.Size = std::strtoull(Value.c_str(), nullptr, 10);
                    else if (Field.rfind("size", 0) == 0) Object.Parts++;
                }
                if (Object.Size != 0) Object.Parts++;
                if (!ObjectName.empty() && Exists(Key))
                    It->second.emplace(ObjectName, Object);
            }
        }
        const auto Found = It->second.find(Want);
        if (Found != It->second.end())
        {
            Out = Found->second;
            return true;
        }
    }
    return false;
}

std::unique_ptr<uint8_t[]> XPAKCache::DecompressPackageObject(uint64_t cacheID, uint8_t* buffer, size_t bufferSize, size_t decompressedSize, size_t& resultSize, bool paddedBlocks)
{
    resultSize = 0;

    // We don't accept unknown sizes in xsub, caller must know how much memory is needed.
    if (decompressedSize == 0)
        return nullptr;

    // Our final big blob of data to return, this will be the entire decompressed buffer.
    auto result = std::make_unique<uint8_t[]>(decompressedSize);
    auto reader = MemoryReader((int8_t*)buffer, bufferSize, true);
    auto remaining = decompressedSize;

    while (reader.GetPosition() < reader.GetLength())
    {
        const auto blockHeader = reader.Read<BO3XPakDataHeader>();

        for (uint32_t i = 0; i < blockHeader.Count; i++)
        {
            const size_t blockSize = (blockHeader.Commands[i] & 0xFFFFFF);
            const size_t flag = (blockHeader.Commands[i] >> 24);

            size_t blockDecompressedSize = 0;

            // Get the current stream, avoids constant allocations as we know we have a memory pointer.
            const auto dataBlock = reader.GetCurrentStream(blockSize);

            // If we hit EOF on this, we can't verify this stream is valid or something is wrong.
            if (dataBlock == nullptr)
            {
                resultSize = 0;
                return nullptr;
            }

            // Anything else = skip over
            switch (flag)
            {
                case 0x3: // compressed (lz4)
                {
                    blockDecompressedSize = remaining;
                    const size_t decompressedResult = Compression::DecompressLZ4Block(dataBlock, (int8_t*)result.get() + resultSize, blockSize, (int32_t)(blockDecompressedSize));
                    resultSize += decompressedResult;
                    remaining -= decompressedResult;
                    break;
                }
                case 0x6:
                {
                    blockDecompressedSize = std::min<size_t>(remaining, 262112);
                    Siren::Decompress((const uint8_t*)dataBlock, blockSize, result.get() + resultSize, blockDecompressedSize);
                    resultSize += blockDecompressedSize;
                    remaining -= blockDecompressedSize;
                    break;
                }
                case 0x8: // compressed (oodle)
                {
                    blockDecompressedSize = *(uint32_t*)(dataBlock);
                    Siren::Decompress((const uint8_t*)(dataBlock + 4), blockSize - 4, result.get() + resultSize, blockDecompressedSize);
                    resultSize += blockDecompressedSize;
                    remaining -= blockDecompressedSize;
                    break;
                }
                case 0x0: // raw data
                {
                    std::memcpy(result.get() + resultSize, dataBlock, blockSize);
                    resultSize += blockSize;
                    remaining -= blockSize;
                    break;
                }
                default:
                {
                    break;
                }
            }

            if (paddedBlocks)
                reader.Advance(((blockSize + 3) & 0xFFFFFFFC) - blockSize);
        }

        reader.SetPosition((reader.GetPosition() + 0x7F) & 0xFFFFFFFFFFFFF80);
    }

    return result;
}
