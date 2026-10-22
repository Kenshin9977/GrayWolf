#include "stdafx.h"

#include "GameBlackOpsCW.h"

#include "GameReaders.h"

#include "CoDAssets.h"

#include "FileSystems.h"
#include "MemoryReader.h"
#include "Games/Common/MeshDecode.h"
#include "SettingsManager.h"
#include "HalfFloats.h"
#include "SABSupport.h"

#include <d3d12.h>
#include <d3d12shader.h>
#include <format>
#include <filesystem>
#include "dxc/dxcapi.h"
#include "dxc/DxilContainer/DxilContainer.h"
#include "DxilReflection.h"
#include "WraithModel.h"
#include "WraithNameIndex.h"


#pragma comment(lib, "dxguid.lib")

WraithNameIndex GameBlackOpsCW::AssetNameCache = WraithNameIndex();
WraithNameIndex GameBlackOpsCW::StringCache = WraithNameIndex();

struct BOCWXAssetPoolData
{
    uint64_t PoolPtr;
    uint32_t AssetSize;
    uint32_t PoolSize;
    uint32_t Padding;
    uint32_t AssetsLoaded;
    uint64_t PoolFreeHeadPtr;
};

uint32_t GetFrameRate(uint8_t Index)
{
    switch (Index)
    {
    case 0: return 8000;
    case 1: return 12000;
    case 2: return 16000;
    case 3: return 24000;
    case 4: return 32000;
    case 5: return 44100;
    case 6: return 48000;
    case 7: return 96000;
    case 8: return 192000;
    default: return 48000;
    }
}

#pragma pack(push, 1)
struct BOCWStreamInfo
{
    uint64_t NamePtr;
    uint64_t UnknownZero;
    uint64_t StreamKey;
};
#pragma pack(pop)

static_assert(sizeof(BOCWXAssetPoolData) == 0x20, "Invalid Pool Data Size (Expected 0x20)");

std::unique_ptr<XAnim_t> GameBlackOpsCW::ReadXAnim(const CoDAnim_t* Animation)
{
    if (!CoDAssets::GameInstance->IsRunning()) return nullptr;

    auto Anim = std::make_unique<XAnim_t>();
    const auto AnimData = CoDAssets::GameInstance->Read<BOCWXAnim>(Animation->AssetPointer);

    Anim->AnimationName = Animation->AssetName;

    GameReaders::CopyAnimHeader(*Anim, AnimData);
    Anim->BoneIndexSize = 4;
    GameReaders::CopyDeltaParts(*Anim, CoDAssets::GameInstance->Read<BO4XAnimDeltaParts>(AnimData.DeltaPartsPtr));
    Anim->BlendShapeNamesPtr = AnimData.UnknownPtr;
    Anim->BlendShapeWeightsPtr = AnimData.UnknownZero1;
    Anim->BlendShapeWeightCount = (uint32_t)*(uint16_t*)&AnimData.Unknown[0];

    Anim->RotationType = AnimationKeyTypes::QuatPackingA;
    Anim->TranslationType = AnimationKeyTypes::MinSizeTable;
    Anim->SupportsInlineIndicies = false;
    return Anim;
}

std::unique_ptr<XModel_t> GameBlackOpsCW::ReadXModel(const CoDModel_t* Model)
{
    if (!CoDAssets::GameInstance->IsRunning()) return nullptr;

    const auto ModelData = CoDAssets::GameInstance->Read<BOCWXModel>(Model->AssetPointer);
    const auto XSkeleton = CoDAssets::GameInstance->Read<BOCWXSkeleton>(ModelData.XSkeletonPtr);
    auto ModelAsset = std::make_unique<XModel_t>(ModelData.NumLods);

    ModelAsset->ModelName = Model->AssetName;
    ModelAsset->BoneCount = XSkeleton.BoneCounts[1];
    ModelAsset->RootBoneCount = XSkeleton.BoneCounts[2];
    ModelAsset->CosmeticBoneCount = XSkeleton.BoneCounts[0];
    ModelAsset->BoneRotationData = BoneDataTypes::QuatPackingA;
    ModelAsset->IsModelStreamed = true;
    ModelAsset->BoneIDsPtr = XSkeleton.BoneIDsPtr;
    ModelAsset->BoneIndexSize = 4;
    ModelAsset->BoneParentsPtr = XSkeleton.ParentListPtr;
    ModelAsset->BoneParentSize = 2;
    ModelAsset->RotationsPtr = XSkeleton.RotationsPtr;
    ModelAsset->TranslationsPtr = XSkeleton.TranslationsPtr;
    ModelAsset->BaseMatriciesPtr = XSkeleton.BaseMatriciesPtr;

    GameReaders::ReadStreamedLods<BOCWXModelLod, BOCWXModelSurface>(
        *ModelAsset, ModelData.ModelLodPtrs, ModelData.NumLods, ModelData.MaterialHandlesPtr, 32, ReadXMaterial,
        [](XModelSubmesh_t& Submesh, const BOCWXModelSurface& Info)
        {
            GameReaders::CopyStreamedSurface(Submesh, Info);
            Submesh.WeightCounts[0] = Info.Flag1;
            Submesh.WeightCounts[1] = Info.Flag2;
        });

    return ModelAsset;
}

