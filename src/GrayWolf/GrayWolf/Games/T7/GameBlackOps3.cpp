#include "stdafx.h"

#include <algorithm>
#include <cstring>

#include "GameBlackOps3.h"

#include "GameReaders.h"

#include "CoDAssets.h"
#include "SABSupport.h"   // BO3 audio lives in .sabl banks under zone\snd\, not in the live asset pool

#include "FileSystems.h"
#include "Games/Common/MeshDecode.h"
#include "SettingsManager.h"
#include "WraithModel.h"

#include <algorithm>
#include <format>

struct BO3XAssetPoolData
{
    uint64_t PoolPtr;
    
    uint32_t AssetSize;
    uint32_t PoolSize;

    uint32_t Padding;

    uint32_t AssetsLoaded;

    uint64_t PoolFreeHeadPtr;
};

static_assert(sizeof(BO3XAssetPoolData) == 0x20, "Invalid Pool Data Size (Expected 0x20)");

std::unique_ptr<XAnim_t> GameBlackOps3::ReadXAnim(const CoDAnim_t* Animation)
{
    if (!CoDAssets::GameInstance->IsRunning()) return nullptr;

    auto Anim = std::make_unique<XAnim_t>();
    const auto AnimData = CoDAssets::GameInstance->Read<BO3XAnim>(Animation->AssetPointer);

    Anim->AnimationName = Animation->AssetName;
    Anim->ViewModelAnimation = GameReaders::StartsWithAny(Animation->AssetName, { "viewmodel_", "vm_" });
    Anim->AdditiveAnimation = AnimData.AssetType == 0x6;
    Anim->LoopingAnimation = AnimData.LoopingFlag > 0;

    GameReaders::CopyAnimHeader(*Anim, AnimData);
    Anim->BoneIndexSize = 4;
    GameReaders::CopyDeltaParts(*Anim, CoDAssets::GameInstance->Read<BO3XAnimDeltaParts>(AnimData.DeltaPartsPtr));

    Anim->RotationType = AnimationKeyTypes::HalfFloat;
    Anim->TranslationType = AnimationKeyTypes::MinSizeTable;
    Anim->SupportsInlineIndicies = false;
    return Anim;
}

std::unique_ptr<XModel_t> GameBlackOps3::ReadXModel(const CoDModel_t* Model)
{
    if (!CoDAssets::GameInstance->IsRunning()) return nullptr;

    const auto ModelData = CoDAssets::GameInstance->Read<BO3XModel>(Model->AssetPointer);
    auto ModelAsset = std::make_unique<XModel_t>(ModelData.NumLods);

    GameReaders::CopyModelBones(*ModelAsset, Model->AssetName, ModelData, 4, 1);
    ModelAsset->CosmeticBoneCount = ModelData.NumCosmeticBones;
    ModelAsset->BoneRotationData = BoneDataTypes::HalfFloat;
    ModelAsset->IsModelStreamed = true;
    ModelAsset->BoneInfoPtr = ModelData.BoneInfoPtr;

    GameReaders::ReadStreamedLods<BO3XModelLod, BO3XModelSurface>(
        *ModelAsset, ModelData.ModelLodPtrs, ModelData.NumLods, ModelData.MaterialHandlesPtr, 24, ReadXMaterial,
        [](XModelSubmesh_t& Submesh, const BO3XModelSurface& Info)
        {
            GameReaders::CopyStreamedSurface(Submesh, Info);
            Submesh.WeightCounts[0] = Info.Flag1;
            Submesh.WeightCounts[1] = Info.Flag2;
            Submesh.WeightCounts[2] = Info.Flag3;
            Submesh.WeightCounts[3] = Info.Flag4;
        },
        [](XModelLod_t& Lod, const BO3XModelLod& Info) { Lod.LodMaxDistance = Info.LodMinDistance; });

    return ModelAsset;
}

const XMaterial_t GameBlackOps3::ReadXMaterial(uint64_t MaterialPointer)
{
    auto MaterialData = CoDAssets::GameInstance->Read<BO3XMaterial>(MaterialPointer);

    XMaterial_t Result(MaterialData.ImageCount);
    Result.MaterialName = FileSystems::GetFileNameWithoutExtension(CoDAssets::GameInstance->ReadNullTerminatedString(MaterialData.NamePtr));

    // The technique set's name (+0x278 -> its name pointer) and each technique's static pixel
    // cbuffer (constantBufferInfo[12] at +0x30, 0x30 apart, psStatic[0] at +0x20 -> init data
    // pointer +0x28, size +0x30): the adapter reads them through shading.txt.
    const uint64_t Techset = CoDAssets::GameInstance->Read<uint64_t>(MaterialPointer + 0x278);
    if (Techset != 0)
    {
        const uint64_t TechsetName = CoDAssets::GameInstance->Read<uint64_t>(Techset);
        if (TechsetName != 0) Result.TechsetName = CoDAssets::GameInstance->ReadNullTerminatedString(TechsetName);
    }
    for (uint32_t T = 0; T < 12; T++)
    {
        std::vector<uint8_t> Block;
        const uint64_t Buffer = CoDAssets::GameInstance->Read<uint64_t>(MaterialPointer + 0x30 + 0x30 * T + 0x20);
        const uint64_t Data = Buffer ? CoDAssets::GameInstance->Read<uint64_t>(Buffer + 0x28) : 0;
        const uint32_t Size = Buffer ? CoDAssets::GameInstance->Read<uint32_t>(Buffer + 0x30) : 0;
        if (Data != 0 && Size > 0 && Size <= 0x1000)
        {
            Block.resize(Size);
            if (CoDAssets::GameInstance->Read(Block.data(), Data, Size) != Size) Block.clear();
        }
        Result.Constants.push_back(std::move(Block));
    }

    // an image's name pointer 16 bytes before its end
    GameReaders::AddMaterialImages<BO3XMaterialImage>(
        Result, MaterialData.ImageTablePtr, MaterialData.ImageCount,
        [](uint64_t ImagePtr) { return GameReaders::ImageNameAt<uint64_t>(ImagePtr, sizeof(BO3GfxImage) - 16); }, GameReaders::TreyarchSemanticUsage);

    return Result;
}

