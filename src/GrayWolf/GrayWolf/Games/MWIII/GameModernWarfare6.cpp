#include "stdafx.h"

#include "GameModernWarfare6.h"
#include "GameModernWarfare6Structures.h"

#include "CoDAssets.h"
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

struct MW6XAssetPoolData
{
    uint64_t PoolPtr;

    uint64_t PoolFreeHeadPtr;

    uint32_t PoolSize;

    uint32_t AssetSize;
};

uint32_t MW6HashSoundString(const std::string& Value)
{
    uint32_t Result = 5381;

    for (auto& Character : Value)
        Result = (uint32_t)(tolower(Character) + (Result << 6) + (Result << 16)) - Result;

    return Result;
}

static_assert(sizeof(MW6XAssetPoolData) == 0x18, "Invalid Pool Data Size (Expected 0x18)");

static MW6XModel ModelFromMP(uint64_t Address);

bool GameModernWarfare6::LoadAssets()
{
    bool NeedsAnims     = (SettingsManager::GetSetting("showxanim", "true") == "true");
    bool NeedsModels    = (SettingsManager::GetSetting("showxmodel", "true") == "true");
    bool NeedsImages    = (SettingsManager::GetSetting("showximage", "false") == "true");
    bool NeedsSounds    = (SettingsManager::GetSetting("showxsounds", "false") == "true");
    bool NeedsMaterials = (SettingsManager::GetSetting("showxmtl", "false") == "true");

    if (NeedsModels)
    {
        auto RemoveMdlBasename = SettingsManager::GetSetting("remove_mdl_basename", "false") == "true";
        auto Pool = CoDAssets::GameInstance->Read<ps::XAssetPool64>(ps::state->PoolsAddress + 9 * sizeof(ps::XAssetPool64));
        ps::PoolParser64(Pool.Root, CoDAssets::ParasyteRequest, [&RemoveMdlBasename](ps::XAsset64& Asset)
        {
            auto ModelResult = CoDAssets::GameFlags == SupportedGameFlags::SP ? CoDAssets::GameInstance->Read<MW6XModel>(Asset.Header)
                                                                               : ModelFromMP(Asset.Header);

            std::string ModelName;

            if (ModelResult.NamePtr > 0)
            {
                ModelName = CoDAssets::GameInstance->ReadNullTerminatedString(ModelResult.NamePtr);

                if (RemoveMdlBasename)
                {
                    const size_t DoubleColonPos = ModelName.find("::");
                    if (DoubleColonPos != std::string::npos)
                    {
                        ModelName = ModelName.substr(0, DoubleColonPos);
                    }
                }
                else
                {
                    ModelName = Strings::Replace(ModelName, "::", "_");
                }

                ModelName = FileSystems::GetFileName(ModelName);
            }
            else
            {
                ModelName = CoDAssets::GetHashedName("xmodel", ModelResult.Hash);
            }

            auto LoadedModel = new CoDModel_t();
            LoadedModel->AssetName = ModelName;
            LoadedModel->AssetPointer = Asset.Header;
            // Some models have thousands of bones with no info (no parent list): no bones
            if ((ModelResult.NumBones + ModelResult.UnkBoneCount) > 1 && ModelResult.ParentListPtr == 0)
                LoadedModel->BoneCount = 0;
            else
                LoadedModel->BoneCount = ModelResult.NumBones + ModelResult.UnkBoneCount;
            LoadedModel->LodCount = ModelResult.NumLods;
            LoadedModel->AssetStatus = Asset.Temp == 1 ? WraithAssetStatus::Placeholder : WraithAssetStatus::Loaded;
            CoDAssets::GameAssets->LoadedAssets.push_back(LoadedModel);
        });
    }

    if (NeedsImages)
    {
        auto Pool = CoDAssets::GameInstance->Read<ps::XAssetPool64>(ps::state->PoolsAddress + 21 * sizeof(ps::XAssetPool64));
        ps::PoolParser64(Pool.Root, CoDAssets::ParasyteRequest, [](ps::XAsset64& Asset)
        {
            auto ImageResult = CoDAssets::GameInstance->Read<MW6GfxImage>(Asset.Header);
            // Mask the name as hashes are 60Bit (Actually 63Bit but maintain with our existing tables)
            ImageResult.Hash &= 0xFFFFFFFFFFFFFFF;
            auto ImageName = CoDAssets::GetHashedName("ximage", ImageResult.Hash);
            auto LoadedImage = new CoDImage_t();
            LoadedImage->AssetName = ImageName;
            LoadedImage->AssetPointer = Asset.Header;
            LoadedImage->Width = ImageResult.Width;
            LoadedImage->Height = ImageResult.Height;
            LoadedImage->Format = ImageResult.ImageFormat;
            LoadedImage->AssetStatus = WraithAssetStatus::Loaded;
            LoadedImage->Streamed = ImageResult.LoadedImagePtr == 0;
            CoDAssets::GameAssets->LoadedAssets.push_back(LoadedImage);
        });
    }

    if (NeedsAnims)
    {
        auto Pool = CoDAssets::GameInstance->Read<ps::XAssetPool64>(ps::state->PoolsAddress + 7 * sizeof(ps::XAssetPool64));
        ps::PoolParser64(Pool.Root, CoDAssets::ParasyteRequest, [](ps::XAsset64& Asset)
        {
            auto AnimResult = CoDAssets::GameInstance->Read<MW6XAnim>(Asset.Header);
            auto AnimName = CoDAssets::GetHashedName("xanim", AnimResult.Hash);

            auto LoadedAnim = new CoDAnim_t();
            LoadedAnim->AssetName = AnimName;
            LoadedAnim->AssetPointer = Asset.Header;
            LoadedAnim->Framerate = AnimResult.Framerate;
            LoadedAnim->FrameCount = AnimResult.FrameCount;
            LoadedAnim->AssetStatus = Asset.Temp == 1 ? WraithAssetStatus::Placeholder : WraithAssetStatus::Loaded;
            LoadedAnim->BoneCount = AnimResult.TotalBoneCount;
            CoDAssets::GameAssets->LoadedAssets.push_back(LoadedAnim);
        });
    }

    if (NeedsMaterials)
    {
        auto Pool = CoDAssets::GameInstance->Read<ps::XAssetPool64>(ps::state->PoolsAddress + 11 * sizeof(ps::XAssetPool64));
        ps::PoolParser64(Pool.Root, CoDAssets::ParasyteRequest, [](ps::XAsset64& Asset)
        {
            auto MatResult = CoDAssets::GameInstance->Read<MW6XMaterial>(Asset.Header);
            auto MaterialName = CoDAssets::GetHashedName("xmaterial", MatResult.Hash);

            auto LoadedMaterial = new CoDMaterial_t();
            LoadedMaterial->AssetName = FileSystems::GetFileName(MaterialName);
            LoadedMaterial->AssetPointer = Asset.Header;
            LoadedMaterial->ImageCount = MatResult.ImageCount;
            LoadedMaterial->AssetStatus = WraithAssetStatus::Loaded;

            CoDAssets::GameAssets->LoadedAssets.push_back(LoadedMaterial);
        });
    }

    if (NeedsSounds)
    {
        auto Pool = CoDAssets::GameInstance->Read<ps::XAssetPool64>(ps::state->PoolsAddress + 0xc1 * sizeof(ps::XAssetPool64));
        ps::PoolParser64(Pool.Root, CoDAssets::ParasyteRequest, [](ps::XAsset64& Asset)
        {
            auto SoundResult = CoDAssets::GameInstance->Read<MW6SndAsset>(Asset.Header);
            // Mask the name as hashes are 60Bit (Actually 63Bit but maintain with our existing tables)
            SoundResult.Name &= 0xFFFFFFFFFFFFFFF;
            auto SoundName = CoDAssets::GetHashedName("xsound", SoundResult.Name);

            auto LoadedSound = new CoDSound_t();
            LoadedSound->AssetName = FileSystems::GetFileNamePurgeExtensions(SoundName);
            LoadedSound->FullPath = FileSystems::GetDirectoryName(SoundName);
            LoadedSound->AssetPointer = Asset.Header;
            LoadedSound->AssetStatus = WraithAssetStatus::Loaded;
            LoadedSound->FrameRate = SoundResult.FrameRate;
            LoadedSound->FrameCount = SoundResult.FrameCount;
            LoadedSound->ChannelsCount = SoundResult.ChannelCount;
            LoadedSound->AssetSize = -1;
            LoadedSound->AssetStatus = WraithAssetStatus::Loaded;
            LoadedSound->IsFileEntry = false;
            LoadedSound->Length = (uint32_t)(1000.0f * (float)(LoadedSound->FrameCount / (float)(LoadedSound->FrameRate)));
            CoDAssets::GameAssets->LoadedAssets.push_back(LoadedSound);
        });
    }

    return true;
}

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

        ModelAsset->ModelName = Model->AssetName;

        // Some models have thousands of bones with no info (no parent list): no bones
        if ((ModelData.NumBones + ModelData.UnkBoneCount) > 1 && ModelData.ParentListPtr == 0)
        {
            ModelAsset->BoneCount = 0;
            ModelAsset->RootBoneCount = 0;
        }
        else
        {
            ModelAsset->BoneCount = ModelData.NumBones + ModelData.UnkBoneCount;
            ModelAsset->RootBoneCount = ModelData.NumRootBones;
        }

        ModelAsset->BoneRotationData = BoneDataTypes::DivideBySize;

        ModelAsset->IsModelStreamed = true;

        ModelAsset->BoneIDsPtr = ModelData.BoneIDsPtr;
        ModelAsset->BoneIndexSize = 4;

        ModelAsset->BoneParentsPtr = ModelData.ParentListPtr;
        ModelAsset->BoneParentSize = 2;

        ModelAsset->RotationsPtr = ModelData.RotationsPtr;
        ModelAsset->TranslationsPtr = ModelData.TranslationsPtr;

        ModelAsset->BaseMatriciesPtr = ModelData.BaseMatriciesPtr;

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

            auto XSurfacePtr = ModelLod.SurfsPtr;

            for (uint32_t s = 0; s < ModelLod.NumSurfs; s++)
            {
                LodReference.Submeshes.emplace_back();
                auto& SubmeshReference = LodReference.Submeshes[s];

                auto SurfaceInfo = CoDAssets::GameInstance->Read<MW6XModelSurface>(XSurfacePtr);

                SubmeshReference.RigidWeightsPtr              = SurfaceInfo.RigidWeightsPtr;
                SubmeshReference.VertListcount                = SurfaceInfo.VertListCount;
                SubmeshReference.VertexCount                  = SurfaceInfo.VertexCount;
                SubmeshReference.FaceCount                    = SurfaceInfo.FacesCount;
                SubmeshReference.PackedIndexTableCount        = SurfaceInfo.PackedIndexTableCount;
                SubmeshReference.VertexPtr                    = SurfaceInfo.Offsets[0];
                SubmeshReference.VertexUVsPtr                 = SurfaceInfo.Offsets[1];
                SubmeshReference.VertexNormalsPtr             = SurfaceInfo.Offsets[2];
                SubmeshReference.FacesPtr                     = SurfaceInfo.Offsets[3];
                SubmeshReference.PackedIndexTablePtr          = SurfaceInfo.Offsets[4];
                SubmeshReference.PackedIndexBufferPtr         = SurfaceInfo.Offsets[5];
                SubmeshReference.VertexColorPtr               = SurfaceInfo.Offsets[6];
                SubmeshReference.WeightsPtr                   = SurfaceInfo.Offsets[11];

                if (SurfaceInfo.NewScale != -1)
                {
                    SubmeshReference.Scale = SurfaceInfo.NewScale;
                    SubmeshReference.XOffset = 0;
                    SubmeshReference.YOffset = 0;
                    SubmeshReference.ZOffset = 0;
                }
                else
                {
                    SubmeshReference.Scale = fmaxf(fmaxf(SurfaceInfo.Min, SurfaceInfo.Scale), SurfaceInfo.Max);
                    SubmeshReference.XOffset = SurfaceInfo.XOffset;
                    SubmeshReference.YOffset = SurfaceInfo.YOffset;
                    SubmeshReference.ZOffset = SurfaceInfo.ZOffset;
                }

                SubmeshReference.WeightCounts[0] = SurfaceInfo.WeightCounts[0];
                SubmeshReference.WeightCounts[1] = SurfaceInfo.WeightCounts[1];
                SubmeshReference.WeightCounts[2] = SurfaceInfo.WeightCounts[2];
                SubmeshReference.WeightCounts[3] = SurfaceInfo.WeightCounts[3];
                SubmeshReference.WeightCounts[4] = SurfaceInfo.WeightCounts[4];
                SubmeshReference.WeightCounts[5] = SurfaceInfo.WeightCounts[5];
                SubmeshReference.WeightCounts[6] = SurfaceInfo.WeightCounts[6];
                SubmeshReference.WeightCounts[7] = SurfaceInfo.WeightCounts[7];

                auto MaterialHandle = CoDAssets::GameInstance->Read<uint64_t>(ModelData.MaterialHandlesPtr);
                LodReference.Materials.emplace_back(ReadXMaterial(MaterialHandle));

                XSurfacePtr += sizeof(MW6XModelSurface);
                ModelData.MaterialHandlesPtr += sizeof(uint64_t);
            }
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

    if (SoundData.StreamKey != 0)
    {
        std::unique_ptr<uint8_t[]> SoundBuffer = nullptr;
        // Pad the audio size: the sound asset's size is literal, XPAK data is padded
        uint32_t SoundMemoryResult = 0;
        SoundBuffer = CoDAssets::GamePackageCache->ExtractPackageObject(SoundData.StreamKey, (int32_t)(((uint64_t)SoundData.Size + (size_t)SoundData.SeekTableSize + 4095) & 0xFFFFFFFFFFFFFFF0), SoundMemoryResult);

        if (SoundMemoryResult == 0)
            return nullptr;

        if (SoundBuffer == nullptr)
            return nullptr;

        return SABSupport::OpusPackets(SoundBuffer.get(), SoundMemoryResult, SoundData.SeekTableSize, (size_t)SoundMemoryResult - SoundData.SeekTableSize, SoundData.FrameRate, SoundData.ChannelCount, SoundData.FrameCount, SABSupport::OpusPreSkipPacket);
    }
    else
    {
        std::unique_ptr<uint8_t[]> SoundBuffer = nullptr;
        // Pad the audio size: the sound asset's size is literal, XPAK data is padded
        uint32_t SoundMemoryResult = 0;
        SoundBuffer = CoDAssets::GamePackageCache->ExtractPackageObject(SoundData.StreamKeyEx, (int32_t)(((uint64_t)SoundData.LoadedSize + (size_t)SoundData.SeekTableSize + 4095) & 0xFFFFFFFFFFFFFFF0), SoundMemoryResult);

        if (SoundMemoryResult == 0)
            return nullptr;

        if (SoundBuffer == nullptr)
            return nullptr;

        return SABSupport::OpusPackets(SoundBuffer.get(), SoundMemoryResult, 32 + (size_t)SoundData.SeekTableSize, (size_t)SoundMemoryResult - 32 - SoundData.SeekTableSize, SoundData.FrameRate, SoundData.ChannelCount, SoundData.FrameCount, SABSupport::OpusPreSkipPacket);
    }

}

