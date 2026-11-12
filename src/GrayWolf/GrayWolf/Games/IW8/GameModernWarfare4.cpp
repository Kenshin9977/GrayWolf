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

            GameReaders::ReadSurfaces<MW4XModelSurface, uint64_t>(LodReference, ModelData.ModelLods[i].SurfsPtr, ModelData.ModelLods[i].NumSurfs,
                                                                  ModelData.MaterialHandlesPtr, ReadXMaterial,
                                                                  [](XModelSubmesh_t& Submesh, const MW4XModelSurface& Info)
                                                                  {
                                                                      Submesh.RigidWeightsPtr = Info.RigidWeightsPtr;
                                                                      Submesh.VertListcount = Info.VertListCount;
                                                                      Submesh.VertexCount = Info.VertexCount;
                                                                      Submesh.FaceCount = Info.FacesCount;
                                                                      Submesh.VertexPtr = Info.Offsets[0];
                                                                      Submesh.VertexUVsPtr = Info.Offsets[2];
                                                                      Submesh.VertexNormalsPtr = Info.Offsets[3];
                                                                      Submesh.FacesPtr = Info.Offsets[4];
                                                                      Submesh.VertexColorPtr = Info.Offsets[6];
                                                                      Submesh.Scale = fmaxf(fmaxf(Info.Min, Info.Scale), Info.Max);
                                                                      Submesh.XOffset = Info.XOffset;
                                                                      Submesh.YOffset = Info.YOffset;
                                                                      Submesh.ZOffset = Info.ZOffset;
                                                                      for (int W = 0; W < 8; W++) Submesh.WeightCounts[W] = Info.WeightCounts[W];
                                                                      Submesh.WeightsPtr = Info.WeightsPtr;
                                                                  });
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

    // The game's own table, image format -> DXGI (iw8_flat.exe, the u32 array 0 61 65 61 49 49 28 29
    // 63 51 ... 71 72 74 75 77 78 80 83 84 95 96 98 99 31 at 0x73f2500). The hand-made switch it
    // replaces had the sRGB ones as UNORM and 42 (BC5 UNORM: ggx_env_brdf_lut, its R and G the
    // means AW's and MWR's RG16F ~envbrdflut_ggx hold, 203 and 27) as SNORM; its 0, 8 and 9 and any
    // other were BC1. A format CoDRawImageTranslator does not write is refused, not guessed.
    static constexpr uint8_t Dxgi[] = { 0,  61, 65, 61, 49, 49, 28, 29, 63, 51, 56, 35, 11, 58, 37, 13, 54, 34, 10, 41, 16, 2,  55, 40, 20,
                                        62, 57, 42, 17, 3,  25, 85, 24, 67, 26, 71, 72, 74, 75, 77, 78, 80, 83, 84, 95, 96, 98, 99, 31 };
    ImageInfo.ImageFormat = ImageInfo.ImageFormat < std::size(Dxgi) ? Dxgi[ImageInfo.ImageFormat] : 0;

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