std::unique_ptr<XImageDDS> GameBlackOps3::LoadXImage(const XImage_t& Image)
{
    // Both loaded and streamed images are read
    auto ImageInfo = CoDAssets::GameInstance->Read<BO3GfxImage>(Image.ImagePtr);

    uint32_t LargestMip = 0;
    uint32_t LargestWidth = 0;
    uint32_t LargestHeight = 0;
    uint64_t LargestHash = 0;
    uint64_t LargestSize = 0;

    for (uint32_t i = 0; i < 4; i++)
    {
        if (ImageInfo.MipLevels[i].Width > LargestWidth)
        {
            LargestMip = i;
            LargestWidth = ImageInfo.MipLevels[i].Width;
            LargestHeight = ImageInfo.MipLevels[i].Height;
            LargestHash = ImageInfo.MipLevels[i].HashID;
            LargestSize = (uint64_t)(i == 0 ? ImageInfo.MipLevels[i].Size >> 4 : (ImageInfo.MipLevels[i].Size >> 4) - (ImageInfo.MipLevels[i - 1].Size >> 4));
        }
    }

    return GameReaders::DecodeTreyarchImage(ImageInfo, LargestWidth != 0 && LargestHash != 0, LargestWidth, LargestHeight, Image.ImageUsage,
                                            [&](uint32_t& ResultSize) { return CoDAssets::GamePackageCache->ExtractPackageObject(LargestHash, LargestSize, ResultSize); });
}

void GameBlackOps3::LoadXModel(const XModelLod_t& ModelLOD, const std::unique_ptr<WraithModel>& ResultModel)
{
    bool ExportColors = (SettingsManager::GetSetting("exportvtxcolor", "true") == "true");
    auto MeshInfo = CoDAssets::GameInstance->Read<BO3XModelMeshInfo>(ModelLOD.LODStreamInfoPtr);

    // loaded, else in the packages, none open failing the model
    const bool Loaded = (MeshInfo.StatusFlag & (1 << 1)) != 0;
    if (!Loaded && CoDAssets::GamePackageCache == nullptr)
        throw std::runtime_error("the mesh is in the packages, none open");
    const auto MeshData = Loaded ? MeshDecode::FromZone(MeshInfo.XModelMeshBufferPtr, MeshInfo.XModelMeshBufferSize)
                                 : MeshDecode::FromPackage(ModelLOD.LODStreamKey, MeshInfo.XModelMeshBufferSize);

    if (MeshData.Data != nullptr)
    {
        // a surface is weighted when its flags say so, while the LOD's weight records last; a bone
        // past the model's own is added (smod_bone<n>)
        uint32_t WeightsLeft = MeshInfo.WeightCount;
        uint32_t MaximumWeightIndex = ResultModel->BoneCount() - 1;
        const MeshDecode::Regions Regions{ MeshInfo.VertexOffset, MeshInfo.UVOffset, MeshInfo.WeightsOffset, MeshInfo.FacesOffset, 16 };

        ResultModel->PrepareSubmeshes((uint32_t)ModelLOD.Submeshes.size());

        std::vector<WeightsData> Weights;
        for (auto& Submesh : ModelLOD.Submeshes)
        {
            auto& Mesh = ResultModel->AddSubmesh();

            // COD has 1 material per submesh
            Mesh.AddMaterial(Submesh.MaterialIndex);

            // a stream outside the buffer fails the model
            const bool Weighted = ((uint8_t)Submesh.WeightCounts[2] & 2) > 0;
            if (Weighted && !MeshDecode::ReadVertexWeights(MeshData.View(), Regions.Weights + Submesh.VertexPtr * 12, Submesh.VertexCount, 0xFFFF,
                                                           MaximumWeightIndex, &WeightsLeft, Weights))
                throw std::out_of_range("weights outside the mesh buffer");
            const auto Streams = MeshDecode::RegionStreams(Regions, Submesh.VertexPtr, Submesh.VertexCount, Submesh.FacesPtr, Submesh.FaceCount, ExportColors);
            if (!MeshDecode::DecodeSubmesh(MeshData.View(), Streams, MeshDecode::RegionEncoding, Weighted ? Weights.data() : nullptr, Mesh))
                throw std::out_of_range("a submesh outside the mesh buffer");
        }

        MeshDecode::AddWeightBones(*ResultModel, MaximumWeightIndex, 1, -1, "smod_bone0");
    }
}
