#include "stdafx.h"

#include "GameModernWarfare6.h"
#include "GameModernWarfare6Structures.h"

#include "CoDAssets.h"
#include "GameReaders.h"
#include "ModernIwReaders.h"
#include "PackedXAnim.h"
#include "CoDXModelBonesHelper.h"
#include "Strings.h"
#include "FileSystems.h"
#include "SettingsManager.h"
#include "Sound.h"

#include "SABSupport.h"

#include "Games/Common/MeshDecode.h"

#include <filesystem>
#include <format>

std::unique_ptr<XAnim_t> GameModernWarfare6::ReadXAnim(const CoDAnim_t* Animation)
{
    return PackedXAnim::Read<MW6XAnim, MW6XAnimDeltaParts, MW6XAnimNotetrack>(Animation, LoadXAnim);
}

// The multiplayer build's XModel (0xe8) as the campaign one's: its skeleton fields from the packed
// data at 0x70, the LODs and material slots where it keeps them
static MW6XModel ModelFromMP(uint64_t Address)
{
    const auto Mp = CoDAssets::GameInstance->Read<MW6XModelMP>(Address);
    MW6XModel M{};
    M.Hash = Mp.Hash;
    M.NamePtr = Mp.NamePtr;
    M.NumSurfaces = Mp.NumSurfaces;
    M.NumLods = Mp.NumLods;
    M.MaxLods = Mp.MaxLods;
    if (Mp.PackedDataPtr != 0)
    {
        const auto P = CoDAssets::GameInstance->Read<MW6XModelPackedData>(Mp.PackedDataPtr);
        M.NumBones = P.NumBones;
        M.NumRootBones = P.NumRootBones;
        M.UnkBoneCount = P.UnkBoneCount;
        M.BoneIDsPtr = P.BoneIDsPtr;
        M.ParentListPtr = P.ParentListPtr;
        M.RotationsPtr = P.RotationsPtr;
        M.TranslationsPtr = P.TranslationsPtr;
        M.PartClassificationPtr = P.PartClassificationPtr;
        M.BaseMatriciesPtr = P.BaseMatriciesPtr;
    }
    M.MaterialHandlesPtr = Mp.MaterialHandlesPtr;
    M.ModelLods = Mp.ModelLods;
    return M;
}

std::unique_ptr<XModel_t> GameModernWarfare6::ReadXModel(const CoDModel_t* Model)
{
    if (CoDAssets::GameInstance->IsRunning())
    {
        auto ModelData = CoDAssets::GameFlags == SupportedGameFlags::SP ? CoDAssets::GameInstance->Read<MW6XModel>(Model->AssetPointer)
                                                                         : ModelFromMP(Model->AssetPointer);

        auto ModelAsset = std::make_unique<XModel_t>(ModelData.NumLods);

        GameReaders::CopyStreamedBones(*ModelAsset, Model->AssetName, ModelData, 2);

        for (uint32_t i = 0; i < ModelData.NumLods; i++)
        {
            auto ModelLod = CoDAssets::GameInstance->Read<MW6XModelLod>(ModelData.ModelLods + i * sizeof(MW6XModelLod));
            // a LOD whose xmodelsurfs another zone defines (a name-only header, no surfaces) is
            // left out; its material slots are still skipped
            if (ModelLod.MeshPtr == 0 || CoDAssets::GameInstance->Read<MW6XModelMesh>(ModelLod.MeshPtr).SurfsPtr == 0)
            {
                ModelData.MaterialHandlesPtr += (uint64_t)ModelLod.NumSurfs * sizeof(uint64_t);
                continue;
            }
            ModelAsset->ModelLods.emplace_back(ModelLod.NumSurfs);
            auto& LodReference = ModelAsset->ModelLods.back();

            // Set distance (use index, "distance" we see may be bounds? doesn't line up)
            LodReference.LodDistance = i;

            LodReference.LODStreamInfoPtr = ModelLod.MeshPtr;

            GameReaders::ReadSurfaces<MW6XModelSurface, uint64_t>(LodReference, ModelLod.SurfsPtr, ModelLod.NumSurfs, ModelData.MaterialHandlesPtr, ReadXMaterial,
                                                                  [](XModelSubmesh_t& Submesh, const MW6XModelSurface& Info)
                                                                  {
                                                                      Submesh.RigidWeightsPtr = Info.RigidWeightsPtr;
                                                                      Submesh.VertListcount = Info.VertListCount;
                                                                      Submesh.VertexCount = Info.VertexCount;
                                                                      Submesh.FaceCount = Info.FacesCount;
                                                                      Submesh.PackedIndexTableCount = Info.PackedIndexTableCount;
                                                                      Submesh.VertexPtr = Info.Offsets[0];
                                                                      Submesh.VertexUVsPtr = Info.Offsets[1];
                                                                      Submesh.VertexNormalsPtr = Info.Offsets[2];
                                                                      Submesh.FacesPtr = Info.Offsets[3];
                                                                      Submesh.PackedIndexTablePtr = Info.Offsets[4];
                                                                      Submesh.PackedIndexBufferPtr = Info.Offsets[5];
                                                                      Submesh.VertexColorPtr = Info.Offsets[6];
                                                                      Submesh.WeightsPtr = Info.Offsets[11];
                                                                      // a new scale: the positions unoffset
                                                                      const bool New = Info.NewScale != -1;
                                                                      Submesh.Scale = New ? Info.NewScale : fmaxf(fmaxf(Info.Min, Info.Scale), Info.Max);
                                                                      Submesh.XOffset = New ? 0 : Info.XOffset;
                                                                      Submesh.YOffset = New ? 0 : Info.YOffset;
                                                                      Submesh.ZOffset = New ? 0 : Info.ZOffset;
                                                                      for (int W = 0; W < 8; W++) Submesh.WeightCounts[W] = Info.WeightCounts[W];
                                                                  });
        }

        return ModelAsset;
    }
    return nullptr;
}

