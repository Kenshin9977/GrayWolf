#pragma once

#include <string>

#include "WraithAnim.h"

#include "DBGameAssets.h"

#include "Image.h"
#include "CoDXAnimReader.h"

enum class AssetCompareMethod
{
    None,
    ByName,
    // Compares the assets by details if they are the same type, otherwise their name.
    ByDetails,
};

class AssetCompareMethodHelper
{
public:
    static AssetCompareMethod CalculateCompareMethod(const std::string& compareMethodStr);
};

class CoDAsset_t : public WraithAsset
{
public:
    CoDAsset_t();
    virtual ~CoDAsset_t();

    bool IsFileEntry;

    uint64_t AssetPointer;

    // The assets offset in the loaded pool
    uint32_t AssetLoadedIndex;

    // Compares the asset to this one, if compare details is enabled, they are used if the assets are the same type.
    virtual bool Compare(const CoDAsset_t* candidate, const AssetCompareMethod compareMethod) const;
};

class AssetPool
{
public:
    ~AssetPool();

    std::vector<CoDAsset_t*> LoadedAssets;
};

class CoDAnim_t : public CoDAsset_t
{
public:
    CoDAnim_t();
    virtual ~CoDAnim_t();

    std::vector<std::string> BoneNames;
    float Framerate;
    uint32_t FrameCount;
    uint32_t BoneCount;
    uint32_t ShapeCount;

    // Modern games
    bool isSiegeAnim;

    bool Compare(const CoDAsset_t* candidate, const AssetCompareMethod compareMethod) const;
};

class CoDModel_t : public CoDAsset_t
{
public:
    CoDModel_t();
    virtual ~CoDModel_t();

    std::vector<std::string> BoneNames;
    uint32_t BoneCount;
    uint32_t CosmeticBoneCount;
    uint16_t LodCount;

    bool Compare(const CoDAsset_t* candidate, const AssetCompareMethod compareMethod) const;
};

class CoDImage_t : public CoDAsset_t
{
public:
    CoDImage_t();
    virtual ~CoDImage_t();

    uint16_t Width;
    uint16_t Height;
    uint16_t Format;

    bool Compare(const CoDAsset_t* candidate, const AssetCompareMethod compareMethod) const;
};

class CoDRawFile_t : public CoDAsset_t
{
public:
    CoDRawFile_t();
    virtual ~CoDRawFile_t();

    std::string RawFilePath;
    uint64_t RawDataPointer;

    bool Compare(const CoDAsset_t* candidate, const AssetCompareMethod compareMethod) const;
};

// A class that represents an FX effect asset (FxEffectDef). Only the top-level fields are read
// (name, element counts, bounding box/sphere) - per-element visual data is not verified.
class CoDEffect_t : public CoDAsset_t
{
public:
    CoDEffect_t();
    virtual ~CoDEffect_t();

    // Total element count (elemDefCountLooping + elemDefCountOneShot + elemDefCountEmission)
    uint32_t ElementCount;
    // How many of those elements are "looping" (persist for msecLoopingLife rather than firing once)
    uint32_t LoopingElementCount;
    // How many are one-shot (fire once, e.g. an impact burst)
    uint32_t OneShotElementCount;
    // How many are spawned by emission over the effect's lifetime
    uint32_t EmissionElementCount;
    // How long a looping element's life is, in milliseconds (0 for pure one-shot effects)
    uint32_t LoopingLifeMsec;

    bool Compare(const CoDAsset_t* candidate, const AssetCompareMethod compareMethod) const;
};

enum class SoundDataTypes
{
    WAV_WithHeader,
    WAV_NeedsHeader,
    FLAC_WithHeader,
    FLAC_NeedsHeader,
    Opus_Interleaved,
    Opus_Interleaved_Streamed,
};

class CoDSound_t : public CoDAsset_t
{
public:
    CoDSound_t();
    virtual ~CoDSound_t();

