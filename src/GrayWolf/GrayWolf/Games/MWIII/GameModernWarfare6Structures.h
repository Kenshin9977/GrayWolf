#pragma once
#include <stdint.h>

#pragma pack(push, 1)
struct MW6XModelLod
{
    uint64_t MeshPtr;
    uint64_t SurfsPtr;

    float LodDistance[3];

    uint16_t NumSurfs;
    uint16_t SurfacesIndex;

    uint8_t Padding[40];
};
#pragma pack(pop)

#pragma pack(push, 1)
struct MW6XModel
{
    uint64_t Hash;
    uint64_t NamePtr;
    uint16_t NumSurfaces;
    uint8_t NumLods;
    uint8_t MaxLods;

    uint8_t Padding[13];

    uint8_t NumBones;
    uint16_t NumRootBones;
    uint16_t UnkBoneCount;

    uint8_t Padding3[0xA2];

    uint64_t BoneIDsPtr;
    uint64_t ParentListPtr;
    uint64_t RotationsPtr;
    uint64_t TranslationsPtr;
    uint64_t PartClassificationPtr;
    uint64_t BaseMatriciesPtr;
    uint64_t UnknownPtr;
    uint64_t UnknownPtr2;
    uint64_t MaterialHandlesPtr;
    uint64_t ModelLods;

    uint8_t Padding4[120];
};
#pragma pack(pop)

// The multiplayer build's XModel (cod23-cod.exe 2026-09, Load_XModel 0x3b23e00, 0xe8 bytes): the
// skeleton moved to a packed-data struct at 0x70, the LODs behind a pointer at 0x98 (0x48 each, the
// count u8 at 0x12), the material slots at 0x90 (u16 at 0x10 of them). Offsets from the zonekit
// tables; MW6XModel above is the campaign build's.
#pragma pack(push, 1)
struct MW6XModelMP
{
    uint64_t Hash;
    uint64_t NamePtr;
    uint16_t NumSurfaces;
    uint8_t NumLods;
    uint8_t MaxLods;
    uint8_t Padding[0x54];
    uint64_t UnknownPtr;            // 0x68: 32 bytes
    uint64_t PackedDataPtr;         // 0x70: MW6XModelPackedData
    uint64_t UnknownPtr2;           // 0x78: 0x38 bytes
    uint64_t ScriptablePtr;         // 0x80
    uint64_t UnknownPtr3;           // 0x88: 0x30 bytes
    uint64_t MaterialHandlesPtr;    // 0x90
    uint64_t ModelLods;             // 0x98
    uint8_t Padding2[0x48];
};
#pragma pack(pop)
static_assert(sizeof(MW6XModelMP) == 0xe8, "MW6XModelMP is 0xe8 bytes");

// Its skeleton (0x80, 0x3b24b40): the bone count u8 at 0, the root count u8 at 1, the other bones'
// count u16 at 6; the parents (u16 per non-root bone) at 0x48, their rotations (4 x i16) at 0x50,
// translations (3 floats) at 0x58, the base matrices (32 bytes per bone) at 0x68, the bone name
// ids (u32 per bone) at 0x70
#pragma pack(push, 1)
struct MW6XModelPackedData
{
    uint8_t NumBones;
    uint8_t NumRootBones;
    uint8_t Unk2;
    uint8_t Unk3;
    uint16_t Unk4;
    uint16_t UnkBoneCount;
    uint64_t Ptrs[8];               // 0x08..0x40
    uint64_t ParentListPtr;         // 0x48
    uint64_t RotationsPtr;          // 0x50
    uint64_t TranslationsPtr;       // 0x58
    uint64_t PartClassificationPtr; // 0x60
    uint64_t BaseMatriciesPtr;      // 0x68
    uint64_t BoneIDsPtr;            // 0x70
    uint64_t UnknownPtr;            // 0x78
};
#pragma pack(pop)
static_assert(sizeof(MW6XModelPackedData) == 0x80, "MW6XModelPackedData is 0x80 bytes");