void GameModernWarfare6::TranslateRawfile(const CoDRawFile_t * Rawfile, const std::string & ExportPath)
{
    std::error_code Ec;
    std::string ExportFolder = ExportPath;

    if (SettingsManager::GetSetting("keeprawpath", "true") == "true" && !Rawfile->RawFilePath.empty())
    {
        ExportFolder = (std::filesystem::path(ExportFolder) / Rawfile->RawFilePath).string();
    }

    std::filesystem::create_directories(ExportFolder, Ec);

    auto Bank = CoDAssets::GameInstance->Read<MW6SoundBank>(Rawfile->AssetPointer);

    // Always stream: the data in memory is unreliable
    uint32_t ResultSize = 0;
    std::unique_ptr<uint8_t[]> Data = CoDAssets::GamePackageCache->ExtractPackageObject(Bank.StreamKey, Bank.SoundBankSize, ResultSize);

    if (Data != nullptr)
    {
        auto Writer = BinaryWriter();
        if (Writer.Create((std::filesystem::path(ExportFolder) / Rawfile->AssetName).string()))
        {
            Writer.Write(Data.get(), (uint32_t)Bank.SoundBankSize);
        }
    }
}

const XMaterial_t GameModernWarfare6::ReadXMaterial(uint64_t MaterialPointer)
{
    uint64_t Hash;
    uint32_t ImageCount;
    uint64_t ImageTablePtr;

    if (CoDAssets::GameFlags == SupportedGameFlags::SP)
    {
        const auto MaterialData = CoDAssets::GameInstance->Read<MW6XMaterialSP>(MaterialPointer);
        Hash = MaterialData.Hash;
        ImageCount = MaterialData.ImageCount;
        ImageTablePtr = MaterialData.ImageTablePtr;
    }
    else
    {
        // the multiplayer build (0x78): slots {u8 semantic, u8 index into the image pointers}
        // (C2M's semantics: 0-3 colorMap, 4-7 nogMap, 12-15 opacityMap, 16-19 revealMap)
        const auto MaterialData = CoDAssets::GameInstance->Read<MW6XMaterialMP>(MaterialPointer);
        XMaterial_t Result(MaterialData.ImageCount);
        Result.MaterialName = CoDAssets::GetHashedName("xmaterial", MaterialData.Hash);
        for (uint32_t m = 0; m < MaterialData.ImageCount; m++)
        {
            const auto Slot = CoDAssets::GameInstance->Read<uint16_t>(MaterialData.ImageSlotsPtr + m * 2);
            const uint8_t Semantic = (uint8_t)(Slot & 0xff), Index = (uint8_t)(Slot >> 8);
            if (Index >= MaterialData.ImageTableCount) continue;
            const uint64_t ImagePtr = CoDAssets::GameInstance->Read<uint64_t>(MaterialData.ImageTablePtr + (uint64_t)Index * 8);
            if (ImagePtr == 0) continue;
            const auto ImageName = CoDAssets::GetHashedName("ximage", CoDAssets::GameInstance->Read<uint64_t>(ImagePtr) & 0x0FFFFFFFFFFFFFFFull);
            const ImageUsageType Usage = Semantic < 4 ? ImageUsageType::DiffuseMap
                                       : Semantic < 8 ? ImageUsageType::NormalMap
                                       : ImageUsageType::Unknown;
            Result.Images.emplace_back(Usage, Semantic, ImagePtr, ImageName);
        }
        return Result;
    }

    XMaterial_t Result(ImageCount);
    Result.MaterialName = CoDAssets::GetHashedName("xmaterial", Hash);

    for (uint32_t m = 0; m < ImageCount; m++)
    {
        auto ImageInfo = CoDAssets::GameInstance->Read<MW6XMaterialImage>(ImageTablePtr);
        // Image name pointer: end of image - 8
        auto ImageName = CoDAssets::GetHashedName("ximage", CoDAssets::GameInstance->Read<uint64_t>(ImageInfo.ImagePtr));

        auto DefaultUsage = ImageUsageType::Unknown;
        switch (ImageInfo.Type)
        {
            case 0:
                DefaultUsage = ImageUsageType::DiffuseMap;
            break;
        }

        Result.Images.emplace_back(DefaultUsage, ImageInfo.Type, ImageInfo.ImagePtr, ImageName);

        ImageTablePtr += sizeof(MW6XMaterialImage);
    }

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

std::string GameModernWarfare6::LoadStringEntry(uint64_t Index)
{
    return CoDAssets::GameInstance->ReadNullTerminatedString(ps::state->StringsAddress + Index);
}

void GameModernWarfare6::PerformInitialSetup()
{
    std::error_code Ec;
    const auto OurPath = (std::filesystem::path(FileSystems::GetApplicationPath()) / "oo2core_8_win64.dll").string();

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

    if (!std::filesystem::is_regular_file(OurPath, Ec))
    {
        const auto SteamDLL = (std::filesystem::path(ps::state->GameDirectory) / "oo2core_8_win64.dll").string();
        if (std::filesystem::is_regular_file(SteamDLL, Ec))
        {
            std::filesystem::copy_file(SteamDLL, OurPath, std::filesystem::copy_options::overwrite_existing, Ec);
        }
        else
        {
            std::filesystem::copy_file((std::filesystem::path(ps::state->GameDirectory) / "_retail_\\oo2core_8_win64.dll").string(), OurPath, std::filesystem::copy_options::overwrite_existing, Ec);
        }
    }
}

void GameModernWarfare6::PerformShutDown()
{
}
