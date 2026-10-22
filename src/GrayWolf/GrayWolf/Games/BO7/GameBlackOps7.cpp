#include "stdafx.h"

#include "GameBlackOps7.h"

#include "CoDAssets.h"
#include "CoDRawImageTranslator.h"
#include "DBGameGenerics.h"
#include "Games/Common/MeshDecode.h"
#include "Games/IW/IwctxReaders.h"
#include "MapLive.h"
#include "SABSupport.h"
#include "SettingsManager.h"
#include "Strings.h"
#include "WraithNameIndex.h"

#include <algorithm>
#include <cmath>
#include <format>

// Every layout below is the one the bo7 tables walk (zonekit/profiles/bo7/tables.generated.txt,
// the loader named beside each struct) and the bytes of the working set's zones confirm.
namespace
{
#pragma pack(push, 1)
    // XModel (0xd8, loader 0x38c8c50): name hash 0, name 8, surface count u16 0x10, LOD count u8
    // 0x12, bounds (midpoint, half size) 0x30, skeleton 0x60 (0x98), material handles 0x88 (u16 0x10
    // of them, each an asset pointer by hash), LODs 0x90 (0x48 each)
    struct BO7XModel
    {
        uint64_t Hash;
        uint64_t NamePtr;
        uint16_t NumSurfaces;
        uint8_t NumLods;
        uint8_t Pad13[0x5d - 0x13];
        uint8_t Pad5d[0x60 - 0x5d];
        uint64_t SkeletonPtr;
        uint8_t Pad68[0x88 - 0x68];
        uint64_t MaterialHandlesPtr;
        uint64_t ModelLods;
        uint8_t Pad98[0xd8 - 0x98];
    };
    static_assert(sizeof(BO7XModel) == 0xd8, "XModel 0xd8");

    // The skeleton (0x98, 0x38c9e40): u8 0 the cosmetic bones?, u8 1 the root bones, u16 6 the
    // bones; parents (u16, one per non-root bone) 0x50, rotations 0x58, translations 0x60, the
    // bones' base matrices (DObjAnimMat, 32 bytes) 0x70, their name hashes (FNV-1a 64) 0x78. The
    // matrices and names count u16 6 + u8 0 bones, the parents that minus u8 1.
    struct BO7XSkeleton
    {
        uint8_t ExtraBones;
        uint8_t RootBones;
        uint8_t Pad2[4];
        uint16_t NumBones;
        uint8_t Pad8[0x50 - 8];
        uint64_t ParentsPtr;
        uint64_t RotationsPtr;
        uint64_t TranslationsPtr;
        uint64_t Unk68;
        uint64_t BaseMatricesPtr;
        uint64_t BoneNamesPtr;
        uint8_t Pad80[0x98 - 0x80];
    };
    static_assert(sizeof(BO7XSkeleton) == 0x98, "skeleton 0x98");

    // XModelLodInfo (0x48, 0x38cad40 for the xmodelsurfs at 0): the game copies the surfaces of the
    // xmodelsurfs into 8 at load (the zone leaves it 0: DiskGame links it)
    struct BO7XModelLod
    {
        uint64_t ModelSurfsPtr;
        uint64_t SurfsPtr;
        float LodDistance[3];
        uint16_t NumSurfs;
        uint16_t SurfacesIndex;
        uint8_t Pad20[0x48 - 0x20];
    };
    static_assert(sizeof(BO7XModelLod) == 0x48, "XModelLodInfo 0x48");

    // XModelSurfs (0x48, 0x38caa50): name hash 0, surfaces 8 (u16 0x20 of them, 0xf0 each), the
    // mesh buffer's stream info 0x10 (0x18: in-memory pointer 0, package key 8, size u32 0x10)
    struct BO7XModelSurfs
    {
        uint64_t Hash;
        uint64_t SurfsPtr;
        uint64_t StreamInfoPtr;
        uint64_t Unk18;
        uint16_t NumSurfs;
        uint8_t Pad22[0x48 - 0x22];
    };
    static_assert(sizeof(BO7XModelSurfs) == 0x48, "XModelSurfs 0x48");

    struct BO7StreamInfo
    {
        uint64_t BufferPtr;
        uint64_t StreamKey;
        uint32_t BufferSize;
        uint32_t Streamed;
    };
    static_assert(sizeof(BO7StreamInfo) == 0x18, "stream info 0x18");

