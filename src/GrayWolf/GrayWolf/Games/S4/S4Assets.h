#pragma once

// Vanguard's model, material and animation records as the s4 tables read them (wolff
// profiles/s4, the loaders of the dumped Vanguard.exe; sizes from GetXAssetTypeSize's table
// 0x7820340: xanim 168, xmodel 392, material 128). Each offset is one the tables name (a pointer
// they follow, a count they read) or one checked on the sample's instances (README.md).

#include <cstddef>
#include <cstdint>

#pragma pack(push, 1)
// XModel (0x188, Load_XModel 0x6c7c7f0): name 0; surface count u16 8 (the material handles' count,
// 0xc0); LOD count u8 0xa (the LOD records', 0xc8); bones u8 0x18, root bones u8 0x19, the
// unnamed bones after them u16 0x1a; bone names (script strings, u32) 0x80 (bones + unnamed),
// parents (u16) 0x88 (bones + unnamed - roots), rotations (4 x i16) 0x90, translations (3 x f32)
// 0x98, part classification 0xa0 (bones), base matrices (0x20) 0xa8; material handles 0xc0; the
// LOD records' pointer 0xc8
struct S4XModel
{
    uint64_t NamePtr;
    uint16_t NumSurfaces;
    uint8_t NumLods;
    uint8_t MaxLods;
    uint8_t Padding[12];
    uint8_t NumBones;
    uint8_t NumRootBones;
    uint16_t UnkBoneCount;
    uint8_t Padding3[100];
    uint64_t BoneIDsPtr;
    uint64_t ParentListPtr;
    uint64_t RotationsPtr;
    uint64_t TranslationsPtr;
    uint64_t PartClassificationPtr;
    uint64_t BaseMatriciesPtr;
    uint64_t UnknownPtr;
    uint64_t UnknownPtr2;
    uint64_t MaterialHandlesPtr;
    uint64_t ModelLodsPtr;
    uint8_t Padding4[184];
};
static_assert(sizeof(S4XModel) == 0x188);
static_assert(offsetof(S4XModel, NumBones) == 0x18 && offsetof(S4XModel, BoneIDsPtr) == 0x80 && offsetof(S4XModel, MaterialHandlesPtr) == 0xc0 &&
              offsetof(S4XModel, ModelLodsPtr) == 0xc8);

// A LOD record (0x40, the xmodel's 0xc8): its xmodelsurfs 0 (an asset), the surfaces the game
// copies from it at load time 8 (empty in the zone), its distance f32 0x10, surface count u16 0x18
struct S4XModelLod
{
    uint64_t MeshPtr;
    uint64_t SurfsPtr;
    float LodDistance;
    float LodDistanceAgain;
    uint16_t NumSurfs;
    uint16_t SurfacesIndex;
    uint8_t Padding[36];
};
static_assert(sizeof(S4XModelLod) == 0x40);

// XModelSurfs (0x50, 0x6c7dc80): name 0, surfaces 8 (count u16 0x28, 0xd0 each), the .xsub key of
// the LOD's mesh 0x10, its buffer record 0x20
struct S4XModelMesh
{
    uint64_t NamePtr;
    uint64_t SurfsPtr;
    uint64_t LODStreamKey;
    uint64_t Padding;
    uint64_t MeshBufferPointer;
    uint16_t NumSurfs;
    uint8_t Padding2[38];
};
static_assert(sizeof(S4XModelMesh) == 0x50 && offsetof(S4XModelMesh, NumSurfs) == 0x28);

// The mesh buffer record (0x10, 0x6c75b10): its bytes in the zone at 0 (u32 8 of them) unless u8
// 0xc & 1 (streamed)
struct S4XModelMeshBufferInfo
{
    uint64_t BufferPtr;
    uint32_t BufferSize;
    uint32_t Streamed;
};
static_assert(sizeof(S4XModelMeshBufferInfo) == 0x10);

// A surface (0xd0, 0x6c75670): vertex count u16 2, triangle count u16 4, rigid run count u8 8
// (8 bytes each at 0x68), skinned vertex counts by influence u16 0x1c..0x2b, the skinned weights'
// size u32 0x2c (their bytes at 0x70), a scale f32 0x30 (-1: use the box), the streams' offsets in
// the mesh buffer u32 0x34.. (positions, UVs, normals, faces, ..., colours 6), its buffer record
// 0x58, the box 0xa0..0xb7, blend shapes 0xc0
struct S4XModelSurface
{
    uint16_t StatusFlag;
    uint16_t VertexCount;
    uint16_t FacesCount;
    uint16_t Padding6;
    uint8_t VertListCount;
    uint8_t Padding0[19];
    uint16_t WeightCounts[8];
    uint32_t WeightsSize;
    float NewScale;
    uint32_t Offsets[9];
    uint64_t MeshBufferPointer;
    uint64_t Padding3;
    uint64_t RigidWeightsPtr;
    uint64_t WeightsPtr;
    uint8_t Padding4[40];
    float XOffset;
    float YOffset;
    float ZOffset;
    float Scale;
    float Min;
    float Max;
    uint64_t Padding7;
    uint64_t BlendShapesPtr;
    uint64_t Padding8;
};
static_assert(sizeof(S4XModelSurface) == 0xd0);
static_assert(offsetof(S4XModelSurface, WeightCounts) == 0x1c && offsetof(S4XModelSurface, NewScale) == 0x30 && offsetof(S4XModelSurface, Offsets) == 0x34 &&
              offsetof(S4XModelSurface, MeshBufferPointer) == 0x58 && offsetof(S4XModelSurface, RigidWeightsPtr) == 0x68 &&
              offsetof(S4XModelSurface, XOffset) == 0xa0 && offsetof(S4XModelSurface, BlendShapesPtr) == 0xc0);

