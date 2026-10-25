#include "stdafx.h"

#include "GameBlackOps6.h"

#include "CoDAssets.h"
#include "CoDRawImageTranslator.h"
#include "DBGameGenerics.h"
#include "FileSystems.h"
#include "Games/Common/MeshDecode.h"
#include "Games/IW/IwctxReaders.h"

#include "SABSupport.h"
#include "SettingsManager.h"
#include "Strings.h"
#include "WraithNameIndex.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <mutex>

// Black Ops 6 (cod24). Offsets: the bo6 profile's reader tables; meanings: C2M's BlackOps6.cs,
// checked on the install's data.
namespace
{
    constexpr uint64_t NameHashMask = 0x7FFFFFFFFFFFFFFFull;   // 63-bit name hashes (bit 63: a reference)

    // GfxImage format -> DXGI (C2M BlackOps6.BO6DXGIFormats)
    // the campaign's (sp24-cod.exe 0x8d38780, u32 each): the multiplayer's without its 8, 9 and 30
    constexpr uint8_t CampaignImageFormats[53] = { 0,  61, 65, 61, 49, 49, 28, 29, 63, 51, 56, 35, 11, 58, 37, 13, 54, 34,
                                                   10, 41, 16, 2,  55, 40, 20, 62, 57, 42, 17, 12, 3,  25, 85, 115, 24, 67,
                                                   26, 71, 72, 74, 75, 77, 78, 80, 83, 84, 95, 96, 98, 99, 31, 0,  0 };
    constexpr uint8_t ImageFormats[56] = { 0,  61, 65, 61, 49, 49, 28, 29, 87, 91, 63, 51, 56, 35, 11, 58, 37, 13, 54,
                                           34, 10, 41, 16, 2,  55, 40, 20, 62, 57, 42, 36, 17, 12, 3,  25, 85, 115, 24,
                                           67, 26, 71, 72, 74, 75, 77, 78, 80, 83, 84, 95, 96, 98, 99, 31, 0,  0 };

    // The skeleton (0x90, loader 0x288c5a0): bone counts u8 0 (regular), u8 1 (roots), u16 6
    // (cosmetic); parents u16 0x50, rotations 0x58, translations 0x60, DObjAnimMat 0x70, FNV-1a 64 names 0x78
    constexpr int64_t SkelBones = 0, SkelRoots = 1, SkelCosmetic = 6, SkelParents = 0x50, SkelRotations = 0x58,
                      SkelTranslations = 0x60, SkelBase = 0x70, SkelNames = 0x78;

    // LOD (0x48): xmodelsurfs 0, surface count u16 0x1c, first material slot u16 0x1e
    constexpr int64_t LodModelSurfs = 0, LodSurfCount = 0x1c, LodFirstSurface = 0x1e;
    // XModelSurfs (0x40): surfaces (0xe0) at 8, stream info at 0x10
    constexpr int64_t SurfsSurfaces = 8, SurfsStream = 0x10, SurfaceSize = 0xe0;
    // Stream info (0x18): in-zone data 0 (size u32 8), else the .xsub key 8, size u32 0x10
    constexpr int64_t StreamData = 0, StreamInZoneSize = 8, StreamKey = 8, StreamSize = 0x10;

    // XSurface (0xe0, loader 0x288d620); Offsets into the mesh buffer: positions 0, uvs 1,
    // tangent frames 2, indices 3, packed tables 4, packed indices 5, colours 6, weights 11
    #pragma pack(push, 1)
    struct Surface
    {
        uint16_t StatusFlag;
        uint16_t VertexCount0, FaceCount0, TableCount0;
        uint8_t Padding[6];
        uint8_t VertListCount;
        uint8_t Unk2;
        uint32_t Unk3;
        uint32_t VertexCount;
        uint32_t FaceCount;
        uint32_t TableCount;
        uint32_t Unk4;
        uint16_t WeightCounts[8];
        uint8_t Padding1[8];
        float NewScale;
        uint32_t Offsets[14];
        uint64_t StreamInfo;
        uint64_t RigidWeights;
        uint8_t Padding4[40];
        float PosOffset[3];
        float Scale;
        float Min;
        float Max;
        uint8_t Padding5[24];
    };
    static_assert(sizeof(Surface) == 0xe0, "XSurface is 0xe0");
    #pragma pack(pop)
    // A rigid run (16 bytes, the table's u8 0xe * 16; the campaign's 12): its bone u16 0, its vertex count u16 2