std::unique_ptr<XImageDDS> GameModernWarfare6::ReadXImage(const CoDImage_t* Image)
{
    return LoadXImage(XImage_t(ImageUsageType::DiffuseMap, 0, Image->AssetPointer, Image->AssetName));
}

std::unique_ptr<XSound> GameModernWarfare6::ReadXSound(const CoDSound_t* Sound)
{
    auto SoundData = CoDAssets::GameInstance->Read<MW6SndAsset>(Sound->AssetPointer);

    // a streamed sound, else the loaded one in the packages (its 32-byte header before the seek table)
    const bool Streamed = SoundData.StreamKey != 0;
    const uint64_t Key = Streamed ? SoundData.StreamKey : SoundData.StreamKeyEx;
    const uint32_t Size = Streamed ? SoundData.Size : SoundData.LoadedSize;
    const size_t Header = Streamed ? 0 : 32;
    // Pad the audio size: the sound asset's size is literal, XPAK data is padded
    uint32_t SoundMemoryResult = 0;
    const auto SoundBuffer = CoDAssets::GamePackageCache->ExtractPackageObject(Key, (int32_t)(((uint64_t)Size + (size_t)SoundData.SeekTableSize + 4095) & 0xFFFFFFFFFFFFFFF0), SoundMemoryResult);

    if (SoundMemoryResult == 0 || SoundBuffer == nullptr)
        return nullptr;

    return SABSupport::OpusPackets(SoundBuffer.get(), SoundMemoryResult, Header + (size_t)SoundData.SeekTableSize, (size_t)SoundMemoryResult - Header - SoundData.SeekTableSize, SoundData.FrameRate, SoundData.ChannelCount, SoundData.FrameCount, SABSupport::OpusPreSkipPacket);
}

thread_local GameReaders::ImagesElsewhereFn GameModernWarfare6::ImagesElsewhere;