// A sound_asset (0x58): FrameCount 8, rate index 0xc, channels 0xd; its Opus packets {u32 size,
// packet} in the packages by the key at 0x38 (the object's stored bytes, the packets from 0), or
// through the streamkey asset at 0x40 (its data in the zone, else the package object by its key at
// 8: 0x118 bytes, the packets' byte count u32 at 0x118, the packets from 0x120). Returned as the game's packets, {u16 size, packet}
// (SABSupport::OpusPackets): decoded for WAV/FLAC, remuxed into Ogg for Opus.
std::unique_ptr<XSound> GameBlackOpsCW::ReadXSound(const CoDSound_t * Sound)
{
    const auto SoundData = CoDAssets::GameInstance->Read<BOCWSoundAsset>(Sound->AssetPointer);
    if (CoDAssets::GamePackageCache == nullptr || SoundData.ChannelCount == 0) return nullptr;

    std::unique_ptr<uint8_t[]> SoundBuffer;
    uint32_t Got = 0;
    size_t Offset = 0, Size = 0;
    if (SoundData.StreamKey != 0)
    {
        SoundBuffer = CoDAssets::GamePackageCache->ExtractPackageObjectRaw(SoundData.StreamKey, Got);
        Size = Got;
    }
    else if (SoundData.StreamInfoPtr != 0)
    {
        // the streamkey (0x38): flags u8 0x37, 2 set when its data is in the zone (at 0x20, u32 size
        // 0x30), else in the packages by the key at 8
        if (CoDAssets::GameInstance->Read<uint8_t>(SoundData.StreamInfoPtr + 0x37) & 2)
        {
            Got = CoDAssets::GameInstance->Read<uint32_t>(SoundData.StreamInfoPtr + 0x30);
            if (Got == 0 || Got > (1u << 30)) return nullptr;
            SoundBuffer = std::make_unique<uint8_t[]>(Got);
            if (CoDAssets::GameInstance->Read(SoundBuffer.get(), CoDAssets::GameInstance->Read<uint64_t>(SoundData.StreamInfoPtr + 0x20), Got) != Got)
                return nullptr;
        }
        else
            SoundBuffer = CoDAssets::GamePackageCache->ExtractPackageObject(CoDAssets::GameInstance->Read<uint64_t>(SoundData.StreamInfoPtr + 0x8), Got);
        if (SoundBuffer != nullptr && Got >= 0x120)
            Size = std::min<size_t>(*(uint32_t*)(SoundBuffer.get() + 0x118), Got - 0x120), Offset = 0x120;
    }
    if (SoundBuffer == nullptr || Size == 0) return nullptr;

    std::vector<uint8_t> Packets;
    Packets.reserve(Size);
    for (size_t At = Offset; At + 4 <= Offset + Size;)
    {
        const uint32_t Block = *(uint32_t*)(SoundBuffer.get() + At);
        At += 4;
        if (Block == 0) break;   // the object's padding
        if (Block > 0xFFFF || At + Block > Offset + Size) return nullptr;
        Packets.push_back((uint8_t)Block);
        Packets.push_back((uint8_t)(Block >> 8));
        Packets.insert(Packets.end(), SoundBuffer.get() + At, SoundBuffer.get() + At + Block);
        At += Block;
    }
    return SABSupport::OpusPackets(Packets.data(), Packets.size(), 0, Packets.size(), GetFrameRate(SoundData.FrameRateIndex),
                                   SoundData.ChannelCount, SoundData.FrameCount, SABSupport::OpusPreSkipPacket);
}