    // a served pointer: a reference the zone does not hold keeps its raw hash (bit 63 set)
    bool Valid(uint64_t Address)
    {
        return Address != 0 && (Address >> 48) == 0;
    }

    template <class T>
    T Read(uint64_t Address)
    {
        return CoDAssets::GameInstance->Read<T>(Address);
    }

    std::string BoneName(uint64_t Hash)
    {
        auto& Db = CoDAssets::StringCache.NameDatabase;
        if (const auto It = Db.find(Hash); It != Db.end()) return It->second;
        if (const auto It = Db.find(Hash & NameHashMask); It != Db.end()) return It->second;
        return std::format("bone_{:x}", Hash & NameHashMask);
    }

    // The bones (CoDXModelBonesHelper with 64-bit names)
    void ReadBones(const std::unique_ptr<XModel_t>& Model, const std::unique_ptr<WraithModel>& Result)
    {
        const size_t Count = (size_t)Model->BoneCount + Model->CosmeticBoneCount;
        for (size_t i = 0; i < Count; i++)
        {
            auto& Bone = Result->AddBone();
            Bone.TagName = BoneName(Read<uint64_t>(Model->BoneIDsPtr + i * 8));
            if (i >= Model->RootBoneCount)
            {
                // relative, the cosmetic bones' too (each one's translation puts it at its base pose under
                // i - parent, never under the parent read as an index: 18 of 18 cosmetic bones of BO6's
                // weapon parts, att_t10_vm_p01_ar_coslo723_rec's 13 among them)
                Bone.BoneParent = (int32_t)i - (int32_t)Read<uint16_t>(Model->BoneParentsPtr + (i - Model->RootBoneCount) * 2);
                Bone.IsCosmetic = i >= Model->BoneCount;
            }
            else
                Bone.BoneParent = -1;
            const auto Global = Read<DObjAnimMat>(Model->BaseMatriciesPtr + i * sizeof(DObjAnimMat));
            Bone.GlobalPosition = Global.Translation;
            Bone.GlobalRotation = Global.Rotation;
        }
        if (Count == 0)
        {
            auto& Bone = Result->AddBone();
            Bone.TagName = "tag_origin";
            Bone.BoneParent = -1;
        }
        Result->GenerateLocalPositions(true, true);
    }

    // C2M's semantics: 44/82/96/110/124 colour, 45/83/97/111/125 normal-gloss-occlusion
    ImageUsageType UsageOf(uint8_t Semantic)
    {
        switch (Semantic)
        {
        case 44: case 82: case 96: case 110: case 124: return ImageUsageType::DiffuseMap;
        case 45: case 83: case 97: case 111: case 125: return ImageUsageType::NormalMap;
        default: return ImageUsageType::Unknown;
        }
    }
}

bool GameBlackOps6::UnpackFace(const uint8_t* Buffer, size_t Size, uint32_t TableCount, uint64_t Indices, uint64_t Tables, uint64_t PackedBase,
                           uint32_t Face, uint32_t (&Out)[3])
{
    return MeshDecode::UnpackDeltaFace(Buffer, Size, Tables, TableCount, PackedBase, Indices, Face, Out, Current->FaceTableSize, MeshDecode::Edge::Zero);
}

std::string GameBlackOps6::HashedName(const char* Type, uint64_t Hash)
{
    return IwctxReaders::HashedName(Type, Hash);
}

void GameBlackOps6::PerformInitialSetup()
{
    IwctxReaders::LoadNameIndexes();
}

const GameBlackOps6::Layout GameBlackOps6::Multiplayer{ ModelSkeleton, ModelMaterials, ModelLods, SoundFlags, SoundRate, 16, 32, SABSupport::OpusPreSkipLookAhead,
                                                        ImageFormats, (uint32_t)std::size(ImageFormats) };