// Material (0x80, 0x6c63950): name 0, image count u8 0x1c, techset 0x30, the 16-byte image slots
// {u32 semantic, image* 8} 0x38
struct S4XMaterial
{
    uint64_t NamePtr;
    uint8_t Padding[20];
    uint8_t ImageCount;
    uint8_t Padding2[19];
    uint64_t TechsetPtr;
    uint64_t ImageTablePtr;
    uint8_t Padding3[64];
};
static_assert(sizeof(S4XMaterial) == 0x80 && offsetof(S4XMaterial, ImageCount) == 0x1c && offsetof(S4XMaterial, ImageTablePtr) == 0x38);

struct S4XMaterialImage
{
    uint32_t Type;
    uint8_t Padding[4];
    uint64_t ImagePtr;
};
static_assert(sizeof(S4XMaterialImage) == 0x10);

// XAnimParts (0xa8, 0x6c45580): name 0; bone names (u32) 8 (count u8 0x8a); data bytes 0x10 (u32
// 0x74), shorts 0x18 (u16 0x78), ints 0x20 (u16 0x7a), random shorts 0x28 (u32 0x60), random bytes
// 0x30 (u32 0x64), random ints 0x38 (u16 0x7c), indices 0x40 (u32 0x68; two bytes each from 256
// frames: u16 0x7e the frame count), notetracks 0x48 (u8 0x8b, 16 bytes each), delta parts 0x50
// (three sub-records), blend shape names 0x98 (u16 0x90) and weights 0xa0 ((frames + 1) x names)
struct S4XAnim
{
    uint64_t NamePtr;
    uint64_t BoneIDsPtr;
    uint64_t DataBytePtr;
    uint64_t DataShortPtr;
    uint64_t DataIntPtr;
    uint64_t RandomDataShortPtr;
    uint64_t RandomDataBytePtr;
    uint64_t RandomDataIntPtr;
    uint64_t LongIndiciesPtr;
    uint64_t NotificationsPtr;
    uint64_t DeltaPartsPtr;
    uint64_t UnknownPartsPtr;
    uint32_t RandomDataShortCount;
    uint32_t RandomDataByteCount;
    uint32_t IndexCount;
    float Framerate;
    float Frequency;
    uint32_t DataByteCount;
    uint16_t DataShortCount;
    uint16_t DataIntCount;
    uint16_t RandomDataIntCount;
    uint16_t NumFrames;
    uint8_t Flags;
    uint8_t NoneRotatedBoneCount;
    uint8_t TwoDRotatedBoneCount;
    uint8_t NormalRotatedBoneCount;
    uint8_t TwoDStaticRotatedBoneCount;
    uint8_t NormalStaticRotatedBoneCount;
    uint8_t NormalTranslatedBoneCount;
    uint8_t PreciseTranslatedBoneCount;
    uint8_t StaticTranslatedBoneCount;
    uint8_t NoneTranslatedBoneCount;
    uint8_t TotalBoneCount;
    uint8_t NotificationCount;
    uint8_t AssetType;
    uint8_t Padding3[3];
    uint16_t BlendShapeCount;
    uint8_t Padding4[6];
    uint64_t BlendShapeNamesPtr;
    uint64_t BlendShapeWeightsPtr;
};
static_assert(sizeof(S4XAnim) == 0xa8);
static_assert(offsetof(S4XAnim, DeltaPartsPtr) == 0x50 && offsetof(S4XAnim, RandomDataShortCount) == 0x60 && offsetof(S4XAnim, Framerate) == 0x6c &&
              offsetof(S4XAnim, DataByteCount) == 0x74 && offsetof(S4XAnim, NumFrames) == 0x7e && offsetof(S4XAnim, TotalBoneCount) == 0x8a &&
              offsetof(S4XAnim, BlendShapeCount) == 0x90 && offsetof(S4XAnim, BlendShapeNamesPtr) == 0x98);

struct S4XAnimDeltaParts
{
    uint64_t DeltaTranslationsPtr;
    uint64_t Delta2DRotationsPtr;
    uint64_t Delta3DRotationsPtr;
};
#pragma pack(pop)
