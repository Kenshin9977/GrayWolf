#pragma once

// What a modern IW reader shares (MW3 (2023) today): images whose mips are package objects
// (local, on demand or CDN).

#include <algorithm>
#include <cstdint>
#include <memory>

#include "CoDRawImageTranslator.h"
#include "CoDXModelTranslator.h"

namespace ModernIwReaders
{
    // An image (TImage) of MW2 (2022) / MW3 (2023): its pixels in the zone when loaded, else its
    // largest mip from the CDN or the on-demand cache, else the largest the local packages hold.
    // Formats maps the game's format to DXGI.
    template <typename TImage, typename TMipArray>
    std::unique_ptr<XImageDDS> LoadMipImage(const XImage_t& Image, const uint32_t* Formats)
    {
        auto ImageInfo = CoDAssets::GameInstance->Read<TImage>(Image.ImagePtr);
        std::unique_ptr<uint8_t[]> ImageData = nullptr;
        size_t ImageSize = 0;

        if (ImageInfo.LoadedImagePtr != 0)
        {
            ImageData = std::make_unique<uint8_t[]>(ImageInfo.BufferSize);
            ImageSize = (size_t)ImageInfo.BufferSize;

            if (CoDAssets::GameInstance->Read(ImageData.get(), ImageInfo.LoadedImagePtr, ImageSize) != ImageSize)
                return nullptr;

            return CoDRawImageTranslator::TranslateBC(ImageData, ImageSize, ImageInfo.Width, ImageInfo.Height, Formats[ImageInfo.ImageFormat]);
        }

        if (ImageInfo.MipMaps == 0)
            return nullptr;

        TMipArray Mips{};
        const size_t MipSize = sizeof(Mips.MipMaps[0]);
        size_t MipCount = std::min((size_t)ImageInfo.MipCount, Mips.GetMipCount());

        if (CoDAssets::GameInstance->Read((uint8_t*)&Mips.MipMaps, ImageInfo.MipMaps, MipCount * MipSize) != (MipCount * MipSize))
            return nullptr;

        // Fallback: the best mip present in the local files, in case the CDN is not viable
        size_t Fallback = 0;
        size_t HighestIndex = (size_t)ImageInfo.MipCount - 1;

        for (size_t i = 0; i < MipCount; i++)
        {
            if (CoDAssets::GamePackageCache->Exists(Mips.MipMaps[i].HashID))
            {
                Fallback = i;
            }
        }

        ImageInfo.ImageFormat = Formats[ImageInfo.ImageFormat];

        if (Fallback != HighestIndex && CoDAssets::OnDemandCache != nullptr)
        {
            uint32_t PackageSize = 0;
            ImageData = CoDAssets::OnDemandCache->ExtractPackageObject(Mips.MipMaps[HighestIndex].HashID, Mips.GetImageSize(HighestIndex), PackageSize);
            ImageSize = PackageSize;
        }

        // Mips above the fallback: try the on-demand version from the CDN
        if (Fallback != HighestIndex && CoDAssets::CDNDownloader != nullptr)
        {
            ImageData = CoDAssets::CDNDownloader->ExtractCDNObject(Mips.MipMaps[HighestIndex].HashID, Mips.GetImageSize(HighestIndex), ImageSize);
        }

        if (ImageData == nullptr)
        {
            uint32_t PackageSize = 0;
            ImageData = CoDAssets::GamePackageCache->ExtractPackageObject(Mips.MipMaps[Fallback].HashID, Mips.GetImageSize(Fallback), PackageSize);
            ImageSize = PackageSize;
            HighestIndex = Fallback;
        }

        if (ImageData == nullptr)
            return nullptr;

        return CoDRawImageTranslator::TranslateBC(ImageData, ImageSize, ImageInfo.Width >> (MipCount - HighestIndex - 1), ImageInfo.Height >> (MipCount - HighestIndex - 1),
                                                  ImageInfo.ImageFormat);
    }
}