const GameBlackOps6::Layout GameBlackOps6::Campaign{ 0x70, 0x98, 0xa0, 0x3c, 0x2c, 12, 28, SABSupport::OpusPreSkipPacket,
                                                     CampaignImageFormats, (uint32_t)std::size(CampaignImageFormats) };
thread_local const GameBlackOps6::Layout* GameBlackOps6::Current = &GameBlackOps6::Multiplayer;

std::unique_ptr<XImageDDS> GameBlackOps6::LoadXImageCampaign(const XImage_t& Image)
{
    LayoutScope S(Campaign);
    return LoadXImage(Image);
}

std::unique_ptr<XModel_t> GameBlackOps6::ReadXModel(const CoDModel_t* Model)
{
    const uint64_t At = Model->AssetPointer;
    const uint8_t LodCount = Read<uint8_t>(At + ModelLodCount);
    const uint16_t SurfaceCount = Read<uint16_t>(At + ModelSurfaces);
    auto Result = std::make_unique<XModel_t>(LodCount);
    Result->ModelName = Model->AssetName;
    Result->IsModelStreamed = true;
    Result->BoneRotationData = BoneDataTypes::DivideBySize;
    if (const uint64_t Skel = Read<uint64_t>(At + Current->ModelSkeleton); Skel != 0)
    {
        Result->BoneCount = Read<uint8_t>(Skel + SkelBones);
        Result->RootBoneCount = Read<uint8_t>(Skel + SkelRoots);
        Result->CosmeticBoneCount = Read<uint16_t>(Skel + SkelCosmetic);
        Result->BoneParentsPtr = Read<uint64_t>(Skel + SkelParents);
        Result->RotationsPtr = Read<uint64_t>(Skel + SkelRotations);
        Result->TranslationsPtr = Read<uint64_t>(Skel + SkelTranslations);
        Result->BaseMatriciesPtr = Read<uint64_t>(Skel + SkelBase);
        Result->BoneIDsPtr = Read<uint64_t>(Skel + SkelNames);
        Result->BoneIndexSize = 8;
        Result->BoneParentSize = 2;
        // a skeleton the zone does not hold whole: no bones
        if (Result->BoneIDsPtr == 0 || Result->BaseMatriciesPtr == 0 ||
            (Result->BoneCount + Result->CosmeticBoneCount > Result->RootBoneCount && Result->BoneParentsPtr == 0))
            Result->BoneCount = Result->CosmeticBoneCount = Result->RootBoneCount = 0;
    }
    const uint64_t Lods = Read<uint64_t>(At + Current->ModelLods);
    const uint64_t Materials = Read<uint64_t>(At + Current->ModelMaterials);
    uint32_t NextSurface = 0;
    for (uint32_t L = 0; L < LodCount; L++)
    {
        const uint64_t Lod = Lods + (uint64_t)L * LodSize;
        const uint64_t ModelSurfs = Read<uint64_t>(Lod + LodModelSurfs);
        const uint16_t Count = Read<uint16_t>(Lod + LodSurfCount);
        uint32_t First = Read<uint16_t>(Lod + LodFirstSurface);
        if (First + Count > SurfaceCount) First = NextSurface;
        NextSurface = First + Count;
        Result->ModelLods.emplace_back(Count);
        auto& Out = Result->ModelLods.back();
        Out.LodDistance = (float)L;
        Out.LODStreamInfoPtr = Valid(ModelSurfs) ? Read<uint64_t>(ModelSurfs + SurfsStream) : 0;
        const uint64_t Surfaces = Valid(ModelSurfs) ? Read<uint64_t>(ModelSurfs + SurfsSurfaces) : 0;
        for (uint32_t S = 0; S < Count && Surfaces != 0; S++)
        {
            Out.Submeshes.emplace_back();
            auto& Sub = Out.Submeshes.back();
            const auto Info = Read<Surface>(Surfaces + (uint64_t)S * SurfaceSize);
            Sub.VertListcount = Info.VertListCount;
            Sub.RigidWeightsPtr = Info.RigidWeights;
            Sub.VertexCount = Info.VertexCount;
            Sub.FaceCount = Info.FaceCount;
            Sub.PackedIndexTableCount = Info.TableCount;
            Sub.VertexPtr = Info.Offsets[0];
            Sub.VertexUVsPtr = Info.Offsets[1];
            Sub.VertexNormalsPtr = Info.Offsets[2];
            Sub.FacesPtr = Info.Offsets[3];
            Sub.PackedIndexTablePtr = Info.Offsets[4];
            Sub.PackedIndexBufferPtr = Info.Offsets[5];
            Sub.VertexColorPtr = Info.Offsets[6];
            Sub.WeightsPtr = Info.Offsets[11];
            if (Info.NewScale != -1.0f)
            {
                Sub.Scale = Info.NewScale;
                Sub.XOffset = Sub.YOffset = Sub.ZOffset = 0;
            }
            else
            {
                Sub.Scale = std::max(std::max(Info.Min, Info.Scale), Info.Max);
                Sub.XOffset = Info.PosOffset[0], Sub.YOffset = Info.PosOffset[1], Sub.ZOffset = Info.PosOffset[2];
            }
            std::memcpy(Sub.WeightCounts, Info.WeightCounts, sizeof(Sub.WeightCounts));
            Sub.MaterialIndex = (int32_t)S;
            const uint64_t Material = Materials != 0 && First + S < SurfaceCount ? Read<uint64_t>(Materials + (uint64_t)(First + S) * 8) : 0;
            Out.Materials.emplace_back(Valid(Material) ? ReadXMaterial(Material) : XMaterial_t(0));
        }
    }
    return Result;
}

