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

        ImageInfo.ImageFormat = Formats[ImageInfo.ImageFormat];

        // the largest mip the packages give (the install's, the game's on-demand cache, the CDN
        // when cdn_images is on: XSUBCache), the last record being the full size
        size_t HighestIndex = MipCount;
        for (size_t i = MipCount; i-- > 0 && ImageData == nullptr;)
        {
            if (!CoDAssets::GamePackageCache->Exists(Mips.MipMaps[i].HashID)) continue;
            uint32_t PackageSize = 0;
            ImageData = CoDAssets::GamePackageCache->ExtractPackageObject(Mips.MipMaps[i].HashID, Mips.GetImageSize(i), PackageSize);
            ImageSize = PackageSize;
            HighestIndex = i;
        }

        if (ImageData == nullptr || ImageSize == 0)
            return nullptr;

        return CoDRawImageTranslator::TranslateBC(ImageData, ImageSize, ImageInfo.Width >> (MipCount - HighestIndex - 1), ImageInfo.Height >> (MipCount - HighestIndex - 1),
                                                  ImageInfo.ImageFormat);
    }
}
