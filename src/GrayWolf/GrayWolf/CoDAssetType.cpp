#include "stdafx.h"

#include "CoDAssetType.h"

CoDAsset_t::CoDAsset_t() : WraithAsset()
{
    AssetPointer = 0;
    AssetLoadedIndex = 0;
    IsFileEntry = false;
}

CoDAsset_t::~CoDAsset_t()
{
}

namespace
{
    // The asset list's order: by type, then, ByDetails, larger Detail first between two assets of
    // Type, then by name
    template <typename TAsset, typename FDetail>
    bool CompareAssets(const CoDAsset_t* lhs, const CoDAsset_t* rhs, AssetCompareMethod Method, WraithAssetType Type, FDetail Detail)
    {
        if (Method != AssetCompareMethod::ByDetails && Method != AssetCompareMethod::ByName)
            return false;

        if (lhs->AssetType < rhs->AssetType) return true;
        if (rhs->AssetType < lhs->AssetType) return false;

        if (Method == AssetCompareMethod::ByDetails && rhs->AssetType == Type)
        {
            const auto Detail1 = Detail((const TAsset*)lhs);
            const auto Detail2 = Detail((const TAsset*)rhs);
            if (Detail1 < Detail2) return false;
            if (Detail2 < Detail1) return true;
        }

        if (lhs->AssetName < rhs->AssetName) return true;
        return false;
    }
}

bool CoDAsset_t::Compare(const CoDAsset_t* candidate, const AssetCompareMethod compareMethod) const
{
    auto lhs = this;
    auto rhs = candidate;

    if (compareMethod != AssetCompareMethod::None)
    {
        if (lhs->AssetType < rhs->AssetType) return true;
        if (rhs->AssetType < lhs->AssetType) return false;
        if (lhs->AssetName < rhs->AssetName) return true;
        if (rhs->AssetName < lhs->AssetName) return false;
    }

    return false;
}

AssetPool::~AssetPool()
{
    for (auto Asset : LoadedAssets)
    {
        delete Asset;
    }
    LoadedAssets.clear();
    LoadedAssets.shrink_to_fit();
}

CoDAnim_t::CoDAnim_t()
{
    Framerate = 30.0f;
    FrameCount = 0;
    BoneCount = 0;
    ShapeCount = 0;
    AssetType = WraithAssetType::Animation;
    AssetSize = -1;
    isSiegeAnim = false;
}

CoDAnim_t::~CoDAnim_t()
{
}

bool CoDAnim_t::Compare(const CoDAsset_t* candidate, const AssetCompareMethod compareMethod) const
{
    return CompareAssets<CoDAnim_t>(this, candidate, compareMethod, WraithAssetType::Animation, [](const CoDAnim_t* C) { return C->FrameCount; });
}

XMaterial_t::XMaterial_t(uint32_t ImageCount)
{
    MaterialName = "wraith_material";
    Images.reserve(ImageCount);
}

XImage_t::XImage_t(ImageUsageType Usage, uint32_t Hash, uint64_t Pointer, std::string Name)
{
    ImageUsage = Usage;
    ImagePtr = Pointer;
    ImageName = std::move(Name);
    SemanticHash = Hash;
}

XModelSubmesh_t::XModelSubmesh_t()
{
    VertListcount = 0;
    RigidWeightsPtr = 0;
    VertexCount = 0;
    FaceCount = 0;
    VertexPtr = 0;
    WeightsPtr = 0;
    FacesPtr = 0;
    VertexPtr = 0;
    VertexNormalsPtr = 0;
    VertexUVsPtr = 0;
    VertexColorPtr = 0;
    PackedIndexTablePtr = 0;
    PackedIndexBufferPtr = 0;
    BlendShapesPtr = 0;
    PackedIndexTableCount = 0;
    Scale = 0;
    XOffset = 0;
    YOffset = 0;
    ZOffset = 0;

    std::memset(&WeightCounts, 0, sizeof(WeightCounts));
    WeightsPtr = 0;
    MaterialIndex = -1;
}