void GameBlackOps6::LoadXModel(const std::unique_ptr<XModel_t>& Model, const XModelLod_t& ModelLOD, const std::unique_ptr<WraithModel>& ResultModel)
{
    ReadBones(Model, ResultModel);
    const uint64_t Info = ModelLOD.LODStreamInfoPtr;
    if (!Valid(Info)) return;
    // the mesh: in the zone, else the .xsub object by its key (sized by its index)
    MeshDecode::Buffer Mesh;
    if (const uint64_t InZone = Read<uint64_t>(Info + StreamData); InZone != 0)
    {
        const uint32_t Size = Read<uint32_t>(Info + StreamInZoneSize);
        Mesh = MeshDecode::FromZone(InZone, Size);
        if (Mesh.Size != Size) return;
    }
    else
        Mesh = MeshDecode::FromPackage(Read<uint64_t>(Info + StreamKey));
    if (Mesh.Data == nullptr || Mesh.Size == 0) return;
    // 21-bit positions, QTangents, half UVs, faces in the layout's delta tables (a corner past the
    // buffer's end reads 0); the layout's rigid runs
    const MeshDecode::LodFormat Format{ { MeshDecode::PositionCodec::Packed21, MeshDecode::NormalCodec::QTangent, MeshDecode::UvCodec::Half2,
                                          MeshDecode::FaceCodec::PackedTableDelta, Current->FaceTableSize, MeshDecode::Edge::Zero },
                                        { Current->RigidRunSize, 2 } };
    MeshDecode::DecodeLod(Mesh.View(), ModelLOD, Format, SettingsManager::GetSetting("exportvtxcolor", "true") == "true", *ResultModel);
}

const XMaterial_t GameBlackOps6::ReadXMaterial(uint64_t MaterialPointer)
{
    // Material (0x80, loader 0x2846c90): hash 0, sort key u8 0x19 (Games/IW/IwMaterialAlpha.h), slot
    // count u8 0x1c, image count u8 0x21, slots {u8 semantic, u8 image} at 0x30, images at 0x38
    const uint8_t SlotCount = Read<uint8_t>(MaterialPointer + 0x1c);
    const uint8_t ImageCount = Read<uint8_t>(MaterialPointer + 0x21);
    const uint64_t Slots = Read<uint64_t>(MaterialPointer + 0x30), Images = Read<uint64_t>(MaterialPointer + 0x38);
    XMaterial_t Result(SlotCount);
    Result.MaterialName = HashedName("xmaterial", Read<uint64_t>(MaterialPointer));
    Result.SortKey = Read<uint8_t>(MaterialPointer + 0x19);
    for (uint32_t K = 0; K < SlotCount && Slots != 0 && Images != 0; K++)
    {
        const uint8_t Semantic = Read<uint8_t>(Slots + K * 2), Index = Read<uint8_t>(Slots + K * 2 + 1);
        if (Index >= ImageCount) continue;
        const uint64_t Image = Read<uint64_t>(Images + (uint64_t)Index * 8);
        if (!Valid(Image)) continue;
        Result.Images.emplace_back(UsageOf(Semantic), Semantic, Image, HashedName("ximage", Read<uint64_t>(Image)));
    }
    return Result;
}