#pragma pack(push, 1)
struct BOCWXMaterialImage
{
    uint64_t ImagePtr;
    uint32_t SemanticHash;
    uint8_t Padding[0xC];
};
#pragma pack(pop)

struct BOCWMaterialTechniqueSet
{
    uint64_t Hash;
    uint64_t Hash2;
    uint64_t Flags;
    uint64_t Passed[18];
};

struct BOCWMaterialTechnique
{
    uint64_t Hash;
    uint8_t Padding[32];
    uint64_t ShadersPtr;
};

struct BOCWMaterialTechniqueShader
{
    uint64_t Hash;
    uint64_t ShaderPtr;
    uint64_t ShaderSize;
};

#pragma pack(push, 1)
struct BOCWXMaterialEx
{
    uint64_t NamePtr;
    uint8_t Padding[0x20];
    uint64_t TechsetPtr;
    uint64_t ImageTablePtr;
    uint64_t UnkPtr;
    uint64_t MaterialInstanceDataPtr;
    uint8_t Padding1[96];
    uint64_t PerPassBuffers[13];
    uint64_t CBuffersSize;
    uint64_t CBuffersPtr;
    uint8_t Padding2[40];
    uint8_t ImageCount;
    uint8_t Padding3[15];
};
#pragma pack(pop)