    std::string FullPath;
    // The bank this entry lives in, so multiple open banks (e.g. Infinite Warfare's dozens)
    // don't collide on GamePackageCache's single cached path. Empty = fall back to the cache.
    std::string BankFile;
    // The entry's key in its bank (the name's hash; set by the v10 banks' reader, 0 elsewhere): what
    // a patch_ bank's entry replaces (IWZone::ExportSoundBanks)
    uint64_t BankKey = 0;
    uint32_t FrameRate;
    uint32_t FrameCount;
    uint8_t ChannelsCount;
    uint32_t Length;
    uint16_t PackageIndex;
    bool IsLocalized;

    SoundDataTypes DataType;

    bool Compare(const CoDAsset_t* candidate, const AssetCompareMethod compareMethod) const;
};

enum class BoneDataTypes
{
    DivideBySize,
    QuatPackingA,
    HalfFloat
};

struct XModelSubmesh_t
{
    XModelSubmesh_t();

    // Count of rigid vertex pairs
    uint32_t VertListcount;
    uint64_t RigidWeightsPtr;

    uint32_t VertexCount;
    uint32_t FaceCount;
    uint32_t PackedIndexTableCount;

    uint64_t FacesPtr;
    uint64_t VertexPtr;
    uint64_t VertexNormalsPtr;
    uint64_t VertexUVsPtr;
    uint64_t VertexColorPtr;
    uint64_t PackedIndexTablePtr;
    uint64_t PackedIndexBufferPtr;

    uint16_t WeightCounts[8];
    uint64_t WeightsPtr;
    uint64_t BlendShapesPtr;

    float Scale;

    float XOffset;
    float YOffset;
    float ZOffset;

    int32_t MaterialIndex;
};

enum class ImageUsageType : uint8_t
{
    Unknown,
    DiffuseMap,
    NormalMap,
    SpecularMap,
    GlossMap
};

struct XImage_t
{
    XImage_t(ImageUsageType Usage, uint32_t Hash, uint64_t Pointer, const std::string& Name);

    ImageUsageType ImageUsage;
    uint64_t ImagePtr;

    uint32_t SemanticHash;

    std::string ImageName;
};

struct XMaterialSetting_t
{
    XMaterialSetting_t(
        const char* name,
        const char* type,
        const float* data,
        const size_t numElements);

    XMaterialSetting_t(
        const char* name,
        const char* type,
        const int32_t* data,
        const size_t numElements);

    XMaterialSetting_t(
        const char* name,
        const char* type,
        const uint32_t* data,
        const size_t numElements);

    std::string Name;
    std::string Type;
    float Data[4];
};

struct XMaterial_t
{
    XMaterial_t(uint32_t ImageCount);

    std::string MaterialName;
    std::string TechsetName;
    std::string SurfaceTypeName;

    std::vector<XImage_t> Images;

    std::vector<XMaterialSetting_t> Settings;

    // The game's own constant bytes, by its block index (BO3: technique's static pixel cbuffer;
    // BO4: [0] the material's constant blob). Read by the material adapters (shading.txt).
    std::vector<std::vector<uint8_t>> Constants;
};

struct XModelLod_t
{
    XModelLod_t(uint16_t SubmeshCount);

    std::string Name;
    std::vector<XModelSubmesh_t> Submeshes;
    std::vector<XMaterial_t> Materials;
    
    // A key used for the lod data if it's streamed
    uint64_t LODStreamKey;
    uint64_t LODStreamInfoPtr;

    // The distance this lod displays at
    float LodDistance;
    // The max distance this lod displays at
    float LodMaxDistance;
};

struct XModel_t
{
    XModel_t(uint16_t LodCount);

    std::string ModelName;

    BoneDataTypes BoneRotationData;

    bool IsModelStreamed;

    uint32_t BoneCount;
    uint32_t RootBoneCount;
    uint32_t CosmeticBoneCount;
    uint32_t BlendShapeCount;

    // A pointer to bone name string indicies
    uint64_t BoneIDsPtr;
    uint8_t BoneIndexSize;

    uint64_t BoneParentsPtr;
    uint8_t BoneParentSize;