// The multiplayer build's Material (0x78, 0x3af4f80): the texture slot count u8 at 0x18, the slots
// {u8 semantic, u8 index} at 0x28, the image pointers (u8 at 0x1b of them) at 0x30
#pragma pack(push, 1)
struct MW6XMaterialMP
{
    uint64_t Hash;
    uint8_t Padding[16];
    uint8_t ImageCount;
    uint8_t Unk19;
    uint8_t Unk1A;
    uint8_t ImageTableCount;
    uint8_t Padding2[4];
    uint64_t TechsetPtr;            // 0x20
    uint64_t ImageSlotsPtr;         // 0x28
    uint64_t ImageTablePtr;         // 0x30
    uint64_t UnknownPtr;            // 0x38
    uint64_t ConstantsPtr;          // 0x40
    uint8_t Padding3[0x30];
};
#pragma pack(pop)
static_assert(sizeof(MW6XMaterialMP) == 0x78, "MW6XMaterialMP is 0x78 bytes");

#pragma pack(push, 1)
struct MW6XModelMesh
{
    uint64_t Hash;
    uint64_t SurfsPtr;
    uint64_t LODStreamKey;
    uint8_t Padding[0x8];

    uint64_t MeshBufferPointer;
    uint16_t NumSurfs;
    uint8_t Padding2[38];

};
#pragma pack(pop)

#pragma pack(push, 1)
struct MW6XModelMeshBufferInfo
{
    uint64_t BufferPtr; // 0 if the model is not loaded, otherwise a pointer, even if streamed
    uint32_t BufferSize;
    uint32_t Streamed;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct MW6XModelSurface
{
    uint16_t StatusFlag;
    uint16_t FaceCountOld;
    uint16_t PackedIndexTableCount;
    uint8_t PaddingFuck[12];
    uint8_t VertListCount;
    uint8_t Unk123[5];
    uint32_t VertexCount;
    uint32_t FacesCount;
    uint32_t UnkValue;

    uint16_t WeightCounts[8];
    uint8_t Padding1[8];
    float NewScale;
    uint32_t Offsets[14];
    uint64_t MeshBufferPointer;
    uint64_t RigidWeightsPtr;
    uint8_t Padding4[40];
    float XOffset;
    float YOffset;
    float ZOffset;
    float Scale;
    float Min;
    float Max;
    uint8_t Padding9[24];
};
#pragma pack(pop)

struct MW6GfxImage
{
    uint64_t Hash;
    uint8_t Unk00[16];
    uint32_t BufferSize;
    uint32_t Unk02;
    uint16_t Width;
    uint16_t Height;
    uint16_t Depth;
    uint16_t Levels;
    uint8_t UnkByte0;
    uint8_t UnkByte1;
    uint8_t ImageFormat;
    uint8_t UnkByte3;
    uint8_t UnkByte4;
    uint8_t UnkByte5;
    uint8_t MipCount;
    uint8_t UnkByte7;
    uint64_t MipMaps;
    uint64_t LoadedImagePtr;
};

#pragma pack(push, 1)
struct MW6GfxMip
{
    uint64_t HashID;
    uint64_t Padding;
    uint64_t PackedInfo;
};
#pragma pack(pop)

template <size_t Size>
struct MW6GfxMipArray
{
    MW6GfxMip MipMaps[Size];

    size_t GetImageSize(size_t i)
    {
        if (i == 0)
            return ((MipMaps[i].PackedInfo >> 30) & 0x1FFFFFFF);
        else
            return ((MipMaps[i].PackedInfo >> 30) & 0x1FFFFFFF) - ((MipMaps[i - 1].PackedInfo >> 30) & 0x1FFFFFFF);
    }

