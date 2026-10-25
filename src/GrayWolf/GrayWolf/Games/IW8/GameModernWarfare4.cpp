#include "stdafx.h"

#include "GameModernWarfare4.h"

#include "CoDAssets.h"
#include "Games/Common/MeshDecode.h"
#include "GameReaders.h"
#include "CoDRawImageTranslator.h"
#include "DBGameFiles.h"
#include "SABSupport.h"
#include "FileSystems.h"
#include "MemoryReader.h"
#include "SettingsManager.h"
#include "HalfFloats.h"

std::unique_ptr<XAnim_t> GameModernWarfare4::ReadXAnim(const CoDAnim_t* Animation)
{
    // no viewmodel prefixes; an additive one (type 6) said so
    return GameReaders::ReadAnim<MW4XAnim, MW4XAnimDeltaParts>(Animation, 4, {}, [](XAnim_t& Anim, const MW4XAnim& Data) {
        if (Data.AssetType == 0x6) Anim.AdditiveAnimation = true;
    });
}

std::unique_ptr<XModel_t> GameModernWarfare4::ReadXModel(const CoDModel_t* Model)
{
    if (CoDAssets::GameInstance->IsRunning())
    {
        auto ModelData = CoDAssets::GameInstance->Read<MW4XModel>(Model->AssetPointer);

        auto ModelAsset = std::make_unique<XModel_t>(ModelData.NumLods);

        GameReaders::CopyStreamedBones(*ModelAsset, Model->AssetName, ModelData, 1);

        for (uint32_t i = 0; i < ModelData.NumLods; i++)
        {
            ModelAsset->ModelLods.emplace_back(ModelData.ModelLods[i].NumSurfs);
            auto& LodReference = ModelAsset->ModelLods[i];

            LodReference.LodDistance = ModelData.ModelLods[i].LodDistance;

            LodReference.LODStreamInfoPtr = ModelData.ModelLods[i].MeshPtr;

            auto XSurfacePtr = ModelData.ModelLods[i].SurfsPtr;

            for (uint32_t s = 0; s < ModelData.ModelLods[i].NumSurfs; s++)
            {
                LodReference.Submeshes.emplace_back();
                auto& SubmeshReference = LodReference.Submeshes[s];

                auto SurfaceInfo = CoDAssets::GameInstance->Read<MW4XModelSurface>(XSurfacePtr);

                SubmeshReference.RigidWeightsPtr  = SurfaceInfo.RigidWeightsPtr;
                SubmeshReference.VertListcount    = SurfaceInfo.VertListCount;
                SubmeshReference.VertexCount      = SurfaceInfo.VertexCount;
                SubmeshReference.FaceCount        = SurfaceInfo.FacesCount;
                SubmeshReference.VertexPtr        = SurfaceInfo.Offsets[0];
                SubmeshReference.VertexUVsPtr     = SurfaceInfo.Offsets[2];
                SubmeshReference.VertexNormalsPtr = SurfaceInfo.Offsets[3];
                SubmeshReference.FacesPtr         = SurfaceInfo.Offsets[4];
                SubmeshReference.VertexColorPtr   = SurfaceInfo.Offsets[6];
                SubmeshReference.Scale            = fmaxf(fmaxf(SurfaceInfo.Min, SurfaceInfo.Scale), SurfaceInfo.Max);
                SubmeshReference.XOffset          = SurfaceInfo.XOffset;
                SubmeshReference.YOffset          = SurfaceInfo.YOffset;
                SubmeshReference.ZOffset          = SurfaceInfo.ZOffset;

                for (int W = 0; W < 8; W++) SubmeshReference.WeightCounts[W] = SurfaceInfo.WeightCounts[W];
                SubmeshReference.WeightsPtr = SurfaceInfo.WeightsPtr;

                auto MaterialHandle = CoDAssets::GameInstance->Read<uint64_t>(ModelData.MaterialHandlesPtr);
                LodReference.Materials.emplace_back(ReadXMaterial(MaterialHandle));

                XSurfacePtr += sizeof(MW4XModelSurface);
                ModelData.MaterialHandlesPtr += sizeof(uint64_t);
            }
        }

        return ModelAsset;
    }
    return nullptr;
}

std::unique_ptr<XImageDDS> GameModernWarfare4::ReadXImage(const CoDImage_t* Image)
{
    return LoadXImage(XImage_t(ImageUsageType::DiffuseMap, 0, Image->AssetPointer, Image->AssetName));
}