XModelLod_t::XModelLod_t(uint16_t SubmeshCount)
{
    Submeshes.reserve(SubmeshCount);
    Materials.reserve(SubmeshCount);
    LodDistance = 100.0f;
    LodMaxDistance = 200.0f;
    LODStreamKey = 0;
    LODStreamInfoPtr = 0;
}

XModel_t::XModel_t(uint16_t LodCount)
{
    ModelLods.reserve(LodCount);
    ModelName = "";
    BoneRotationData = BoneDataTypes::DivideBySize;
    BoneCount = 0;
    RootBoneCount = 0;
    CosmeticBoneCount = 0;
    BoneIDsPtr = 0;
    BoneIndexSize = 2;
    BoneParentsPtr = 0;
    BoneParentSize = 1;
    RotationsPtr = 0;
    TranslationsPtr = 0;
    BaseMatriciesPtr = 0;
    BoneInfoPtr = 0;
    IsModelStreamed = false;
}

CoDModel_t::CoDModel_t()
{
    BoneCount = 0;
    LodCount = 0;
    CosmeticBoneCount = 0;
    AssetType = WraithAssetType::Model;
    AssetSize = -1;
}

CoDModel_t::~CoDModel_t()
{
}

bool CoDModel_t::Compare(const CoDAsset_t* candidate, const AssetCompareMethod compareMethod) const
{
    return CompareAssets<CoDModel_t>(this, candidate, compareMethod, WraithAssetType::Model, [](const CoDModel_t* C) { return C->BoneCount; });
}

CoDImage_t::CoDImage_t()
{
    Width = 0;
    Height = 0;
    Format = 0;
    AssetType = WraithAssetType::Image;
    AssetSize = -1;
}

CoDImage_t::~CoDImage_t()
{
}

bool CoDImage_t::Compare(const CoDAsset_t* candidate, const AssetCompareMethod compareMethod) const
{
    return CompareAssets<CoDImage_t>(this, candidate, compareMethod, WraithAssetType::Image, [](const CoDImage_t* C) { return C->Width * C->Height; });
}

CoDSound_t::CoDSound_t()
{
    FullPath = "";
    FrameRate = 0;
    FrameCount = 0;
    ChannelsCount = 0;
    PackageIndex = 0;
    Length = 0;
    IsLocalized = false;
    DataType = SoundDataTypes::WAV_WithHeader;
    AssetType = WraithAssetType::Sound;
    AssetSize = -1;
}

CoDSound_t::~CoDSound_t()
{
}

bool CoDSound_t::Compare(const CoDAsset_t* candidate, const AssetCompareMethod compareMethod) const
{
    return CompareAssets<CoDSound_t>(this, candidate, compareMethod, WraithAssetType::Sound, [](const CoDSound_t* C) { return C->Length; });
}

CoDRawFile_t::CoDRawFile_t()
{
    RawFilePath = "";
    RawDataPointer = 0;
    AssetType = WraithAssetType::RawFile;
    AssetSize = -1;
}

CoDRawFile_t::~CoDRawFile_t()
{
}

bool CoDRawFile_t::Compare(const CoDAsset_t* candidate, const AssetCompareMethod compareMethod) const
{
    return CompareAssets<CoDRawFile_t>(this, candidate, compareMethod, WraithAssetType::RawFile, [](const CoDRawFile_t* C) { return C->AssetSize; });
}

CoDEffect_t::CoDEffect_t()
{
    ElementCount = 0;
    LoopingElementCount = 0;
    OneShotElementCount = 0;
    EmissionElementCount = 0;
    LoopingLifeMsec = 0;
    AssetType = WraithAssetType::Effect;
    // Size, not meaningfully known without walking elemDefs (see the note on the class)
    AssetSize = -1;
}

CoDEffect_t::~CoDEffect_t()
{
}