    size_t GetMipCount()
    {
        return Size;
    }
};


#pragma pack(push, 1)
struct MW6XAnimStreamInfo
{
    uint64_t StreamKey;
    uint32_t Size;
    uint32_t Padding;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct MW6XAnimNotetrack
{
    uint32_t Name;
    float Time;
    uint8_t Padding[24];
};
#pragma pack(pop)

#pragma pack(push, 1)
struct MW6XAnimDataInfo
{
    int32_t DataByteOffset;
    int32_t DataShortOffset;
    int32_t DataIntOffset;
    int32_t Padding4;
    uint64_t OffsetPtr;
    uint64_t OffsetPtr2;
    uint32_t StreamIndex;
    uint32_t OffsetCount;
    uint64_t PackedInfoPtr;
    uint32_t PackedInfoCount;
    uint32_t PackedInfoCount2;
    uint64_t Flags;
    uint64_t StreamInfoPtr;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct MW6XAnim
{
    uint64_t Hash;
    uint64_t BoneIDsPtr;
    uint64_t IndicesPtr;
    uint64_t NotificationsPtr;
    uint64_t DeltaPartsPtr;
    uint8_t Padding[16];
    uint32_t RandomDataShortCount;
    uint32_t RandomDataByteCount;
    uint32_t IndexCount;

    float Framerate;
    float Frequency;

    uint32_t DataByteCount;
    uint16_t DataShortCount;
    uint16_t DataIntCount;
    uint16_t RandomDataIntCount;
    uint16_t FrameCount;

    uint8_t Padding2[6];

    uint16_t NoneRotatedBoneCount;
    uint16_t TwoDRotatedBoneCount;
    uint16_t NormalRotatedBoneCount;
    uint16_t TwoDStaticRotatedBoneCount;
    uint16_t NormalStaticRotatedBoneCount;
    uint16_t NormalTranslatedBoneCount;
    uint16_t PreciseTranslatedBoneCount;
    uint16_t StaticTranslatedBoneCount;
    uint16_t NoneTranslatedBoneCount;
    uint16_t TotalBoneCount;

    uint8_t NotetrackCount;
    uint8_t AssetType;

    uint8_t Padding3[20];

    MW6XAnimDataInfo DataInfo;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct MW6XAnimDeltaParts
{
    uint64_t DeltaTranslationsPtr;
    uint64_t Delta2DRotationsPtr;
    uint64_t Delta3DRotationsPtr;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct MW6XMaterialSP
{
    uint64_t Hash;
    uint8_t Padding[16];
    uint8_t ImageCount;
    uint8_t Padding2[15];
    uint64_t TechsetPtr;
    uint64_t ImageTablePtr;
    uint8_t Padding3[56];
};
#pragma pack(pop)

#pragma pack(push, 1)
struct MW6XMaterialImage
{
    uint32_t Type;
    uint8_t Padding[4];
    uint64_t ImagePtr;
};
#pragma pack(pop)

extern uint32_t MW6DXGIFormats[52];

#pragma pack(push, 1)
struct MW6SndAsset
{
    uint64_t Name;
    uint64_t Unk;
    uint64_t StreamKeyEx;
    uint64_t StreamKey;
    uint32_t Size;
    uint32_t Unk3;
    uint64_t Unk4;
    uint64_t Unk5;
    uint32_t SeekTableSize;
    uint32_t LoadedSize;
    uint32_t FrameCount;
    uint32_t FrameRate;
    uint32_t Unk9;
    uint8_t Unk10;
    uint8_t Unk11;
    uint8_t Unk12;
    uint8_t ChannelCount;
    uint64_t Unk13;
};
#pragma pack(pop)
static_assert(sizeof(MW6XModelLod) == 0x48, "MW6XModelLod is 0x48 bytes");
static_assert(sizeof(MW6XModelMesh) == 0x50, "MW6XModelMesh (xmodelsurfs) is 0x50 bytes");
static_assert(sizeof(MW6XModelSurface) == 0xe0, "MW6XModelSurface is 0xe0 bytes");
static_assert(sizeof(MW6GfxImage) == 0x40, "MW6GfxImage is 0x40 bytes");