std::unique_ptr<XImageDDS> GameBlackOps6::ReadXImage(const CoDImage_t* Image)
{
    return LoadXImage(XImage_t(ImageUsageType::DiffuseMap, 0, Image->AssetPointer, Image->AssetName));
}

std::unique_ptr<XImageDDS> GameBlackOps6::LoadXImage(const XImage_t& Image)
{
    return IwctxReaders::LoadXImage(Image, Current->ImageFormats, Current->ImageFormatCount);
}

std::unique_ptr<XSound> GameBlackOps6::ReadXSound(const CoDSound_t* Sound)
{
    // primed (flags bits 3-4): the .xsub key at 8, else the frames the zone primes at 0x10 (size
    // u32 0x18); not primed: the key at 0x10
    const auto Where = [](uint8_t Flags) {
        IwctxReaders::SoundSource S;
        if (Flags & 0x18) S.Key = 8, S.ZoneFrames = 0x10, S.ZoneSize = 0x18;
        else S.Key = 0x10;
        return S;
    };
    return IwctxReaders::ReadXSound(Sound, { Current->SoundFlags, Current->SoundRate, Where, false, Current->SoundPreSkip });
}

// ---- xanim (read by IwctxReaders, shared with Black Ops 7)
namespace
{
    // The header (0x78): counts u32 8 (random shorts), 0xc (random bytes), 0x10 (u16 indices),
    // rate float 0x14, data bytes u32 0x1c, data shorts u16 0x20, data ints u16 0x22, frames u16
    // 0x26, a flag byte 0x2c (bit 0 looping), ten bone counts u16 from 0x2e (Modern Warfare's order,
    // the total last), the asset type u8 0x43; the stream list's count u8 0x5a and its 0x18-byte
    // entries at 0x70 ({data, u32 size} in the zone, else {0, .xsub key, u32 size}). Read by the
    // game's rules (GameRules): the sampler of cod24 0x87b4c30 / 0x87bb3a0 (sp24 0x51f9bb0 /
    // 0x51ff3d0), emulated, reads them as Black Ops 7's. Notetracks: count u8 0x42, names 0x50 (u16
    // count 0x58): multiplayer 64-bit script string hashes, the campaign zone string slots
    // (iterator cod24 0x2ecdd60, sp24 0x298b520)
    constexpr IwctxReaders::XAnimLayout AnimLayout{ 0x14, 0x26, 0x2c, 0x43, 0x2e, 0x5a, 0x70, true, 0x42, 0x50, 0x58, IwctxReaders::ScriptString };
    constexpr IwctxReaders::XAnimLayout CampaignAnimLayout{ 0x14, 0x26, 0x2c, 0x43, 0x2e, 0x5a, 0x70, true, 0x42, 0x50, 0x58, IwctxReaders::SlotScriptString, true };
    const IwctxReaders::XAnimLayout& AnimLayoutOf(const GameBlackOps6::Layout* L) { return L == &GameBlackOps6::Campaign ? CampaignAnimLayout : AnimLayout; }
}

std::unique_ptr<XAnim_t> GameBlackOps6::ReadXAnim(const CoDAnim_t* Animation)
{
    return IwctxReaders::ReadXAnim(Animation, AnimLayoutOf(Current), BoneName);
}

void GameBlackOps6::LoadXAnim(const std::unique_ptr<XAnim_t>& Anim, std::unique_ptr<WraithAnim>& Result)
{
    IwctxReaders::LoadXAnim(Anim, Result, AnimLayoutOf(Current));
}
