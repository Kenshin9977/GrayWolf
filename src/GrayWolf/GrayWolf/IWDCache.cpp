#include "stdafx.h"

#include "IWDCache.h"

#include "Hashing.h"
#include "FileSystems.h"
#include "Strings.h"
#include "ZipInflate.h"

#include <filesystem>
#include <miniz/miniz.h>

IWDCache::IWDCache()
{
}

IWDCache::~IWDCache()
{
}

void IWDCache::LoadPackageCache(const std::string& BasePath)
{
    CoDPackageCache::LoadPackageCache(BasePath);
    LoadArchives(BasePath);
    this->SetLoadedState();
}

void IWDCache::LoadArchives(const std::string& Dir)
{
    std::error_code Ec;
    for (const auto& File : std::filesystem::directory_iterator(Dir, Ec))
    {
        if (File.is_regular_file(Ec) && Strings::ToLower(File.path().extension().string()) == ".iwd")
            this->LoadPackage(File.path().string());
    }
}

bool IWDCache::LoadPackage(const std::string& FilePath)
{
    CoDPackageCache::LoadPackage(FilePath);

    auto PackageIndex = (uint32_t)PackageFilePaths.size();

    mz_zip_archive ZipArchive;
    std::memset(&ZipArchive, 0, sizeof(ZipArchive));

    if (!mz_zip_reader_init_file(&ZipArchive, FilePath.c_str(), 0))
    {
        return false;
    }
    else
    {
        PackageFilePaths.push_back(FilePath);
    }

    auto FileCount = (uint32_t)mz_zip_reader_get_num_files(&ZipArchive);

    for (uint32_t i = 0; i < FileCount; i++)
    {
        mz_zip_archive_file_stat FileInfo;
        if (mz_zip_reader_file_stat(&ZipArchive, i, &FileInfo))
        {
            auto EntryName = std::string(FileInfo.m_filename);

            // IWI (image) or WAV (BO1's IWD-packed sound container), both hashed the same way
            if (EntryName.ends_with(".iwi") || EntryName.ends_with(".wav"))
            {
                auto AssetEntryHash = Hashing::HashXXHashString(FileSystems::GetFileNameWithoutExtension(EntryName));

                PackageCacheObject NewObject;
                NewObject.Offset = i;
                NewObject.CompressedSize = FileInfo.m_comp_size;
                NewObject.UncompressedSize = FileInfo.m_uncomp_size;
                NewObject.PackageFileIndex = PackageIndex;

                CacheObjects.insert(std::make_pair(AssetEntryHash, NewObject));
            }
        }
    }

    mz_zip_reader_end(&ZipArchive);

    return true;
}

std::unique_ptr<uint8_t[]> IWDCache::ExtractPackageObject(uint64_t CacheID, uint32_t& ResultSize)
{
    if (CacheObjects.find(CacheID) != CacheObjects.end())
    {
        auto& CacheInfo = CacheObjects[CacheID];
        auto& IWDFileName = PackageFilePaths[CacheInfo.PackageFileIndex];

        mz_zip_archive ZipArchive;
        std::memset(&ZipArchive, 0, sizeof(ZipArchive));
        if (!mz_zip_reader_init_file(&ZipArchive, IWDFileName.c_str(), 0))
        {
            return nullptr;
        }

        auto ResultBuffer = std::make_unique<uint8_t[]>(CacheInfo.UncompressedSize);

        // Offset is the entry index
        if (ZipInflate::Extract(&ZipArchive, (mz_uint)CacheInfo.Offset, ResultBuffer.get(), CacheInfo.UncompressedSize))
        {
            mz_zip_reader_end(&ZipArchive);
            ResultSize = (uint32_t)CacheInfo.UncompressedSize;
            return ResultBuffer;
        }

        mz_zip_reader_end(&ZipArchive);
    }

    ResultSize = 0;
    
    return nullptr;
}

uint64_t IWDCache::HashPackageID(const std::string& Value)
{
    return Hashing::HashXXHashString(Value);
}