    // A pointer to local rotations
    uint64_t RotationsPtr;
    // A pointer to local positions
    uint64_t TranslationsPtr;

    // A pointer to global matricies
    uint64_t BaseMatriciesPtr;

    // A pointer to the bone collision data, hitbox offsets
    uint64_t BoneInfoPtr;

    uint64_t BlendShapeNamesPtr;

    std::vector<XModelLod_t> ModelLods;
};

enum class AnimationKeyTypes
{
    DivideBySize,
    MinSizeTable,
    QuatPackingA,
    HalfFloat
};

struct XAnim_t
{
    XAnim_t();

    std::string AnimationName;

    float FrameRate;
    uint32_t FrameCount;

    bool ViewModelAnimation;
    bool LoopingAnimation;
    bool AdditiveAnimation;
    bool SupportsInlineIndicies;

    // A pointer to bone name string indicies
    uint64_t BoneIDsPtr;
    uint8_t BoneIndexSize;
    // The size of the inline bone indicies (0) when not in use
    uint8_t BoneTypeSize;

    AnimationKeyTypes RotationType;
    AnimationKeyTypes TranslationType;

    uint64_t DataBytesPtr;
    uint64_t DataShortsPtr;
    uint64_t DataIntsPtr;

    uint64_t RandomDataBytesPtr;
    uint64_t RandomDataShortsPtr;
    uint64_t RandomDataIntsPtr;

    // A pointer to animation indicies (When we have more than 255)
    uint64_t LongIndiciesPtr;
    // A pointer to animation notetracks
    uint64_t NotificationsPtr;
    uint64_t BlendShapeNamesPtr;
    uint64_t BlendShapeWeightsPtr;

    uint64_t DeltaTranslationPtr;
    uint64_t Delta2DRotationsPtr;
    uint64_t Delta3DRotationsPtr;

    uint32_t NoneRotatedBoneCount;
    uint32_t TwoDRotatedBoneCount;
    uint32_t NormalRotatedBoneCount;
    uint32_t TwoDStaticRotatedBoneCount;
    uint32_t NormalStaticRotatedBoneCount;
    uint32_t NormalTranslatedBoneCount;
    uint32_t PreciseTranslatedBoneCount;
    uint32_t StaticTranslatedBoneCount;
    uint32_t NoneTranslatedBoneCount;
    uint32_t TotalBoneCount;
    uint32_t NotificationCount;
    uint32_t BlendShapeWeightCount;

    // XAnim Reader (Streamed)
    std::unique_ptr<CoDXAnimReader> Reader;
    std::function<void(const std::unique_ptr<XAnim_t>&, std::unique_ptr<WraithAnim>&)> ReaderFunction;
    uint64_t ReaderInformationPointer;
};

struct XImageDDS
{
    XImageDDS();
    ~XImageDDS();

    int8_t* DataBuffer;
    uint32_t DataSize;

    ImagePatch ImagePatchType;
};

struct XSound
{
    XSound();
    ~XSound();

    int8_t* DataBuffer;
    uint32_t DataSize;

    // The sound format: WAV/FLAC with their header, or Opus_Interleaved: the game's packets as it
    // keeps them, {u16 size, Opus packet} from DataBuffer, each starting before DataSize (the last
    // one may end past it, SABSupport::OpusPackets keeps those bytes), with the three below
    SoundDataTypes DataType;
    uint32_t FrameRate = 0, Channels = 0, FrameCount = 0;
    // Opus_Interleaved: the decoded samples (at 48 kHz) before the sound's first, which the game
    // skips (SABSupport::OpusPreSkip...); the sound is then FrameCount samples from there. -1: not
    // measured for the game, every packet's samples kept
    int32_t OpusPreSkip = -1;
};

class CoDMaterial_t : public CoDAsset_t
{
public:
    CoDMaterial_t();
    virtual ~CoDMaterial_t();

    size_t ImageCount;

    bool Compare(const CoDAsset_t* candidate, const AssetCompareMethod compareMethod) const;
};