    // XSurface (0xf0, 0x38cb000): vertex, face and packed-index-table counts u32 0x14, 0x18, 0x1c;
    // its vertex lists u8 0xc (rigid weights 16 bytes each at 0x90); weight counts u16 x 8 at 0x24;
    // a scale 0x3c (-1: none, use the bounds); 16 offsets into the mesh buffer 0x40 (positions 0,
    // uvs 1, tangent frames 2, indices 3, packed index tables 4, packed indices 5, colours 6, a
    // second uv 8, the skinned weights 14: Offsets[11] is 0 on every surface; on all 23 skinned
    // surfaces of the M15 mag and viewhands_mp_base_iw8, Offsets[14] is set and u32 0x34 is the
    // weights' byte size, sum of 4 * (g + 1) * WeightCounts[g]); the bounds (midpoint, half size) 0xc0
    struct BO7XSurface
    {
        uint16_t StatusFlag;
        uint16_t VertexCount16;
        uint16_t FacesCount16;
        uint16_t PackedIndexTableCount16;
        uint32_t Unk8;
        uint8_t VertListCount;
        uint8_t SubdivCount;
        uint8_t PadE[2];
        uint32_t Unk10;
        uint32_t VertexCount;
        uint32_t FacesCount;
        uint32_t PackedIndexTableCount;
        uint32_t Unk20;
        uint16_t WeightCounts[8];
        uint32_t WeightsSize;
        uint8_t Pad38[4];
        float NewScale;
        uint32_t Offsets[16];
        uint64_t MeshBufferPtr;
        uint64_t Unk88;
        uint64_t RigidWeightsPtr;
        uint8_t Pad98[0xc0 - 0x98];
        float Mid[3];
        float Half[3];
        uint8_t PadD8[0xf0 - 0xd8];
    };
    static_assert(sizeof(BO7XSurface) == 0xf0, "XSurface 0xf0");

    // Material (0x78, 0x386e2d0): name hash 0, techset 0x20, the image index table 0x28 (u8 0x68
    // records of 2 bytes: semantic, image index), the images 0x30 (u8 0x6d asset pointers by hash)
    struct BO7XMaterial
    {
        uint64_t Hash;
        uint8_t Pad8[0x20 - 8];
        uint64_t TechsetPtr;
        uint64_t ImageIndexPtr;
        uint64_t ImageTablePtr;
        uint8_t Pad38[0x68 - 0x38];
        uint8_t ImageIndexCount;
        uint8_t Pad69[4];
        uint8_t ImageCount;
        uint8_t Pad6e[0x78 - 0x6e];
    };
    static_assert(sizeof(BO7XMaterial) == 0x78, "Material 0x78");

#pragma pack(pop)

    // The game's image formats to DXGI (C2M BO7DXGIFormats)
    constexpr uint8_t BO7DXGIFormats[72] = {
        0, 61, 65, 61, 49, 49, 28, 29, 87, 91, 63, 51, 56, 35, 11, 58, 37, 13, 54, 34,
        10, 41, 16, 2,  55, 40, 20, 62, 57, 42, 36, 17, 12, 3,  25, 85, 115, 24, 0, 67,
        26, 71, 72, 74, 75, 77, 78, 80, 83, 84, 95, 96, 98, 99, 31, 66, 0, 0, 0, 0,
        0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0 };

    // A material image's semantic -> how the exporter uses it: the first layer's colour 0x47 and nog
    // 0x48 (MaterialAdapter.cpp). C2M's table (82 + 14 per layer) is Black Ops 6's: in Black Ops 7, 97
    // (0x61) is a greyscale mask on most weapon materials, not a normal map. 44/45 kept from it.
    ImageUsageType UsageOf(uint8_t Semantic)
    {
        switch (Semantic)
        {
        case 44: case 0x47: return ImageUsageType::DiffuseMap;
        case 45: case 0x48: return ImageUsageType::NormalMap;
        default: return ImageUsageType::Unknown;
        }
    }

    template <class T>
    T Read(uint64_t Address) { return CoDAssets::GameInstance->Read<T>(Address); }

    // A pointer the served zone holds (a relocated one): an asset named by hash and defined in
    // another zone, or a rewind key no earlier load registered, stays its raw value
    bool Mapped(uint64_t Address)
    {
        uint8_t B = 0;
        return Address != 0 && CoDAssets::GameInstance->Read(&B, Address, 1) == 1;
    }
}

uint64_t GameBlackOps7::HashIWAsset(const std::string& Name)
{
    return IwctxReaders::AssetHash(Name);
}