const XMaterial_t GameBlackOpsCW::ReadXMaterial(uint64_t MaterialPointer)
{
    auto MaterialData = CoDAssets::GameInstance->Read<BOCWXMaterialEx>(MaterialPointer);
    // Mask the name (some bits are used for other stuffs)
    MaterialData.NamePtr &= 0xFFFFFFFFFFFFFFF;
    XMaterial_t Result(MaterialData.ImageCount);
    Result.MaterialName = std::format("xmaterial_{:x}", MaterialData.NamePtr);
    Result.TechsetName = std::format("xtechset_{:x}", CoDAssets::GameInstance->Read<uint64_t>(MaterialData.TechsetPtr));

    if (AssetNameCache.NameDatabase.find(MaterialData.NamePtr) != AssetNameCache.NameDatabase.end())
        Result.MaterialName = FileSystems::GetFileNamePurgeExtensions(AssetNameCache.NameDatabase[MaterialData.NamePtr]);

    for (uint32_t m = 0; m < MaterialData.ImageCount; m++)
    {
        auto ImageInfo = CoDAssets::GameInstance->Read<BOCWXMaterialImage>(MaterialData.ImageTablePtr);

        // Get Hash and mask it (some bits are used for other stuffs)
        auto ImageHash = CoDAssets::GameInstance->Read<uint64_t>(ImageInfo.ImagePtr) & 0xFFFFFFFFFFFFFFF;

        auto ImageName = std::format("ximage_{:x}", ImageHash);

        if (AssetNameCache.NameDatabase.find(ImageHash) != AssetNameCache.NameDatabase.end())
            ImageName = AssetNameCache.NameDatabase[ImageHash];

        auto DefaultUsage = ImageUsageType::Unknown;
        switch (ImageInfo.SemanticHash)
        {
        case 0xA0AB1041:
            DefaultUsage = ImageUsageType::DiffuseMap;
            break;
        case 0x59D30D0F:
            DefaultUsage = ImageUsageType::NormalMap;
            break;
        case 0xEC443804:
            DefaultUsage = ImageUsageType::SpecularMap;
            break;
        }

        Result.Images.emplace_back(DefaultUsage, ImageInfo.SemanticHash, ImageInfo.ImagePtr, ImageName);

        MaterialData.ImageTablePtr += sizeof(BOCWXMaterialImage);
    }

    // Grab the pixel shader for this pass (most useful reflection info); which pass index
    // discriminates deferred vs. forward materials is easiest to tell by pointer.
    auto TechSetData = CoDAssets::GameInstance->Read<BOCWMaterialTechniqueSet>(MaterialData.TechsetPtr);
    auto TechSetPassIndex = TechSetData.Passed[6] != 0 ? 6 : 8;
    auto TechSetPassData = CoDAssets::GameInstance->Read<BOCWMaterialTechnique>(TechSetData.Passed[TechSetPassIndex]);
    auto PixelShader = CoDAssets::GameInstance->Read<BOCWMaterialTechniqueShader>(CoDAssets::GameInstance->Read<uint64_t>(TechSetPassData.ShadersPtr + 24));
    auto PixelShaderBuffer = std::make_unique<uint8_t[]>(PixelShader.ShaderSize);
    auto PixelShaderSize = CoDAssets::GameInstance->Read(PixelShaderBuffer.get(), PixelShader.ShaderPtr, PixelShader.ShaderSize);

    // Two paths: forward materials need shader reflection to resolve their types, while GBuffer
    // materials can skip that and read tints/ranges directly out of the constant instance data.
    if (TechSetPassIndex == 6)
    {
        IDxcLibrary* library = nullptr;
        ID3D12ShaderReflection* reflection{};
        IDxcContainerReflection* pReflection{};
        IDxcBlobEncoding* blob = nullptr;
        ID3D12ShaderReflection* ppReflection{};
        UINT32 shaderIdx = 0;
        D3D12_SHADER_DESC desc{};

        // dxcompiler.dll: the shared loader's (beside the exe, a Windows SDK, the PATH)
        if (!FAILED(DxilReflection::CreateInstance(CLSID_DxcContainerReflection, &pReflection)) &&
            !FAILED(DxilReflection::CreateInstance(CLSID_DxcLibrary, &library)) &&
            !FAILED(library->CreateBlobWithEncodingFromPinned(PixelShaderBuffer.get(), PixelShader.ShaderSize, 0, &blob)) &&
            !FAILED(pReflection->Load(blob)) &&
            !FAILED(pReflection->FindFirstPartKind(hlsl::DFCC_ShaderStatistics, &shaderIdx)) &&
            !FAILED(pReflection->GetPartReflection(shaderIdx, __uuidof(ID3D12ShaderReflection), (void**)&ppReflection)) &&
            !FAILED(ppReflection->GetDesc(&desc)))
        {
            ID3D12ShaderReflectionConstantBuffer* constBuf = ppReflection->GetConstantBufferByName("$Globals");

            D3D12_SHADER_BUFFER_DESC bufDesc{};
            D3D12_SHADER_VARIABLE_DESC varDesc{};
            D3D12_SHADER_TYPE_DESC typeDesc{};

            if (!FAILED(constBuf->GetDesc(&bufDesc)))
            {
                auto CBuffers = std::make_unique<uint8_t[]>(MaterialData.CBuffersSize);
                auto CBuffersSize = CoDAssets::GameInstance->Read(CBuffers.get(), MaterialData.CBuffersPtr, MaterialData.CBuffersSize);
                auto CBufferOffset = CoDAssets::GameInstance->Read<uint32_t>(CoDAssets::GameInstance->Read<uint64_t>(MaterialData.PerPassBuffers[TechSetPassIndex] + 16));

                for (UINT i = 0; i < bufDesc.Variables; i++)
                {
                    auto variable = constBuf->GetVariableByIndex(i);

                    // Treyarch generates these from techsets (as in BO3): assumed plain types, no nested structures
                    if (!FAILED(variable->GetDesc(&varDesc)) && !FAILED(variable->GetType()->GetDesc(&typeDesc)))
                    {
                        switch (typeDesc.Type)
                        {
                        case D3D_SVT_BOOL:
                        case D3D_SVT_FLOAT:
                            Result.Settings.emplace_back(
                                varDesc.Name,
                                "float",
                                (float*)(CBuffers.get() + CBufferOffset + varDesc.StartOffset),
                                typeDesc.Columns);
                            break;
                        case D3D_SVT_INT:
                            Result.Settings.emplace_back(
                                varDesc.Name,
                                "int",
                                (int32_t*)(CBuffers.get() + CBufferOffset + varDesc.StartOffset),
                                typeDesc.Columns);
                            break;
                        case D3D_SVT_UINT:
                            Result.Settings.emplace_back(
                                varDesc.Name,
                                "uint",
                                (uint32_t*)(CBuffers.get() + CBufferOffset + varDesc.StartOffset),
                                typeDesc.Columns);
                            break;
                        }
                    }
                }
            }
#if _DEBUG
            else
            {
                printf("WARNING: Failed to get $Globals for material: %s\n", Result.MaterialName.c_str());
            }
#endif
        }
    }
    else
    {
        auto MaterialInstanceData = std::make_unique<uint8_t[]>(220);
        auto MaterialInstanceDataSize = CoDAssets::GameInstance->Read(MaterialInstanceData.get(), MaterialData.MaterialInstanceDataPtr, 220);

        Result.Settings.emplace_back(
            "colorTint",
            "float3",
            (float*)(MaterialInstanceData.get() + 8),
            3);
        Result.Settings.emplace_back(
            "specColorTint",
            "float3",
            (float*)(MaterialInstanceData.get() + 40),
            3);
        Result.Settings.emplace_back(
            "perceptualRoughnessRange",
            "float2",
            (float*)(MaterialInstanceData.get() + 44),
            2);
    }

    return Result;
}