bool CoDEffect_t::Compare(const CoDAsset_t* candidate, const AssetCompareMethod compareMethod) const
{
    return CompareAssets<CoDEffect_t>(this, candidate, compareMethod, WraithAssetType::Effect, [](const CoDEffect_t* C) { return C->ElementCount; });
}

XAnim_t::XAnim_t()
{
    AnimationName = "";
    FrameRate = 30.0f;
    FrameCount = 0;
    ViewModelAnimation = false;
    LoopingAnimation = false;
    AdditiveAnimation = false;
    SupportsInlineIndicies = true;
    BoneIDsPtr = 0;
    BoneIndexSize = 2;
    BoneTypeSize = 0;
    DataBytesPtr = 0;
    DataShortsPtr = 0;
    DataIntsPtr = 0;
    RandomDataBytesPtr = 0;
    RandomDataShortsPtr = 0;
    RandomDataIntsPtr = 0;
    LongIndiciesPtr = 0;
    NotificationsPtr = 0;
    DeltaTranslationPtr = 0;
    Delta2DRotationsPtr = 0;
    Delta3DRotationsPtr = 0;
    RotationType = AnimationKeyTypes::DivideBySize;
    TranslationType = AnimationKeyTypes::MinSizeTable;
}

XImageDDS::XImageDDS()
{
    DataBuffer = nullptr;
    DataSize = 0;
    ImagePatchType = ImagePatch::NoPatch;
}

XImageDDS::~XImageDDS()
{
    if (DataBuffer != nullptr)
    {
        delete[] DataBuffer;
    }
}

XSound::XSound()
{
    DataBuffer = nullptr;
    DataSize = 0;
    DataType = SoundDataTypes::FLAC_WithHeader;
}

XSound::~XSound()
{
    if (DataBuffer != nullptr)
    {
        delete[] DataBuffer;
    }
}

CoDMaterial_t::CoDMaterial_t()
{
    ImageCount = 0;
    AssetType = WraithAssetType::Material;
}

CoDMaterial_t::~CoDMaterial_t()
{
}

bool CoDMaterial_t::Compare(const CoDAsset_t* candidate, const AssetCompareMethod compareMethod) const
{
    return CompareAssets<CoDMaterial_t>(this, candidate, compareMethod, WraithAssetType::Material, [](const CoDMaterial_t* C) { return C->ImageCount; });
}

XMaterialSetting_t::XMaterialSetting_t(
    const char* name,
    const char* type,
    const float* data,
    const size_t numElements)
{
    Name = name;
    Type = type;

    std::memset(Data, 0, sizeof(Data));
    std::memcpy(Data, data, numElements * sizeof(float));

    // Append element count to match HLSL names
    if(numElements > 1)
        Type += std::to_string(numElements);
}

XMaterialSetting_t::XMaterialSetting_t(
    const char* name,
    const char* type,
    const int32_t* data,
    const size_t numElements)
{
    Name = name;
    Type = type;

    std::memset(Data, 0, sizeof(Data));

    for (size_t i = 0; i < numElements; i++)
    {
        Data[i] = (float)data[i];
    }

    // Append element count to match HLSL names
    if (numElements > 1)
        Type += std::to_string(numElements);
}

XMaterialSetting_t::XMaterialSetting_t(
    const char* name,
    const char* type,
    const uint32_t* data,
    const size_t numElements)
{
    Name = name;
    Type = type;

    std::memset(Data, 0, sizeof(Data));

    for (size_t i = 0; i < numElements; i++)
    {
        Data[i] = (float)data[i];
    }

    // Append element count to match HLSL names
    if (numElements > 1)
        Type += std::to_string(numElements);
}

AssetCompareMethod AssetCompareMethodHelper::CalculateCompareMethod(const std::string& compareMethodStr)
{
    if (compareMethodStr == "Name")
        return AssetCompareMethod::ByName;
    else if (compareMethodStr == "Details")
        return AssetCompareMethod::ByDetails;
    else
        return AssetCompareMethod::None;
}
