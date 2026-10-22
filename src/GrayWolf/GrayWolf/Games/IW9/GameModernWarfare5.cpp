#include "stdafx.h"

#include "GameModernWarfare5.h"
#include "GameModernWarfare5Structures.h"

#include "CoDAssets.h"
#include "ModernIwReaders.h"
#include "PackedXAnim.h"
#include "Strings.h"
#include "FileSystems.h"
#include "MemoryReader.h"
#include "SettingsManager.h"
#include "HalfFloats.h"
#include "Sound.h"

#include "CoDXModelBonesHelper.h"

#include "SABSupport.h"

#include "CoDQTangent.h"

#include <filesystem>

struct MW5GfxRigidVerts
{
    uint16_t BoneIndex;
    uint16_t VertexCount;
    uint16_t FacesCount;
    uint16_t FacesIndex;
    uint32_t Unk123;
};

#pragma pack(push, 1)
struct MW5GfxStreamVertex
{
    uint64_t PackedPosition; // Packed 21bits, scale + offset in Mesh Info
    uint32_t BiNormal;
    uint16_t UVUPosition;
    uint16_t UVVPosition;
    uint32_t NormalQuaternion;
};
#pragma pack(pop)

struct MW5GfxStreamFace
{
    uint16_t Index1;
    uint16_t Index2;
    uint16_t Index3;
};

struct MW5XAssetPoolData
{
    uint64_t PoolPtr;

    uint64_t PoolFreeHeadPtr;

    uint32_t PoolSize;

    uint32_t AssetSize;
};

uint32_t MW5HashSoundString(const std::string& Value)
{
    uint32_t Result = 5381;

    for (auto& Character : Value)
        Result = (uint32_t)(tolower(Character) + (Result << 6) + (Result << 16)) - Result;

    return Result;
}

static_assert(sizeof(MW5XAssetPoolData) == 0x18, "Invalid Pool Data Size (Expected 0x18)");