std::unique_ptr<XSound> GameModernWarfare4::ReadXSound(const CoDSound_t* Sound)
{
    std::unique_ptr<uint8_t[]> SoundBuffer = nullptr;
    size_t OpusOffset = 0, Held = (size_t)Sound->AssetSize;

    if (Sound->DataType == SoundDataTypes::Opus_Interleaved_Streamed)
    {
        // A bank entry (SAB v17): its seek table, then the packets
        auto Entry = CoDAssets::GameInstance->Read<SABv17Entry>(Sound->AssetPointer);

        uint32_t ResultSize = 0;
        SoundBuffer = CoDAssets::GamePackageCache->ExtractPackageObject(Entry.Offset, Entry.Size + Entry.SeekTableLength, ResultSize);
        OpusOffset = Entry.SeekTableLength;
        Held = ResultSize;
    }
    else
    {
        SoundBuffer = std::make_unique<uint8_t[]>((size_t)Sound->AssetSize);

        if (CoDAssets::GameInstance->Read(SoundBuffer.get(), Sound->AssetPointer, Sound->AssetSize) != (size_t)Sound->AssetSize)
            return nullptr;
    }

    if (SoundBuffer == nullptr)
        return nullptr;
    return SABSupport::OpusPackets(SoundBuffer.get(), Held, OpusOffset, (size_t)Sound->AssetSize, Sound->FrameRate, Sound->ChannelsCount, Sound->FrameCount,
                                   SABSupport::OpusPreSkipUnmeasured);
}

thread_local GameReaders::ImagesElsewhereFn GameModernWarfare4::ImagesElsewhere;

const XMaterial_t GameModernWarfare4::ReadXMaterial(uint64_t MaterialPointer)
{
    // an image's name pointer first in it; type 0 the colour map
    return GameReaders::ReadNamedMaterial<MW4XMaterial, MW4XMaterialImage>(
        MaterialPointer, 0, ImagesElsewhere, [](uint32_t Semantic) { return Semantic == 0 ? ImageUsageType::DiffuseMap : ImageUsageType::Unknown; });
}