const XMaterial_t GameModernWarfare6::ReadXMaterial(uint64_t MaterialPointer)
{
    uint64_t Hash;
    uint32_t ImageCount;
    uint64_t ImageTablePtr;
    bool Header = false;

    if (CoDAssets::GameFlags == SupportedGameFlags::SP)
    {
        const auto MaterialData = CoDAssets::GameInstance->Read<MW6XMaterialSP>(MaterialPointer);
        Hash = MaterialData.Hash;
        ImageCount = MaterialData.ImageCount;
        ImageTablePtr = MaterialData.ImageTablePtr;
        Header = MaterialData.ImageCount == 0 && MaterialData.TechsetPtr == 0;
    }
    else
    {
        // the multiplayer build (0x78): slots {u8 semantic, u8 index into the image pointers}
        // (C2M's semantics: 0-3 colorMap, 4-7 nogMap, 12-15 opacityMap, 16-19 revealMap)
        const auto MaterialData = CoDAssets::GameInstance->Read<MW6XMaterialMP>(MaterialPointer);
        XMaterial_t Result(MaterialData.ImageCount);
        Result.MaterialName = CoDAssets::GetHashedName("xmaterial", MaterialData.Hash);
        const auto UsageOf = [](uint32_t Semantic) {
            return Semantic < 4 ? ImageUsageType::DiffuseMap : Semantic < 8 ? ImageUsageType::NormalMap : ImageUsageType::Unknown;
        };
        // a header (a transient zone's handle to a material another zone defines): that zone's images
        if (MaterialData.ImageCount == 0 && MaterialData.TechsetPtr == 0)
            GameReaders::AddImagesElsewhere(Result, ImagesElsewhere, std::format("{:x}", MaterialData.Hash & 0x0FFFFFFFFFFFFFFFull), UsageOf);
        for (uint32_t m = 0; m < MaterialData.ImageCount; m++)
        {
            const auto Slot = CoDAssets::GameInstance->Read<uint16_t>(MaterialData.ImageSlotsPtr + m * 2);
            const uint8_t Semantic = (uint8_t)(Slot & 0xff), Index = (uint8_t)(Slot >> 8);
            if (Index >= MaterialData.ImageTableCount) continue;
            const uint64_t ImagePtr = CoDAssets::GameInstance->Read<uint64_t>(MaterialData.ImageTablePtr + (uint64_t)Index * 8);
            if (ImagePtr == 0) continue;
            const auto ImageName = CoDAssets::GetHashedName("ximage", CoDAssets::GameInstance->Read<uint64_t>(ImagePtr) & 0x0FFFFFFFFFFFFFFFull);
            Result.Images.emplace_back(UsageOf(Semantic), Semantic, ImagePtr, ImageName);
        }
        return Result;
    }

    XMaterial_t Result(ImageCount);
    Result.MaterialName = CoDAssets::GetHashedName("xmaterial", Hash);

    // an image named by its hash first in it; type 0 the colour map
    const auto UsageOf = [](uint32_t Semantic) { return Semantic == 0 ? ImageUsageType::DiffuseMap : ImageUsageType::Unknown; };
    GameReaders::AddMaterialImages<MW6XMaterialImage>(
        Result, ImageTablePtr, ImageCount, [](uint64_t ImagePtr) { return CoDAssets::GetHashedName("ximage", CoDAssets::GameInstance->Read<uint64_t>(ImagePtr)); },
        UsageOf);
    // a header (a transient zone's handle to a material another zone defines): that zone's images
    if (Header) GameReaders::AddImagesElsewhere(Result, ImagesElsewhere, std::format("{:x}", Hash & 0x0FFFFFFFFFFFFFFFull), UsageOf);

    return Result;
}

std::unique_ptr<XImageDDS> GameModernWarfare6::LoadXImage(const XImage_t& Image)
{
    return ModernIwReaders::LoadMipImage<MW6GfxImage, MW6GfxMipArray<32>>(Image, MW6DXGIFormats);
}