std::unique_ptr<XImageDDS> GameBlackOpsCW::LoadXImage(const XImage_t& Image)
{
    // Both loaded and streamed images are read
    auto ImageInfo = CoDAssets::GameInstance->Read<BOCWGfxImage>(Image.ImagePtr);

    uint32_t LargestMip = 0;
    uint32_t LargestSize = 0;
    uint32_t LargestWidth = 0;
    uint32_t LargestHeight = 0;
    uint64_t LargestHash = 0;

    for (uint32_t i = 0; i < ImageInfo.GfxMipMaps; i++)
    {
        auto MipMap = CoDAssets::GameInstance->Read<BOCWGfxMip>(ImageInfo.GfxMipsPtr);
        // The mip must exist: users without HD Texture Packs lack some
        if (MipMap.Size > LargestSize && MipMap.HashID != 0 && CoDAssets::GamePackageCache->Exists(MipMap.HashID))
        {
            LargestMip    = i;
            LargestSize   = MipMap.Size;
            LargestHash   = MipMap.HashID;
            LargestWidth  = ImageInfo.LoadedMipWidth >> (ImageInfo.GfxMipMaps - i - 1);
            LargestHeight = ImageInfo.LoadedMipHeight >> (ImageInfo.GfxMipMaps - i - 1);
        }
        ImageInfo.GfxMipsPtr += sizeof(BOCWGfxMip);
    }

    return GameReaders::DecodeTreyarchImage(ImageInfo, LargestHash != 0, LargestWidth, LargestHeight, Image.ImageUsage,
                                            [&](uint32_t& ResultSize) { return CoDAssets::GamePackageCache->ExtractPackageObject(LargestHash, ResultSize); });
}

struct BlendShapeTargetInfo
{
    uint32_t Name;
    uint32_t Index;
};

struct BlendShapeVertexInfo
{
    uint32_t Offset;
    uint32_t Count;
};

struct BlendShapeVertex
{
    uint16_t VertexIndex;
    uint16_t X;
    uint16_t Y;
    uint16_t Z;
};