std::string GameBlackOps7::HashName(const char* Prefix, uint64_t Hash)
{
    return IwctxReaders::HashedName(Prefix, Hash);
}

std::string GameBlackOps7::BoneName(uint64_t Hash)
{
    const auto Found = CoDAssets::StringCache.NameDatabase.find(Hash);
    if (Found != CoDAssets::StringCache.NameDatabase.end()) return Found->second;
    return std::format("bone_{:x}", Hash);
}

std::unique_ptr<XModel_t> GameBlackOps7::ReadXModel(const CoDModel_t* Model)
{
    const auto Data = Read<BO7XModel>(Model->AssetPointer);
    auto Asset = std::make_unique<XModel_t>(Data.NumLods);
    Asset->ModelName = Model->AssetName;
    Asset->BoneRotationData = BoneDataTypes::DivideBySize;
    Asset->IsModelStreamed = true;
    if (Mapped(Data.SkeletonPtr))
    {
        const auto Skel = Read<BO7XSkeleton>(Data.SkeletonPtr);
        Asset->BoneCount = (uint32_t)Skel.NumBones + Skel.ExtraBones;
        Asset->RootBoneCount = Skel.RootBones;
        Asset->BoneIDsPtr = Skel.BoneNamesPtr;
        Asset->BoneIndexSize = 8;
        Asset->BoneParentsPtr = Skel.ParentsPtr;
        Asset->BoneParentSize = 2;
        Asset->RotationsPtr = Skel.RotationsPtr;
        Asset->TranslationsPtr = Skel.TranslationsPtr;
        Asset->BaseMatriciesPtr = Skel.BaseMatricesPtr;
    }
    uint64_t Handles = Data.MaterialHandlesPtr;
    for (uint32_t L = 0; L < Data.NumLods && Mapped(Data.ModelLods); L++)
    {
        const auto Lod = Read<BO7XModelLod>(Data.ModelLods + (uint64_t)L * sizeof(BO7XModelLod));
        Asset->ModelLods.emplace_back(Lod.NumSurfs);
        auto& Ref = Asset->ModelLods.back();
        Ref.LodDistance = (float)L;
        Ref.LODStreamInfoPtr = Lod.ModelSurfsPtr;   // the xmodelsurfs: LoadXModel reads its stream info
        uint64_t Surface = Lod.SurfsPtr;
        // a LOD whose surfaces are in no zone served (defined in a zone loaded before this one) has none
        const uint32_t NumSurfs = Mapped(Lod.SurfsPtr) ? std::min<uint32_t>(Lod.NumSurfs, Data.NumSurfaces) : 0;
        for (uint32_t S = 0; S < NumSurfs; S++, Surface += sizeof(BO7XSurface))
        {
            Ref.Submeshes.emplace_back();
            auto& Sub = Ref.Submeshes.back();
            const auto Info = Surface != 0 ? Read<BO7XSurface>(Surface) : BO7XSurface{};
            Sub.RigidWeightsPtr = Info.RigidWeightsPtr;
            Sub.VertListcount = Info.VertListCount;
            Sub.VertexCount = Info.VertexCount;
            Sub.FaceCount = Info.FacesCount;
            Sub.PackedIndexTableCount = Info.PackedIndexTableCount;
            Sub.VertexPtr = Info.Offsets[0];
            Sub.VertexUVsPtr = Info.Offsets[1];
            Sub.VertexNormalsPtr = Info.Offsets[2];
            Sub.FacesPtr = Info.Offsets[3];
            Sub.PackedIndexTablePtr = Info.Offsets[4];
            Sub.PackedIndexBufferPtr = Info.Offsets[5];
            Sub.VertexColorPtr = Info.Offsets[6];
            Sub.WeightsPtr = Info.Offsets[14];
            if (Info.NewScale != -1.0f)
            {
                Sub.Scale = Info.NewScale;
                Sub.XOffset = Sub.YOffset = Sub.ZOffset = 0;
            }
            else
            {
                Sub.Scale = std::max({ Info.Half[0], Info.Half[1], Info.Half[2] });
                Sub.XOffset = Info.Mid[0], Sub.YOffset = Info.Mid[1], Sub.ZOffset = Info.Mid[2];
            }
            for (int W = 0; W < 8; W++) Sub.WeightCounts[W] = Info.WeightCounts[W];
            Sub.MaterialIndex = -1;
            const uint64_t Material = Handles != 0 ? Read<uint64_t>(Handles) : 0;
            Ref.Materials.emplace_back(ReadXMaterial(Material));
            Handles += Handles != 0 ? 8 : 0;
        }
    }
    return Asset;
}