std::unique_ptr<XImageDDS> GameModernWarfare4::LoadXImage(const XImage_t& Image)
{
    std::unique_ptr<uint8_t[]> ImageData = nullptr;
    // Both loaded and streamed images are read
    uint32_t ResultSize = 0;

    auto ImageInfo = CoDAssets::GameInstance->Read<MW4GfxImage>(Image.ImagePtr);
    uint64_t ImagePointer = *(uint64_t*)&ImageInfo.Padding5[8];
    uint32_t LoadedImageSize = *(uint32_t*)&ImageInfo.Padding2[7];

    uint32_t Width = 0;
    uint32_t Height = 0;

    if (ImagePointer != 0)
    {
        ImageData = std::make_unique<uint8_t[]>(LoadedImageSize);

        if (CoDAssets::GameInstance->Read(ImageData.get(), ImagePointer, LoadedImageSize) != LoadedImageSize)
            return nullptr;

        Width = ImageInfo.LoadedMipWidth;
        Height = ImageInfo.LoadedMipHeight;
        ResultSize = LoadedImageSize;
    }
    else if (CoDAssets::GamePackageCache != nullptr)
    {
        // Fallback: the best mip present in the local files, in case the CDN is not viable
        size_t Fallback = 0;
        size_t HighestIndex = 4;

        for (size_t i = 0; i < 4; i++)
        {
            if (ImageInfo.Mips.Levels[i].HashID != 0)
            {
                if (CoDAssets::GamePackageCache->Exists(ImageInfo.Mips.Levels[i].HashID))
                {
                    Fallback = i;
                }

                HighestIndex = i;
            }
        }

        // Highest mip not in the files: try the on-demand cache, then the CDN
        // (no on-demand cache when the zone is read from disk)
        if (HighestIndex != Fallback && CoDAssets::OnDemandCache != nullptr)
        {
            ImageData = CoDAssets::OnDemandCache->ExtractPackageObject(ImageInfo.Mips.Levels[HighestIndex].HashID, ImageInfo.Mips.GetImageSize(HighestIndex), ResultSize);

            if (ImageData == nullptr && CoDAssets::CDNDownloader != nullptr)
            {
                size_t CDNSize = 0;
                ImageData = CoDAssets::CDNDownloader->ExtractCDNObject(ImageInfo.Mips.Levels[HighestIndex].HashID, ImageInfo.Mips.GetImageSize(HighestIndex), CDNSize);
                ResultSize = (uint32_t)CDNSize;
            }
        }

        // No CDN version available: fall back to the files
        if (ImageData == nullptr)
        {
            ImageData = CoDAssets::GamePackageCache->ExtractPackageObject(ImageInfo.Mips.Levels[Fallback].HashID, ImageInfo.Mips.GetImageSize(Fallback), ResultSize);
            HighestIndex = Fallback;
        }

        Width = ImageInfo.Mips.Levels[HighestIndex].Width;
        Height = ImageInfo.Mips.Levels[HighestIndex].Height;
    }

    if (ImageData == nullptr)
        return nullptr;

    switch (ImageInfo.ImageFormat)
    {
    case 0:  ImageInfo.ImageFormat = 71; break;
    case 1:  ImageInfo.ImageFormat = 61; break;
    case 2:  ImageInfo.ImageFormat = 65; break;
    case 3:  ImageInfo.ImageFormat = 65; break;
    case 4:  ImageInfo.ImageFormat = 49; break;
    case 5:  ImageInfo.ImageFormat = 49; break;
    case 6:  ImageInfo.ImageFormat = 28; break;
    case 7:  ImageInfo.ImageFormat = 28; break;
    case 8:  ImageInfo.ImageFormat = 71; break;
    case 9:  ImageInfo.ImageFormat = 71; break;
    case 10: ImageInfo.ImageFormat = 63; break;
    case 11: ImageInfo.ImageFormat = 51; break;
    case 12: ImageInfo.ImageFormat = 56; break;
    case 13: ImageInfo.ImageFormat = 35; break;
    case 14: ImageInfo.ImageFormat = 11; break;
    case 15: ImageInfo.ImageFormat = 58; break;
    case 16: ImageInfo.ImageFormat = 54; break;
    case 17: ImageInfo.ImageFormat = 34; break;
    case 18: ImageInfo.ImageFormat = 10; break;
    case 19: ImageInfo.ImageFormat = 41; break;
    case 20: ImageInfo.ImageFormat = 16; break;
    case 21: ImageInfo.ImageFormat = 2;  break;
    case 22: ImageInfo.ImageFormat = 40; break;
    case 23: ImageInfo.ImageFormat = 20; break;
    case 24: ImageInfo.ImageFormat = 62; break;
    case 25: ImageInfo.ImageFormat = 57; break;
    case 26: ImageInfo.ImageFormat = 42; break;
    case 27: ImageInfo.ImageFormat = 17; break;
    case 28: ImageInfo.ImageFormat = 3;  break;
    case 29: ImageInfo.ImageFormat = 25; break;
    case 30: ImageInfo.ImageFormat = 85; break;
    case 31: ImageInfo.ImageFormat = 85; break;
    case 32: ImageInfo.ImageFormat = 24; break;
    case 33: ImageInfo.ImageFormat = 67; break;
    case 34: ImageInfo.ImageFormat = 67; break;
    case 35: ImageInfo.ImageFormat = 71; break;
    case 36: ImageInfo.ImageFormat = 71; break;
    case 37: ImageInfo.ImageFormat = 74; break;
    case 38: ImageInfo.ImageFormat = 74; break;
    case 39: ImageInfo.ImageFormat = 77; break;
    case 40: ImageInfo.ImageFormat = 77; break;
    case 41: ImageInfo.ImageFormat = 80; break;
    case 42: ImageInfo.ImageFormat = 84; break;
    case 43: ImageInfo.ImageFormat = 84; break;
    case 44: ImageInfo.ImageFormat = 95; break;
    case 45: ImageInfo.ImageFormat = 96; break;
    case 46: ImageInfo.ImageFormat = 98; break;
    case 47: ImageInfo.ImageFormat = 98; break;
    default: ImageInfo.ImageFormat = 71; break;
    }

    auto Result = CoDRawImageTranslator::TranslateBC(ImageData, ResultSize, Width, Height, ImageInfo.ImageFormat);
    return Result;
}

void GameModernWarfare4::LoadXModel(const XModelLod_t& ModelLOD, const std::unique_ptr<WraithModel>& ResultModel)
{
    bool ExportColors = (SettingsManager::GetSetting("exportvtxcolor", "true") == "true");
    auto MeshInfo = CoDAssets::GameInstance->Read<MW4XModelMesh>(ModelLOD.LODStreamInfoPtr);
    auto BufferInfo = CoDAssets::GameInstance->Read<MW4XModelMeshBufferInfo>(MeshInfo.MeshBufferPointer);

    // Even a streamed mesh can be in memory if loaded
    const auto Mesh = BufferInfo.BufferPtr != 0 ? MeshDecode::FromZone(BufferInfo.BufferPtr, BufferInfo.BufferSize)
                                                : MeshDecode::FromPackage(MeshInfo.LODStreamKey, BufferInfo.BufferSize);

    // 21-bit positions, packed quaternion normals, half UVs, u16 faces; rigid runs of 8 bytes, the
    // skinned weights in the zone
    constexpr MeshDecode::LodFormat Format{ { MeshDecode::PositionCodec::Packed21, MeshDecode::NormalCodec::PackedQuat, MeshDecode::UvCodec::Half2,
                                              MeshDecode::FaceCodec::U16 }, { 8, 2 }, false };
    if (Mesh.Data != nullptr)
        MeshDecode::DecodeLod(Mesh.View(), ModelLOD, Format, ExportColors, *ResultModel);
}