void GameModernWarfare6::LoadXModel(const std::unique_ptr<XModel_t>& Model, const XModelLod_t& ModelLOD, const std::unique_ptr<WraithModel>& ResultModel)
{
    bool ExportColors = (SettingsManager::GetSetting("exportvtxcolor", "true") == "true");
    auto MeshInfo = CoDAssets::GameInstance->Read<MW6XModelMesh>(ModelLOD.LODStreamInfoPtr);

    // the multiplayer build keeps the LOD's buffer record at its surfaces' 0x78 (every surface
    // names the same one), xmodelsurfs' 0x20 being something else there
    uint64_t BufferInfoPtr = MeshInfo.MeshBufferPointer;
    if (CoDAssets::GameFlags != SupportedGameFlags::SP && MeshInfo.SurfsPtr != 0)
        BufferInfoPtr = CoDAssets::GameInstance->Read<MW6XModelSurface>(MeshInfo.SurfsPtr).MeshBufferPointer;
    auto BufferInfo = CoDAssets::GameInstance->Read<MW6XModelMeshBufferInfo>(BufferInfoPtr);

    // Even a streamed mesh can be in memory if loaded
    const auto Mesh = BufferInfo.BufferPtr != 0 ? MeshDecode::FromZone(BufferInfo.BufferPtr, BufferInfo.BufferSize)
                                                : MeshDecode::FromPackage(MeshInfo.LODStreamKey, BufferInfo.BufferSize);

    CoDXModelBonesHelper::ReadXModelBones(Model, ModelLOD, ResultModel);

    // 21-bit positions, QTangents, half UVs, faces packed in 40-byte tables; rigid runs of 12 bytes
    constexpr MeshDecode::LodFormat Format{ { MeshDecode::PositionCodec::Packed21, MeshDecode::NormalCodec::QTangent, MeshDecode::UvCodec::Half2,
                                              MeshDecode::FaceCodec::PackedTable40 }, { 12, 2 } };
    if (Mesh.Data != nullptr)
        MeshDecode::DecodeLod(Mesh.View(), ModelLOD, Format, ExportColors, *ResultModel);
}

void GameModernWarfare6::LoadXAnim(const std::unique_ptr<XAnim_t>& Anim, std::unique_ptr<WraithAnim>& ResultAnim)
{
    PackedXAnim::Load<MW6XAnim, MW6XAnimStreamInfo>(Anim, ResultAnim);
}

void GameModernWarfare6::PerformInitialSetup()
{
    CoDAssets::StringCache.LoadIndex((std::filesystem::path(FileSystems::GetApplicationPath()) / "package_index\\fnv1a_string.wni").string());
    CoDAssets::StringCache.LoadIndex((std::filesystem::path(FileSystems::GetApplicationPath()) / "package_index\\fnv1a_bones.wni").string());

    CoDAssets::AssetNameCache.LoadIndex((std::filesystem::path(FileSystems::GetApplicationPath()) / "package_index\\fnv1a_v2_xanims.wni").string());
    CoDAssets::AssetNameCache.LoadIndex((std::filesystem::path(FileSystems::GetApplicationPath()) / "package_index\\fnv1a_v2_ximages.wni").string());
    CoDAssets::AssetNameCache.LoadIndex((std::filesystem::path(FileSystems::GetApplicationPath()) / "package_index\\fnv1a_v2_xmaterials.wni").string());

    CoDAssets::AssetNameCache.LoadIndex((std::filesystem::path(FileSystems::GetApplicationPath()) / "package_index\\fnv1a_v2_xanims_test.wni").string());

    CoDAssets::AssetNameCache.LoadIndex((std::filesystem::path(FileSystems::GetApplicationPath()) / "package_index\\fnv1a_xsounds_MW2022_Weapon.wni").string());

    CoDAssets::AssetNameCache.LoadIndex((std::filesystem::path(FileSystems::GetApplicationPath()) / "package_index\\fnv1a_ximages_mw5.wni").string());

    // For hashes testing
    CoDAssets::AssetNameCache.LoadIndex((std::filesystem::path(FileSystems::GetApplicationPath()) / "pkg_test\\fnv1a_xanims.wni").string());
    CoDAssets::AssetNameCache.LoadIndex((std::filesystem::path(FileSystems::GetApplicationPath()) / "pkg_test\\fnv1a_ximages.wni").string());
    CoDAssets::AssetNameCache.LoadIndex((std::filesystem::path(FileSystems::GetApplicationPath()) / "pkg_test\\fnv1a_xmaterials.wni").string());
    CoDAssets::AssetNameCache.LoadIndex((std::filesystem::path(FileSystems::GetApplicationPath()) / "pkg_test\\fnv1a_xsounds.wni").string());
}