const XMaterial_t GameBlackOps7::ReadXMaterial(uint64_t MaterialPointer)
{
    if (MaterialPointer == 0 || !Mapped(MaterialPointer))
    {
        XMaterial_t None(0);
        None.MaterialName = HashName("xmaterial", MaterialPointer);
        return None;
    }
    const auto Data = Read<BO7XMaterial>(MaterialPointer);
    XMaterial_t Result(Data.ImageIndexCount);
    Result.MaterialName = HashName("xmaterial", Data.Hash);
    if (Data.TechsetPtr != 0 && Mapped(Data.TechsetPtr))
        Result.TechsetName = HashName("xtechset", Read<uint64_t>(Data.TechsetPtr));
    for (uint32_t K = 0; K < Data.ImageIndexCount && Data.ImageIndexPtr != 0 && Data.ImageTablePtr != 0; K++)
    {
        const uint8_t Semantic = Read<uint8_t>(Data.ImageIndexPtr + K * 2);
        const uint8_t Index = Read<uint8_t>(Data.ImageIndexPtr + K * 2 + 1);
        if (Index >= Data.ImageCount) continue;
        const uint64_t Image = Read<uint64_t>(Data.ImageTablePtr + (uint64_t)Index * 8);
        if (Image == 0 || !Mapped(Image)) continue;
        // the engine's fill-ins for a slot the material leaves empty ($black, $white,
        // $identitypackednog in code_post_gfx, as C2M skips them): not the material's own
        static const uint64_t Defaults[] = { HashIWAsset("$black"), HashIWAsset("$white"), HashIWAsset("$identitypackednog") };
        const uint64_t Hash = Read<uint64_t>(Image) & 0x7FFFFFFFFFFFFFFFull;
        if (std::find(std::begin(Defaults), std::end(Defaults), Hash) != std::end(Defaults)) continue;
        const std::string Name = HashName("ximage", Read<uint64_t>(Image));
        Result.Images.emplace_back(UsageOf(Semantic), Semantic, Image, Name);
    }
    return Result;
}

// sndasset (0x40, loader 0x38924d0): flags u8 0x30 (bits 4-5 not 0: its frames in the .xsub object
// whose key is at 8; else in the zone at 8, their size u32 0x10), rate u16 0x32
std::unique_ptr<XSound> GameBlackOps7::ReadXSound(const CoDSound_t* Sound)
{
    static const IwctxReaders::SoundLayout Layout{ SoundFlags, SoundRate,
                                                   [](uint8_t Flags) {
                                                       IwctxReaders::SoundSource S;
                                                       if ((Flags & 0x30) >= 0x10) S.Key = SoundKey;
                                                       else S.ZoneFrames = SoundKey, S.ZoneSize = 0x10;
                                                       return S;
                                                   },
                                                   0, true, SABSupport::OpusPreSkipLookAhead };
    return IwctxReaders::ReadXSound(Sound, Layout);
}

std::unique_ptr<XImageDDS> GameBlackOps7::ReadXImage(const CoDImage_t* Image)
{
    return LoadXImage(XImage_t(ImageUsageType::DiffuseMap, 0, Image->AssetPointer, Image->AssetName));
}

std::unique_ptr<XImageDDS> GameBlackOps7::LoadXImage(const XImage_t& Image)
{
    return IwctxReaders::LoadXImage(Image, BO7DXGIFormats, (uint32_t)std::size(BO7DXGIFormats));
}