void GameBlackOpsCW::LoadXModel(const XModelLod_t& ModelLOD, const std::unique_ptr<WraithModel>& ResultModel)
{
    bool ExportColors = (SettingsManager::GetSetting("exportvtxcolor", "true") == "true");
    auto MeshInfo = CoDAssets::GameInstance->Read<BOCWXModelMeshInfo>(ModelLOD.LODStreamInfoPtr);

    bool HasExtendedVertexInfo = (MeshInfo.StatusFlag & 64) != 0;

    // flag == 8 seems to mean loaded; else the packages, none open failing the model
    const bool Loaded = (MeshInfo.StatusFlag & 0x3F) == 8;
    if (!Loaded && CoDAssets::GamePackageCache == nullptr)
        throw std::runtime_error("the mesh is in the packages, none open");
    const auto MeshData = Loaded ? MeshDecode::FromZone(MeshInfo.XModelMeshBufferPtr, MeshInfo.XModelMeshBufferSize)
                                 : MeshDecode::FromPackage(ModelLOD.LODStreamKey);

    if (MeshData.Data != nullptr)
    {
        // Reader over the mesh (don't close)
        auto MeshReader = MemoryReader((int8_t*)MeshData.Data.get(), MeshData.Size, true);
        ResultModel->PrepareSubmeshes((uint32_t)ModelLOD.Submeshes.size());

        auto Vertices = std::make_unique<std::vector<std::pair<uint32_t, Vector3>>[]>(MeshInfo.VertexCount);
        bool HasShapes = false;

        if (MeshInfo.BlendShapeIndexes != 0 && MeshInfo.BlendShapeCounts != 0)
        {
            for (size_t i = 0; i < ((size_t)MeshInfo.ShapeCount + (size_t)MeshInfo.FuckKnowsAgain); i++)
            {
                auto TargetInfo = CoDAssets::GameInstance->Read<BlendShapeTargetInfo>(MeshInfo.BlendShapeIndexes + i * sizeof(BlendShapeTargetInfo));
                auto VertexInfo = CoDAssets::GameInstance->Read<BlendShapeVertexInfo>(MeshInfo.BlendShapeCounts + TargetInfo.Index * sizeof(BlendShapeVertexInfo));

                // TODO: hitting the same shape again may be Normal/Tangent data (see c_t9_eng_hero_park_head,
                // campaign); there must be a flag for it.
                if (TargetInfo.Index >= MeshInfo.ShapeCount)
                    continue;

                // A unique shape, first instance: delta positions
                auto ShapeIndex = ResultModel->BlendShapes.size();
                ResultModel->BlendShapes.push_back(CoDAssets::GameStringHandler(TargetInfo.Name));
                MeshReader.SetPosition(MeshInfo.BlendshapesOffset + (size_t)VertexInfo.Offset);

                for (size_t j = 0; j < (size_t)VertexInfo.Count; j++)
                {
                    auto ValueForVertex = MeshReader.Read<BlendShapeVertex>();

                    Vertices[ValueForVertex.VertexIndex].push_back(std::make_pair(
                        (uint32_t)ShapeIndex,
                        Vector3(
                        HalfFloats::ToFloat(ValueForVertex.X),
                        HalfFloats::ToFloat(ValueForVertex.Y),
                        HalfFloats::ToFloat(ValueForVertex.Z)
                    )));
                    HasShapes = true;
                }
            }
        }

        // the vertex records carry 8 more bytes when extended (the first 4 seem to be a second UV,
        // possibly for camo mapping); a surface is weighted when its flags say so (bones masked
        // off their bit 15, seemingly blendshape related)
        const MeshDecode::Regions Regions{ MeshInfo.VertexOffset, MeshInfo.UVOffset, MeshInfo.WeightsOffset, MeshInfo.FacesOffset,
                                           HasExtendedVertexInfo ? 24u : 16u };
        std::vector<WeightsData> Weights;
        for (auto& Submesh : ModelLOD.Submeshes)
        {
            auto& Mesh = ResultModel->AddSubmesh();

            // COD has 1 material per submesh
            Mesh.AddMaterial(Submesh.MaterialIndex);

            // a stream outside the buffer fails the model
            uint32_t MaxBone = 0;
            const bool Weighted = ((uint8_t)Submesh.WeightCounts[0] & 2) > 0;
            if (Weighted && !MeshDecode::ReadVertexWeights(MeshData.View(), Regions.Weights + Submesh.VertexPtr * 12, Submesh.VertexCount, 0x7FFF,
                                                           MaxBone, nullptr, Weights))
                throw std::out_of_range("weights outside the mesh buffer");
            const auto Streams = MeshDecode::RegionStreams(Regions, Submesh.VertexPtr, Submesh.VertexCount, Submesh.FacesPtr, Submesh.FaceCount, ExportColors);
            if (!MeshDecode::DecodeSubmesh(MeshData.View(), Streams, MeshDecode::RegionEncoding, Weighted ? Weights.data() : nullptr, Mesh))
                throw std::out_of_range("a submesh outside the mesh buffer");

            for (uint32_t i = 0; HasShapes && i < Submesh.VertexCount && Submesh.VertexPtr + i < MeshInfo.VertexCount; i++)
                Mesh.Verticies[i].BlendShapeDeltas = Vertices[Submesh.VertexPtr + i];
        }
    }
}

void GameBlackOpsCW::PerformInitialSetup()
{
    AssetNameCache.LoadIndex((std::filesystem::path(FileSystems::GetApplicationPath()) / "package_index\\fnv1a_xmaterials.wni").string());
    AssetNameCache.LoadIndex((std::filesystem::path(FileSystems::GetApplicationPath()) / "package_index\\fnv1a_ximages.wni").string());
    AssetNameCache.LoadIndex((std::filesystem::path(FileSystems::GetApplicationPath()) / "package_index\\fnv1a_xsounds.wni").string());
    AssetNameCache.LoadIndex((std::filesystem::path(FileSystems::GetApplicationPath()) / "package_index\\fnv1a_xmodels.wni").string());
    AssetNameCache.LoadIndex((std::filesystem::path(FileSystems::GetApplicationPath()) / "package_index\\fnv1a_xanims.wni").string());
    StringCache.LoadIndex((std::filesystem::path(FileSystems::GetApplicationPath()) / "package_index\\fnv1a_string.wni").string());
}