bool GameModernWarfare5::LoadAssets()
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
            auto ModelResult = CoDAssets::GameInstance->Read<MW5XModel>(Asset.Header);

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
        auto Pool = CoDAssets::GameInstance->Read<ps::XAssetPool64>(ps::state->PoolsAddress + 19 * sizeof(ps::XAssetPool64));
        ps::PoolParser64(Pool.Root, CoDAssets::ParasyteRequest, [](ps::XAsset64& Asset)
        {
            auto ImageResult = CoDAssets::GameInstance->Read<MW5GfxImage>(Asset.Header);
            // Mask the name as hashes are 60Bit (Actually 63Bit but maintain with our existing tables)
            ImageResult.Hash &= 0xFFFFFFFFFFFFFFF;
            auto ImageName = CoDAssets::GetHashedName("ximage", ImageResult.Hash);
            auto LoadedImage = new CoDImage_t();
            LoadedImage->AssetName = ImageName;
            LoadedImage->AssetPointer = Asset.Header;
            LoadedImage->Width = (uint16_t)ImageResult.Width;
            LoadedImage->Height = (uint16_t)ImageResult.Height;
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
            auto AnimResult = CoDAssets::GameInstance->Read<MW5XAnim>(Asset.Header);
            auto AnimName = CoDAssets::GameInstance->ReadNullTerminatedString(AnimResult.NamePtr);

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
            if (CoDAssets::GameFlags == SupportedGameFlags::SP)
            {
                auto MatResult = CoDAssets::GameInstance->Read<MW5XMaterialSP>(Asset.Header);

                std::string MaterialName;

                if (MatResult.NamePtr > 0)
                    MaterialName = Strings::Replace(CoDAssets::GameInstance->ReadNullTerminatedString(MatResult.NamePtr), "*", "");
                else
                    MaterialName = CoDAssets::GetHashedName("xmaterial", MatResult.Hash);

                auto LoadedMaterial = new CoDMaterial_t();
                LoadedMaterial->AssetName = FileSystems::GetFileName(MaterialName);
                LoadedMaterial->AssetPointer = Asset.Header;
                LoadedMaterial->ImageCount = MatResult.ImageCount;
                LoadedMaterial->AssetStatus = WraithAssetStatus::Loaded;

                CoDAssets::GameAssets->LoadedAssets.push_back(LoadedMaterial);
            }
            else
            {
                auto MatResult = CoDAssets::GameInstance->Read<MW5XMaterial>(Asset.Header);

                auto MaterialName = CoDAssets::GetHashedName("xmaterial", MatResult.Hash);

                auto LoadedMaterial = new CoDMaterial_t();
                LoadedMaterial->AssetName = FileSystems::GetFileName(MaterialName);
                LoadedMaterial->AssetPointer = Asset.Header;
                LoadedMaterial->ImageCount = MatResult.ImageCount;
                LoadedMaterial->AssetStatus = WraithAssetStatus::Loaded;

                CoDAssets::GameAssets->LoadedAssets.push_back(LoadedMaterial);
            }
        });
    }

    if (NeedsSounds)
    {
        auto Pool = CoDAssets::GameInstance->Read<ps::XAssetPool64>(ps::state->PoolsAddress + 197 * sizeof(ps::XAssetPool64));
        ps::PoolParser64(Pool.Root, CoDAssets::ParasyteRequest, [](ps::XAsset64& Asset)
        {
            auto SoundResult = CoDAssets::GameInstance->Read<MW5SndAsset>(Asset.Header);
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

std::unique_ptr<XAnim_t> GameModernWarfare5::ReadXAnim(const CoDAnim_t* Animation)
{
    return PackedXAnim::Read<MW5XAnim, MW5XAnimDeltaParts, MW5XAnimNotetrack>(Animation, LoadXAnim);
}

namespace
{
    // SP and MP models differ only in layout and in the width of a bone's id
    template <typename TModel>
    std::unique_ptr<XModel_t> ReadMW5Model(const CoDModel_t* Model, uint8_t BoneIndexSize)
    {
        auto ModelData = CoDAssets::GameInstance->Read<TModel>(Model->AssetPointer);

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
        ModelAsset->BoneIndexSize = BoneIndexSize;

        ModelAsset->BoneParentsPtr = ModelData.ParentListPtr;
        ModelAsset->BoneParentSize = 2;

        ModelAsset->RotationsPtr = ModelData.RotationsPtr;
        ModelAsset->TranslationsPtr = ModelData.TranslationsPtr;

        ModelAsset->BaseMatriciesPtr = ModelData.BaseMatriciesPtr;

        for (uint32_t i = 0; i < ModelData.NumLods; i++)
        {
            auto ModelLod = CoDAssets::GameInstance->Read<MW5XModelLod>(ModelData.ModelLods + i * sizeof(MW5XModelLod));
            ModelAsset->ModelLods.emplace_back(ModelLod.NumSurfs);
            auto& LodReference = ModelAsset->ModelLods[i];

            // Set distance (use index, "distance" we see may be bounds? doesn't line up)
            LodReference.LodDistance = i;

            LodReference.LODStreamInfoPtr = ModelLod.MeshPtr;

            auto XSurfacePtr = ModelLod.SurfsPtr;

            for (uint32_t s = 0; s < ModelLod.NumSurfs; s++)
            {
                LodReference.Submeshes.emplace_back();
                auto& SubmeshReference = LodReference.Submeshes[s];

                auto SurfaceInfo = CoDAssets::GameInstance->Read<MW5XModelSurface>(XSurfacePtr);

                SubmeshReference.RigidWeightsPtr = SurfaceInfo.RigidWeightsPtr;
                SubmeshReference.VertListcount = SurfaceInfo.VertListCount;
                SubmeshReference.VertexCount = SurfaceInfo.VertexCount;
                SubmeshReference.FaceCount = SurfaceInfo.FacesCount;
                SubmeshReference.VertexPtr = SurfaceInfo.Offsets[0];
                SubmeshReference.VertexUVsPtr = SurfaceInfo.Offsets[1];
                SubmeshReference.VertexNormalsPtr = SurfaceInfo.Offsets[2];
                SubmeshReference.FacesPtr = SurfaceInfo.Offsets[3];
                SubmeshReference.VertexColorPtr = SurfaceInfo.Offsets[6];
                SubmeshReference.WeightsPtr = SurfaceInfo.Offsets[10];

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

                for (size_t w = 0; w < 8; w++)
                    SubmeshReference.WeightCounts[w] = SurfaceInfo.WeightCounts[w];

                auto MaterialHandle = CoDAssets::GameInstance->Read<uint64_t>(ModelData.MaterialHandlesPtr);
                LodReference.Materials.emplace_back(GameModernWarfare5::ReadXMaterial(MaterialHandle));

                XSurfacePtr += sizeof(MW5XModelSurface);
                ModelData.MaterialHandlesPtr += sizeof(uint64_t);
            }
        }

        return ModelAsset;
    }
}

std::unique_ptr<XModel_t> GameModernWarfare5::ReadXModel(const CoDModel_t* Model)
{
    if (!CoDAssets::GameInstance->IsRunning()) return nullptr;

    if (CoDAssets::GameFlags == SupportedGameFlags::SP)
        return ReadMW5Model<MW5XModelSP>(Model, 8);
    return ReadMW5Model<MW5XModel>(Model, 4);
}

std::unique_ptr<XImageDDS> GameModernWarfare5::ReadXImage(const CoDImage_t* Image)
{
    return LoadXImage(XImage_t(ImageUsageType::DiffuseMap, 0, Image->AssetPointer, Image->AssetName));
}

std::unique_ptr<XSound> GameModernWarfare5::ReadXSound(const CoDSound_t* Sound)
{
    auto SoundData = CoDAssets::GameInstance->Read<MW5SndAsset>(Sound->AssetPointer);

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

        return SABSupport::OpusPackets(SoundBuffer.get(), SoundMemoryResult, SoundData.SeekTableSize, (size_t)SoundMemoryResult - SoundData.SeekTableSize, SoundData.FrameRate, SoundData.ChannelCount, SoundData.FrameCount, SABSupport::OpusPreSkipUnmeasured);
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

        return SABSupport::OpusPackets(SoundBuffer.get(), SoundMemoryResult, 32 + (size_t)SoundData.SeekTableSize, (size_t)SoundMemoryResult - 32 - SoundData.SeekTableSize, SoundData.FrameRate, SoundData.ChannelCount, SoundData.FrameCount, SABSupport::OpusPreSkipUnmeasured);
    }

}

void GameModernWarfare5::TranslateRawfile(const CoDRawFile_t * Rawfile, const std::string & ExportPath)
{
    std::error_code Ec;
    std::string ExportFolder = ExportPath;

    if (SettingsManager::GetSetting("keeprawpath", "true") == "true" && !Rawfile->RawFilePath.empty())
    {
        ExportFolder = (std::filesystem::path(ExportFolder) / Rawfile->RawFilePath).string();
    }

    std::filesystem::create_directories(ExportFolder, Ec);

    auto Bank = CoDAssets::GameInstance->Read<MW5SoundBank>(Rawfile->AssetPointer);

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

const XMaterial_t GameModernWarfare5::ReadXMaterial(uint64_t MaterialPointer)
{
    if (CoDAssets::GameFlags == SupportedGameFlags::SP)
    {
        auto MaterialData = CoDAssets::GameInstance->Read<MW5XMaterialSP>(MaterialPointer);

        XMaterial_t Result(MaterialData.ImageCount);
        if (MaterialData.NamePtr > 0)
            Result.MaterialName = Strings::Replace(FileSystems::GetFileName(CoDAssets::GameInstance->ReadNullTerminatedString(MaterialData.NamePtr)), "*", "");
        else
            Result.MaterialName = CoDAssets::GetHashedName("xmaterial", MaterialData.Hash);

        for (uint32_t m = 0; m < MaterialData.ImageCount; m++)
        {
            auto ImageInfo = CoDAssets::GameInstance->Read<MW5XMaterialImage>(MaterialData.ImageTablePtr);
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

            MaterialData.ImageTablePtr += sizeof(MW5XMaterialImage);
        }

        return Result;
    }
    else
    {
        auto MaterialData = CoDAssets::GameInstance->Read<MW5XMaterial>(MaterialPointer);

        XMaterial_t Result(MaterialData.ImageCount);
        Result.MaterialName = CoDAssets::GetHashedName("xmaterial", MaterialData.Hash);

        for (uint32_t m = 0; m < MaterialData.ImageCount; m++)
        {
            auto ImageInfo = CoDAssets::GameInstance->Read<MW5XMaterialImage>(MaterialData.ImageTablePtr);
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

            MaterialData.ImageTablePtr += sizeof(MW5XMaterialImage);
        }

        return Result;
    }
}

void GameModernWarfare5::PrepareVertexWeights(MemoryReader& ComplexReader, std::vector<WeightsData>& Weights, const XModelSubmesh_t & Submesh)
{
    uint32_t WeightDataIndex = 0;
    ModernIwReaders::ReadRigidWeights<MW5GfxRigidVerts>(Weights, WeightDataIndex, Submesh);

    if (ComplexReader.GetLength() > 0)
        ModernIwReaders::ReadSkinnedWeights(ComplexReader, Weights, WeightDataIndex, Submesh);
}

std::unique_ptr<XImageDDS> GameModernWarfare5::LoadXImage(const XImage_t& Image)
{
    return ModernIwReaders::LoadMipImage<MW5GfxImage, MW5GfxMipArray<32>>(Image, MW5DXGIFormats);
}

void GameModernWarfare5::LoadXModel(const std::unique_ptr<XModel_t>& Model, const XModelLod_t& ModelLOD, const std::unique_ptr<WraithModel>& ResultModel)
{
    auto ScaleConstant = (1.0f / 0x1FFFFF) * 2.0f;
    bool ExportColors = (SettingsManager::GetSetting("exportvtxcolor", "true") == "true");
    auto MeshInfo = CoDAssets::GameInstance->Read<MW5XModelMesh>(ModelLOD.LODStreamInfoPtr);

    auto BufferInfo = CoDAssets::GameInstance->Read<MW5XModelMeshBufferInfo>(MeshInfo.MeshBufferPointer);

    std::unique_ptr<uint8_t[]> MeshDataBuffer = nullptr;
    uint64_t MeshDataBufferSize = 0;

    // Even a streamed mesh can be in memory if loaded
    if (BufferInfo.BufferPtr != 0)
    {
        uintptr_t ResultSize = 0;
        auto TemporaryBuffer = CoDAssets::GameInstance->Read(BufferInfo.BufferPtr, BufferInfo.BufferSize, ResultSize);

        if (TemporaryBuffer != nullptr)
        {
            MeshDataBuffer = std::make_unique<uint8_t[]>(BufferInfo.BufferSize);
            std::memcpy(MeshDataBuffer.get(), TemporaryBuffer, (size_t)ResultSize);
            MeshDataBufferSize = ResultSize;

            delete[] TemporaryBuffer;
        }
    }
    else
    {
        uint32_t ResultSize = 0;
        MeshDataBuffer = CoDAssets::GamePackageCache->ExtractPackageObject(MeshInfo.LODStreamKey, BufferInfo.BufferSize, ResultSize);
        MeshDataBufferSize = ResultSize;
    }

    CoDXModelBonesHelper::ReadXModelBones(Model, ModelLOD, ResultModel);

    if (MeshDataBuffer != nullptr)
    {
        // Reader over the mesh (don't close)
        auto MeshReader = MemoryReader((int8_t*)MeshDataBuffer.get(), MeshDataBufferSize, true);

        ResultModel->PrepareSubmeshes((uint32_t)ModelLOD.Submeshes.size());

        // Readers for all data types, global so that we can use them when streaming...
        MemoryReader VertexPosReader;
        MemoryReader VertexNormReader;
        MemoryReader VertexUVReader;
        MemoryReader VertexColorReader;
        MemoryReader VertexWeightReader;
        MemoryReader SimpleWeightReader;
        MemoryReader FaceIndiciesReader;

        for (auto& Submesh : ModelLOD.Submeshes)
        {
            auto& Mesh = ResultModel->AddSubmesh();

            // COD has 1 material per submesh
            Mesh.AddMaterial(Submesh.MaterialIndex);

            Mesh.PrepareMesh(Submesh.VertexCount, Submesh.FaceCount);

            // Pre-allocate vertex weights (Data defaults to weight 1.0 on bone 0)
            auto VertexWeights = std::vector<WeightsData>(Submesh.VertexCount);

            VertexPosReader.Setup((int8_t*)MeshDataBuffer.get() + Submesh.VertexPtr, Submesh.VertexCount * sizeof(uint64_t), true);
            VertexNormReader.Setup((int8_t*)MeshDataBuffer.get() + Submesh.VertexNormalsPtr, Submesh.VertexCount * sizeof(uint32_t), true);
            VertexUVReader.Setup((int8_t*)MeshDataBuffer.get() + Submesh.VertexUVsPtr, Submesh.VertexCount * sizeof(uint32_t), true);
            FaceIndiciesReader.Setup((int8_t*)MeshDataBuffer.get() + Submesh.FacesPtr, Submesh.FaceCount * sizeof(uint16_t) * 3, true);

            auto WeightsDataLength = (
                (4 * Submesh.WeightCounts[0]) +
                (8 * Submesh.WeightCounts[1]) +
                (12 * Submesh.WeightCounts[2]) +
                (16 * Submesh.WeightCounts[3]) +
                (20 * Submesh.WeightCounts[4]) +
                (24 * Submesh.WeightCounts[5]) +
                (28 * Submesh.WeightCounts[6]) +
                (32 * Submesh.WeightCounts[7]));

            if (ResultModel->BoneCount() > 1)
            {
                VertexWeightReader.Setup((int8_t*)MeshDataBuffer.get() + Submesh.WeightsPtr, WeightsDataLength, true);
                PrepareVertexWeights(VertexWeightReader, VertexWeights, Submesh);
            }

            for (uint32_t i = 0; i < Submesh.VertexCount; i++)
            {
                auto& Vertex = Mesh.AddVertex();

                auto VertexPosition = VertexPosReader.Read<uint64_t>();

                Vertex.Position = Vector3(
                    (((((VertexPosition >> 00) & 0x1FFFFF) * ScaleConstant) - 1.0f) * Submesh.Scale) + Submesh.XOffset,
                    (((((VertexPosition >> 21) & 0x1FFFFF) * ScaleConstant) - 1.0f) * Submesh.Scale) + Submesh.YOffset,
                    (((((VertexPosition >> 42) & 0x1FFFFF) * ScaleConstant) - 1.0f) * Submesh.Scale) + Submesh.ZOffset);

                auto QTangent = VertexNormReader.Read<CoDQTangent>();

                Vertex.Normal = QTangent.Unpack(nullptr, nullptr);

                auto UVU = HalfFloats::ToFloat(VertexUVReader.Read<uint16_t>());
                auto UVV = HalfFloats::ToFloat(VertexUVReader.Read<uint16_t>());

                Vertex.AddUVLayer(UVU, UVV);

                // Some models don't store colors: the pointer is checked below
                Vertex.Color[0] = 255;
                Vertex.Color[1] = 255;
                Vertex.Color[2] = 255;
                Vertex.Color[3] = 255;

                auto& WeightValue = VertexWeights[i];

                for (uint32_t w = 0; w < WeightValue.WeightCount; w++)
                {
                    Vertex.AddVertexWeight(WeightValue.BoneValues[w], WeightValue.WeightValues[w]);
                }
            }

            if (ExportColors && Submesh.VertexColorPtr != 0xFFFFFFFF)
            {
                VertexColorReader.Setup((int8_t*)MeshDataBuffer.get() + Submesh.VertexColorPtr, Submesh.VertexCount * sizeof(uint32_t), true);

                for (uint32_t i = 0; i < Submesh.VertexCount; i++)
                {
                    // Some models don't store colors: the pointer is checked below
                    Mesh.Verticies[i].Color[0] = VertexColorReader.Read<uint8_t>();
                    Mesh.Verticies[i].Color[1] = VertexColorReader.Read<uint8_t>();
                    Mesh.Verticies[i].Color[2] = VertexColorReader.Read<uint8_t>();
                    Mesh.Verticies[i].Color[3] = VertexColorReader.Read<uint8_t>();
                }
            }

            for (uint32_t i = 0; i < Submesh.FaceCount; i++)
            {
                auto Face = FaceIndiciesReader.Read<MW5GfxStreamFace>();

                Mesh.AddFace(Face.Index1, Face.Index2, Face.Index3);
            }
        }
    }
}

void GameModernWarfare5::LoadXAnim(const std::unique_ptr<XAnim_t>& Anim, std::unique_ptr<WraithAnim>& ResultAnim)
{
    PackedXAnim::Load<MW5XAnim, MW5XAnimStreamInfo>(Anim, ResultAnim);
}

std::string GameModernWarfare5::LoadStringEntry(uint64_t Index)
{
    return CoDAssets::GameInstance->ReadNullTerminatedString(ps::state->StringsAddress + Index);
}

void GameModernWarfare5::PerformInitialSetup()
{
    std::error_code Ec;
    const auto OurPath = (std::filesystem::path(FileSystems::GetApplicationPath()) / "oo2core_8_win64.dll").string();

    CoDAssets::StringCache.LoadIndex((std::filesystem::path(FileSystems::GetApplicationPath()) / "package_index\\fnv1a_string.wni").string());
    CoDAssets::StringCache.LoadIndex((std::filesystem::path(FileSystems::GetApplicationPath()) / "package_index\\fnv1a_bones.wni").string());
    CoDAssets::AssetNameCache.LoadIndex((std::filesystem::path(FileSystems::GetApplicationPath()) / "package_index\\fnv1a_xsounds_unverified.wni").string());
    CoDAssets::AssetNameCache.LoadIndex((std::filesystem::path(FileSystems::GetApplicationPath()) / "package_index\\fnv1a_ximages_mw5.wni").string());

    CoDAssets::AssetNameCache.LoadIndex((std::filesystem::path(FileSystems::GetApplicationPath()) / "package_index\\fnv1a_xsounds_MW2022_Weapon.wni").string());

    // For hashes testing
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

void GameModernWarfare5::PerformShutDown()
{
}