void GameBlackOps7::LoadXModel(const std::unique_ptr<XModel_t>& Model, const XModelLod_t& ModelLOD, const std::unique_ptr<WraithModel>& ResultModel)
{
    // ---- the bones (names FNV-1a 64; parents relative for the first BoneCount)
    const size_t BoneCount = Model->BoneCount;
    for (size_t I = 0; I < BoneCount; I++)
    {
        auto& Bone = ResultModel->AddBone();
        Bone.TagName = Model->BoneIDsPtr != 0 ? BoneName(Read<uint64_t>(Model->BoneIDsPtr + I * 8)) : std::string();
        if (Strings::IsNullOrWhiteSpace(Bone.TagName)) Bone.TagName = I == 0 ? "tag_origin" : std::format("no_tag_{}", I);
        if (I >= Model->RootBoneCount && Model->BoneParentsPtr != 0)
        {
            const int32_t Parent = (int32_t)Read<uint16_t>(Model->BoneParentsPtr + (I - Model->RootBoneCount) * 2);
            Bone.BoneParent = (int32_t)I - Parent;
        }
        else
            Bone.BoneParent = -1;
        if (Model->BaseMatriciesPtr != 0)
        {
            const auto Global = Read<DObjAnimMat>(Model->BaseMatriciesPtr + I * sizeof(DObjAnimMat));
            Bone.GlobalPosition = Global.Translation;
            Bone.GlobalRotation = Global.Rotation;
        }
    }
    if (BoneCount == 0)
    {
        auto& Bone = ResultModel->AddBone();
        Bone.TagName = "tag_origin";
        Bone.BoneParent = -1;
    }
    ResultModel->GenerateLocalPositions(true, true);

    // ---- the mesh buffer: in the zone when loaded, else the package object its stream info names
    if (FILE* Dbg = Maps::DebugFile())
        std::fprintf(Dbg, "[bo7 model] %s lod surfs %llx\n", Model->ModelName.c_str(), (unsigned long long)ModelLOD.LODStreamInfoPtr);
    if (ModelLOD.LODStreamInfoPtr == 0) return;
    const auto Surfs = Read<BO7XModelSurfs>(ModelLOD.LODStreamInfoPtr);
    if (Surfs.StreamInfoPtr == 0) return;
    const auto Stream = Read<BO7StreamInfo>(Surfs.StreamInfoPtr);
    const auto Mesh = Stream.BufferPtr != 0 ? MeshDecode::FromZone(Stream.BufferPtr, Stream.BufferSize) : MeshDecode::FromPackage(Stream.StreamKey);
    if (FILE* Dbg = Maps::DebugFile())
        std::fprintf(Dbg, "[bo7 model] %s surfs %llx stream info %llx: buffer %llx key %016llx size %u -> %llu\n", Model->ModelName.c_str(),
                     (unsigned long long)ModelLOD.LODStreamInfoPtr, (unsigned long long)Surfs.StreamInfoPtr, (unsigned long long)Stream.BufferPtr,
                     (unsigned long long)Stream.StreamKey, Stream.BufferSize, (unsigned long long)Mesh.Size);
    if (Mesh.Data == nullptr || Mesh.Size == 0) return;
    // 21-bit positions, QTangents, half UVs, faces in 32-byte delta tables; rigid runs of 16 bytes
    // (the vertex count at 6)
    constexpr MeshDecode::LodFormat Format{ { MeshDecode::PositionCodec::Packed21, MeshDecode::NormalCodec::QTangent, MeshDecode::UvCodec::Half2,
                                              MeshDecode::FaceCodec::PackedTableDelta, 32, MeshDecode::Edge::Partial }, { 16, 6 } };
    MeshDecode::DecodeLod(Mesh.View(), ModelLOD, Format, SettingsManager::GetSetting("exportvtxcolor", "true") == "true", *ResultModel);
}

// ---- xanim
namespace
{
    // XAnimParts (0x80, 0x38c21b0): name hash 0, rate float 0x14, the last frame's index u16 0x26
    // (frame = t * it), ten bone counts u16 from 0x2c (Black Ops 6's order, two bytes earlier), flags
    // u8 0x40 (bit 0 looping), asset type u8 0x42 (6 additive); the part at 0x48 (0x28: bone list
    // u32 0 / u16 8, the part count u8 0xa, notetracks 0x18 / u8 0x14, the part list 0x20). The
    // sampler (0xabf8eb0) reads the part count at 0x52 and the list at 0x68: {data, .xsub key, size}
    constexpr IwctxReaders::XAnimLayout AnimLayout{ 0x14, 0x26, 0x40, 0x42, 0x2c, 0x52, 0x68, true, 0x41, 0x48, 0x50, IwctxReaders::ScriptString };
}

std::unique_ptr<XAnim_t> GameBlackOps7::ReadXAnim(const CoDAnim_t* Animation)
{
    return IwctxReaders::ReadXAnim(Animation, AnimLayout, BoneName);
}

void GameBlackOps7::LoadXAnim(const std::unique_ptr<XAnim_t>& Anim, std::unique_ptr<WraithAnim>& Result)
{
    IwctxReaders::LoadXAnim(Anim, Result, AnimLayout);
}
