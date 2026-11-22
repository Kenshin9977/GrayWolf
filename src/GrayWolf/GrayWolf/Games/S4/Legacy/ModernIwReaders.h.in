#pragma once

// What the readers of MW (2019), Vanguard, MW2 (2022) and MW3 (2023) read alike: skinned weights
// stored column by column, and images whose mips are package objects (local, on demand or CDN).

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "CoDRawImageTranslator.h"
#include "CoDXModelTranslator.h"
#include "MemoryReader.h"

namespace ModernIwReaders
{
    // A surface's rigid vertices: VertListcount runs of vertices on one bone (TRigid)
    template <typename TRigid>
    void ReadRigidWeights(std::vector<WeightsData>& Weights, uint32_t& Index, const XModelSubmesh_t& Submesh)
    {
        for (uint32_t i = 0; i < Submesh.VertListcount; i++)
        {
            auto RigidInfo = CoDAssets::GameInstance->Read<TRigid>(Submesh.RigidWeightsPtr + (i * sizeof(TRigid)));
            for (uint32_t w = 0; w < RigidInfo.VertexCount; w++)
            {
                Weights[Index].BoneValues[0] = RigidInfo.BoneIndex;
                Index++;
            }
        }
    }

    // Then the skinned ones: eight groups (WeightCounts[g] vertices on g + 1 bones), each stored
    // influence by influence: every vertex's first bone (and 2 bytes of padding), then every
    // vertex's (bone, u16 weight) for the second influence, and so on
    inline void ReadSkinnedWeights(MemoryReader& Reader, std::vector<WeightsData>& Weights, uint32_t Index, const XModelSubmesh_t& Submesh)
    {
        for (int i = 0; i < 8; i++)
        {
            for (int k = 0; k < i + 1; k++)
            {
                uint32_t LocalWeightDataIndex = Index;

                for (int w = 0; w < Submesh.WeightCounts[i]; w++)
                {
                    Weights[LocalWeightDataIndex].WeightCount = k + 1;
                    Weights[LocalWeightDataIndex].BoneValues[k] = Reader.Read<uint16_t>();

                    if (k > 0)
                    {
                        Weights[LocalWeightDataIndex].WeightValues[k] = ((float)Reader.Read<uint16_t>() / 65536.0f);
                        Weights[LocalWeightDataIndex].WeightValues[0] -= Weights[LocalWeightDataIndex].WeightValues[k];
                    }
                    else
                    {
                        Reader.Advance(2);
                    }

                    LocalWeightDataIndex++;
                }
            }

            Index += Submesh.WeightCounts[i];
        }
    }

    // The skinned weights' bytes for ReadSkinnedWeights, read from the zone
    inline MemoryReader ReadSkinnedWeightData(const XModelSubmesh_t& Submesh)
    {
        uintptr_t ReadDataSize = 0;
        auto WeightsDataLength = ((4 * Submesh.WeightCounts[0]) + (8 * Submesh.WeightCounts[1]) + (12 * Submesh.WeightCounts[2]) + (16 * Submesh.WeightCounts[3]) +
                                  (20 * Submesh.WeightCounts[4]) + (24 * Submesh.WeightCounts[5]) + (28 * Submesh.WeightCounts[6]) + (32 * Submesh.WeightCounts[7]));
        return MemoryReader(CoDAssets::GameInstance->Read(Submesh.WeightsPtr, WeightsDataLength, ReadDataSize), WeightsDataLength);
    }

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
