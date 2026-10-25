#pragma once

// What the live readers (Game*.cpp) share: the XAnim, XModel, material and sound fields every
// IW-engine layout names alike. Each game keeps its own structs; these templates read them by
// field name, so a layout difference stays in DBGameAssets.h.

#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "CoDAssets.h"
#include "ExportNaming.h"
#include "ExportWriters.h"
#include "FileSystems.h"

namespace GameReaders
{
    // The images a material another zone defines names (semantic hash, image name), looked up by
    // the material's name: set by a disk game, read by the reader's ReadXMaterial.
    using ImagesElsewhereFn = std::function<bool(const std::string& Name, std::vector<std::pair<uint32_t, std::string>>& Images)>;

    namespace Detail
    {
        template <typename T, typename = void> struct HasLooped : std::false_type {};
        template <typename T> struct HasLooped<T, std::void_t<decltype(T::Looped)>> : std::true_type {};
        template <typename T, typename = void> struct HasDelta3D : std::false_type {};
        template <typename T> struct HasDelta3D<T, std::void_t<decltype(T::Delta3DRotationsPtr)>> : std::true_type {};
        template <typename T, typename = void> struct HasSurfaceArray : std::false_type {};
        template <typename T> struct HasSurfaceArray<T, std::void_t<decltype(T::SurfacesPtr)>> : std::true_type {};
        template <typename T, typename = void> struct HasSemanticHash : std::false_type {};
        template <typename T> struct HasSemanticHash<T, std::void_t<decltype(T::SemanticHash)>> : std::true_type {};

        struct NoExtra
        {
            template <typename... T> void operator()(T&&...) const {}
        };
    }

    // True when Name starts with one of Prefixes, ignoring case
    inline bool StartsWithAny(const std::string& Name, std::initializer_list<const char*> Prefixes)
    {
        for (const char* Prefix : Prefixes)
            if (_strnicmp(Name.c_str(), Prefix, std::strlen(Prefix)) == 0) return true;
        return false;
    }

    // The XAnim header fields every layout names alike: frames, the data and notetrack
    // pointers, the bone counts
    template <typename TAnim>
    void CopyAnimHeader(XAnim_t& Anim, const TAnim& Data)
    {
        Anim.FrameCount = Data.NumFrames;
        Anim.FrameRate = Data.Framerate;

        Anim.BoneIDsPtr = Data.BoneIDsPtr;
        Anim.DataBytesPtr = Data.DataBytePtr;
        Anim.DataShortsPtr = Data.DataShortPtr;
        Anim.DataIntsPtr = Data.DataIntPtr;
        Anim.RandomDataBytesPtr = Data.RandomDataBytePtr;
        Anim.RandomDataShortsPtr = Data.RandomDataShortPtr;
        Anim.NotificationsPtr = Data.NotificationsPtr;

        Anim.NoneRotatedBoneCount = Data.NoneRotatedBoneCount;
        Anim.TwoDRotatedBoneCount = Data.TwoDRotatedBoneCount;
        Anim.NormalRotatedBoneCount = Data.NormalRotatedBoneCount;
        Anim.TwoDStaticRotatedBoneCount = Data.TwoDStaticRotatedBoneCount;
        Anim.NormalStaticRotatedBoneCount = Data.NormalStaticRotatedBoneCount;
        Anim.NormalTranslatedBoneCount = Data.NormalTranslatedBoneCount;
        Anim.PreciseTranslatedBoneCount = Data.PreciseTranslatedBoneCount;
        Anim.StaticTranslatedBoneCount = Data.StaticTranslatedBoneCount;
        Anim.NoneTranslatedBoneCount = Data.NoneTranslatedBoneCount;
        Anim.TotalBoneCount = Data.TotalBoneCount;
        Anim.NotificationCount = Data.NotificationCount;
    }

    // The delta parts (root motion); the 32-bit IW layouts have no 3D rotations
    template <typename TDelta>
    void CopyDeltaParts(XAnim_t& Anim, const TDelta& Delta)
    {
        Anim.DeltaTranslationPtr = Delta.DeltaTranslationsPtr;
        Anim.Delta2DRotationsPtr = Delta.Delta2DRotationsPtr;
        if constexpr (Detail::HasDelta3D<TDelta>::value) Anim.Delta3DRotationsPtr = Delta.Delta3DRotationsPtr;
    }

    // An IW-engine XAnim (divide-by-size rotations, inline indices): its header, random ints,
    // long indices, looping flag and delta parts. Extra(Anim, Data) adds what one game reads more.
    template <typename TAnim, typename TDelta, typename FExtra = Detail::NoExtra>
    std::unique_ptr<XAnim_t> ReadAnim(const CoDAnim_t* Animation, uint8_t BoneIndexSize, std::initializer_list<const char*> ViewmodelPrefixes,
                                      FExtra Extra = {})
    {
        if (!CoDAssets::GameInstance->IsRunning()) return nullptr;

        auto Anim = std::make_unique<XAnim_t>();
        const auto Data = CoDAssets::GameInstance->Read<TAnim>(Animation->AssetPointer);

        Anim->AnimationName = Animation->AssetName;
        Anim->ViewModelAnimation = StartsWithAny(Animation->AssetName, ViewmodelPrefixes);
        if constexpr (Detail::HasLooped<TAnim>::value) Anim->LoopingAnimation = Data.Looped > 0;
        else Anim->LoopingAnimation = Data.Flags & 1;

        CopyAnimHeader(*Anim, Data);
        Anim->RandomDataIntsPtr = Data.RandomDataIntPtr;
        Anim->LongIndiciesPtr = Data.LongIndiciesPtr;
        Anim->BoneIndexSize = BoneIndexSize;
        CopyDeltaParts(*Anim, CoDAssets::GameInstance->Read<TDelta>(Data.DeltaPartsPtr));

        Anim->RotationType = AnimationKeyTypes::DivideBySize;
        Anim->TranslationType = AnimationKeyTypes::MinSizeTable;
        Anim->SupportsInlineIndicies = true;

        Extra(*Anim, Data);
        return Anim;
    }

    // The skeleton fields every XModel layout names alike
    template <typename TModel>
    void CopyModelBones(XModel_t& Asset, const std::string& Name, const TModel& Data, uint8_t BoneIndexSize, uint8_t BoneParentSize)
    {
        Asset.ModelName = Name;
        Asset.BoneCount = Data.NumBones;
        Asset.RootBoneCount = Data.NumRootBones;
        Asset.BoneIDsPtr = Data.BoneIDsPtr;
        Asset.BoneIndexSize = BoneIndexSize;
        Asset.BoneParentsPtr = Data.ParentListPtr;
        Asset.BoneParentSize = BoneParentSize;
        Asset.RotationsPtr = Data.RotationsPtr;
        Asset.TranslationsPtr = Data.TranslationsPtr;
        Asset.BaseMatriciesPtr = Data.BaseMatriciesPtr;
    }

    // A streamed IW XModel's skeleton (MW 2019, MWIII): its bones then the unnamed ones after
    // them, divide-by-size; none for a model of several bones with no parent list (thousands of
    // bones with no info)
    template <typename TModel>
    void CopyStreamedBones(XModel_t& Asset, const std::string& Name, const TModel& Data, uint8_t BoneParentSize)
    {
        CopyModelBones(Asset, Name, Data, 4, BoneParentSize);
        Asset.BoneCount = Data.NumBones + Data.UnkBoneCount;
        if (Asset.BoneCount > 1 && Data.ParentListPtr == 0) Asset.BoneCount = Asset.RootBoneCount = 0;
        Asset.BoneRotationData = BoneDataTypes::DivideBySize;
        Asset.IsModelStreamed = true;
    }

    // A surface whose vertices, faces and weights the zone holds
    template <typename TSurface>
    void CopySurface(XModelSubmesh_t& Submesh, const TSurface& Info)
    {
        Submesh.VertListcount = Info.VertListCount;
        Submesh.RigidWeightsPtr = Info.RigidWeightsPtr;
        Submesh.VertexCount = Info.VertexCount;
        Submesh.FaceCount = Info.FacesCount;
        Submesh.VertexPtr = Info.VerticiesPtr;
        Submesh.FacesPtr = Info.FacesPtr;
        for (size_t w = 0; w < std::size(Info.WeightCounts); w++) Submesh.WeightCounts[w] = Info.WeightCounts[w];
        Submesh.WeightsPtr = Info.WeightsPtr;
    }

    // Count TSurface at Surfaces into Lod, each with the material its THandle at Handles names
    // (Handles advances past them). Fill(Submesh, Surface) copies a surface.
    template <typename TSurface, typename THandle, typename FMaterial, typename FFill>
    void ReadSurfaces(XModelLod_t& Lod, uint64_t Surfaces, uint32_t Count, uint64_t& Handles, FMaterial ReadMaterial, FFill Fill)
    {
        for (uint32_t s = 0; s < Count; s++)
        {
            Lod.Submeshes.emplace_back();
            Fill(Lod.Submeshes[s], CoDAssets::GameInstance->Read<TSurface>(Surfaces));
            Lod.Materials.emplace_back(ReadMaterial(CoDAssets::GameInstance->Read<THandle>(Handles)));

            Surfaces += sizeof(TSurface);
            Handles += sizeof(THandle);
        }
    }

    // An IW-engine XModel whose surfaces the zone holds (divide-by-size bones, one byte per
    // parent): a LOD's surfaces at its own pointer, or at its index in the model's array (32-bit);
    // one material handle per surface, LOD after LOD. Extra(Asset, Data) adds what one game reads more.
    template <typename TModel, typename TSurface, typename THandle, typename FMaterial, typename FExtra = Detail::NoExtra>
    std::unique_ptr<XModel_t> ReadModel(const CoDModel_t* Model, uint8_t BoneIndexSize, FMaterial ReadMaterial, FExtra Extra = {})
    {
        if (!CoDAssets::GameInstance->IsRunning()) return nullptr;

        const auto Data = CoDAssets::GameInstance->Read<TModel>(Model->AssetPointer);
        auto Asset = std::make_unique<XModel_t>(Data.NumLods);
        CopyModelBones(*Asset, Model->AssetName, Data, BoneIndexSize, 1);
        Asset->BoneRotationData = BoneDataTypes::DivideBySize;
        Extra(*Asset, Data);

        uint64_t Handles = Data.MaterialHandlesPtr;
        for (uint32_t i = 0; i < Data.NumLods; i++)
        {
            const auto& Lod = Data.ModelLods[i];
            Asset->ModelLods.emplace_back(Lod.NumSurfs);
            Asset->ModelLods[i].LodDistance = Lod.LodDistance;

            uint64_t Surfaces = 0;
            if constexpr (Detail::HasSurfaceArray<TModel>::value) Surfaces = Data.SurfacesPtr + (Lod.SurfacesIndex * sizeof(TSurface));
            else Surfaces = Lod.SurfsPtr;

            ReadSurfaces<TSurface, THandle>(Asset->ModelLods[i], Surfaces, Lod.NumSurfs, Handles, ReadMaterial,
                                            [](XModelSubmesh_t& Submesh, const TSurface& Info) { CopySurface(Submesh, Info); });
        }
        return Asset;
    }

    // A Treyarch surface (BO3, BO4, CW): counts and its vertex and face indices in the LOD's
    // streamed mesh
    template <typename TSurface>
    void CopyStreamedSurface(XModelSubmesh_t& Submesh, const TSurface& Info)
    {
        Submesh.VertexCount = Info.VertexCount;
        Submesh.FaceCount = Info.FacesCount;
        Submesh.VertexPtr = Info.VerticiesIndex;
        Submesh.FacesPtr = Info.FacesIndex;
    }

    // The LODs a Treyarch XModel points to (BO3, BO4, CW), each streamed (its key and mesh info).
    // The model's material list holds one HandleStride-byte entry per LOD, the pointer to its
    // surfaces' handles 8 bytes in. Fill(Submesh, Surface) copies a surface; LodExtra(Lod, Info)
    // adds what one game reads more.
    template <typename TLod, typename TSurface, typename FMaterial, typename FFill, typename FLod = Detail::NoExtra>
    void ReadStreamedLods(XModel_t& Asset, const uint64_t* LodPtrs, uint32_t NumLods, uint64_t MaterialList, uint32_t HandleStride, FMaterial ReadMaterial,
                          FFill Fill, FLod LodExtra = {})
    {
        for (uint32_t i = 0; i < NumLods; i++)
        {
            const auto Info = CoDAssets::GameInstance->Read<TLod>(LodPtrs[i]);
            Asset.ModelLods.emplace_back(Info.NumSurfs);
            auto& Lod = Asset.ModelLods[i];
            Lod.LodDistance = Info.LodDistance;
            Lod.LODStreamKey = Info.LODStreamKey;
            Lod.LODStreamInfoPtr = Info.XModelMeshPtr;
            LodExtra(Lod, Info);

            uint64_t Handles = CoDAssets::GameInstance->Read<uint64_t>(MaterialList + (uint64_t)i * HandleStride + 8);
            ReadSurfaces<TSurface, uint64_t>(Lod, Info.XSurfacePtr, Info.NumSurfs, Handles, ReadMaterial, Fill);
        }
    }

    // The usage the IW-engine shaders give the colour, normal and specular semantics
    inline ImageUsageType SemanticUsage(uint32_t Semantic)
    {
        switch (Semantic)
        {
        case 0xA0AB1041: return ImageUsageType::DiffuseMap;
        case 0x59D30D0F: return ImageUsageType::NormalMap;
        case 0x34ECCCB3: return ImageUsageType::SpecularMap;
        default: return ImageUsageType::Unknown;
        }
    }

    // The same for Treyarch's shaders (BO3, BO4, CW): their specular semantic differs
    inline ImageUsageType TreyarchSemanticUsage(uint32_t Semantic)
    {
        switch (Semantic)
        {
        case 0xA0AB1041: return ImageUsageType::DiffuseMap;
        case 0x59D30D0F: return ImageUsageType::NormalMap;
        case 0xEC443804: return ImageUsageType::SpecularMap;
        default: return ImageUsageType::Unknown;
        }
    }

    // Count TImage entries (image pointer, semantic hash or type) at Table: each image named by
    // NameOf(ImagePtr), used as UsageOf(Semantic)
    template <typename TImage, typename FName, typename FUsage>
    void AddMaterialImages(XMaterial_t& Result, uint64_t Table, uint32_t Count, FName NameOf, FUsage UsageOf)
    {
        for (uint32_t m = 0; m < Count; m++)
        {
            const auto Image = CoDAssets::GameInstance->Read<TImage>(Table);
            const auto Semantic = [&Image] {
                if constexpr (Detail::HasSemanticHash<TImage>::value) return Image.SemanticHash;
                else return Image.Type;
            }();
            Result.Images.emplace_back(UsageOf(Semantic), Semantic, Image.ImagePtr, NameOf(Image.ImagePtr));
            Table += sizeof(TImage);
        }
    }

    // A material's image name: the string pointer (TPointer wide) at Offset in its image
    template <typename TPointer>
    std::string ImageNameAt(uint64_t ImagePtr, uint64_t Offset)
    {
        return CoDAssets::GameInstance->ReadNullTerminatedString(CoDAssets::GameInstance->Read<TPointer>(ImagePtr + Offset));
    }

    // A name-only material header (no images): the images the zone defining Name gives it, read
    // later by name (ExportWriters::ImageElsewhere)
    template <typename FUsage>
    void AddImagesElsewhere(XMaterial_t& Result, const ImagesElsewhereFn& Lookup, const std::string& Name, FUsage UsageOf)
    {
        std::vector<std::pair<uint32_t, std::string>> Images;
        if (!Lookup || !Lookup(Name, Images)) return;
        for (const auto& [Semantic, ImageName] : Images)
            Result.Images.emplace_back(UsageOf(Semantic), Semantic, ExportWriters::ImageElsewhere, ImageName);
    }

    // The stream level (0 = base, 1-3 = MipLevels) with the largest width, and its size
    template <typename TImage>
    uint32_t LargestLevel(const TImage& Info, uint32_t& Width, uint32_t& Height)
    {
        uint32_t Largest = 0;
        Width = Info.Width;
        Height = Info.Height;
        for (uint32_t i = 0; i < 3; i++)
        {
            if (Info.MipLevels[i].Width > Width)
            {
                Largest = i + 1;
                Width = Info.MipLevels[i].Width;
                Height = Info.MipLevels[i].Height;
            }
        }
        return Largest;
    }

    // Pixels as a DDS, a normal map marked for expansion when "patchnormals" is on
    std::unique_ptr<XImageDDS> TranslateNormalPatched(const std::unique_ptr<uint8_t[]>& Data, uint32_t Size, uint32_t Width, uint32_t Height, uint8_t Format,
                                                      ImageUsageType Usage);

    // A pak image's pixels (Levels mips from the largest) as a DDS: A2XY normal maps (format 84)
    // un-swapped, a normal map marked for expansion when "patchnormals" is on
    std::unique_ptr<XImageDDS> TranslatePakImage(std::unique_ptr<uint8_t[]> Data, uint32_t Size, uint8_t Format, uint8_t MapType, uint32_t Width,
                                                 uint32_t Height, ImageUsageType Usage, uint8_t Levels = 1);

    // A loaded sound's file name without its folder or an audio extension: a variant's number
    // stays (MWR's khouse_gaz_2_1.4 is one of eight takes of khouse_gaz_2_1 in a zone)
    inline std::string LoadedSoundStem(const std::string& Name)
    {
        std::string S = FileSystems::GetFileName(Name);
        const size_t Dot = S.rfind('.');
        if (Dot == std::string::npos) return S;
        const std::string Ext = ExportNaming::Lower(S.substr(Dot));
        for (const char* Audio : { ".wav", ".flac", ".mp3", ".ogg", ".opus", ".xma", ".xwma", ".snd" })
            if (Ext == Audio) return S.substr(0, Dot);
        return S;
    }

    // A sound whose samples the zone holds, named after its file
    template <typename TLoaded>
    CoDSound_t* LoadedSound(const TLoaded& Info, const std::string& Name, SoundDataTypes DataType)
    {
        auto Sound = new CoDSound_t();
        Sound->AssetName = LoadedSoundStem(Name);
        Sound->FullPath = FileSystems::GetDirectoryName(Name);
        Sound->AssetPointer = Info.SoundDataPtr;
        Sound->AssetSize = Info.SoundDataSize;
        Sound->FrameRate = Info.FrameRate;
        Sound->FrameCount = Info.FrameCount;
        Sound->ChannelsCount = Info.Channels;
        Sound->IsFileEntry = false;
        Sound->DataType = DataType;
        Sound->AssetStatus = WraithAssetStatus::Loaded;
        Sound->Length = (uint32_t)(1000.0f * (float)(Sound->FrameCount / (float)(Sound->FrameRate)));
        return Sound;
    }

    // A streamed sound: nameless, named after its alias and its entry Index
    template <typename TStreamed>
    CoDSound_t* StreamedSound(const TStreamed& Info, const std::string& AliasName, uint32_t Index)
    {
        auto Sound = new CoDSound_t();
        Sound->AssetName = std::format("{}_{}", ExportNaming::Lower(AliasName).c_str(), Index);
        Sound->FullPath = "streamed";
        Sound->IsFileEntry = true;
        Sound->DataType = SoundDataTypes::FLAC_WithHeader;
        Sound->AssetStatus = WraithAssetStatus::Loaded;
        Sound->PackageIndex = Info.PackageIndex;
        Sound->AssetPointer = Info.Offset;
        Sound->AssetSize = Info.Size;
        Sound->Length = Info.Length;
        Sound->IsLocalized = Info.Localization > 0;
        return Sound;
    }

    // A primed sound: its loaded header names it, its samples are in a package
    template <typename TLoaded, typename TPrimed>
    CoDSound_t* PrimedSound(const TLoaded& Info, const std::string& Name, const TPrimed& Primed, SoundDataTypes DataType)
    {
        auto Sound = LoadedSound(Info, Name, DataType);
        Sound->AssetName = FileSystems::GetFileName(Name);
        Sound->IsFileEntry = true;
        Sound->PackageIndex = Primed.PackageIndex;
        Sound->AssetPointer = Primed.Offset;
        Sound->AssetSize = Primed.Size;
        Sound->IsLocalized = Primed.Localization > 0;
        return Sound;
    }

    // The data type a loaded sound's format byte names: FLAC (6, 7) with its header, else PCM
    inline SoundDataTypes LoadedFormat(uint8_t Format)
    {
        return (Format == 7 || Format == 6) ? SoundDataTypes::FLAC_WithHeader : SoundDataTypes::WAV_NeedsHeader;
    }

    // An image from the IWD image cache, by name (CoD4, WaW, BO1): a normal map marked as a
    // bump map when "patchnormals" is on
    std::unique_ptr<XImageDDS> LoadIwdImage(const XImage_t& Image);

    // A 64-bit IW material whose images the zone holds (Ghosts, AW, MW 2019), named after its
    // file: each image named by the string pointer NameAt bytes into it, used as UsageOf(its
    // semantic); a name-only header (",name") gets the images the zone defining it names (Elsewhere)
    template <typename TMaterial, typename TImage, typename FUsage>
    XMaterial_t ReadNamedMaterial(uint64_t MaterialPointer, uint64_t NameAt, const ImagesElsewhereFn& Elsewhere, FUsage UsageOf)
    {
        const auto Data = CoDAssets::GameInstance->Read<TMaterial>(MaterialPointer);
        const auto RawName = CoDAssets::GameInstance->ReadNullTerminatedString(Data.NamePtr);

        XMaterial_t Result(Data.ImageCount);
        Result.MaterialName = FileSystems::GetFileNameWithoutExtension(RawName);
        AddMaterialImages<TImage>(Result, Data.ImageTablePtr, Data.ImageCount, [NameAt](uint64_t Image) { return ImageNameAt<uint64_t>(Image, NameAt); },
                                  UsageOf);
        if (Data.ImageCount == 0 && !RawName.empty() && RawName[0] == ',') AddImagesElsewhere(Result, Elsewhere, RawName.substr(1), UsageOf);
        return Result;
    }

    // A 32-bit IW material (CoD4, WaW): its images named by the pointer 4 bytes before their
    // end, and its technique set's name (the techset pointer at TechsetOffset, its name first; a
    // zone's name-only header starts with ','): what draws it (wc_shadowcaster: shadow maps only)
    template <typename TMaterial, typename TImage, typename TGfxImage>
    XMaterial_t ReadMaterial32(uint64_t MaterialPointer, uint32_t TechsetOffset)
    {
        const auto Data = CoDAssets::GameInstance->Read<TMaterial>(MaterialPointer);

        XMaterial_t Result(Data.ImageCount);
        Result.MaterialName = FileSystems::GetFileNameWithoutExtension(CoDAssets::GameInstance->ReadNullTerminatedString(Data.NamePtr));
        if (const auto Techset = CoDAssets::GameInstance->Read<uint32_t>(MaterialPointer + TechsetOffset))
        {
            Result.TechsetName = CoDAssets::GameInstance->ReadNullTerminatedString(CoDAssets::GameInstance->Read<uint32_t>(Techset));
            if (!Result.TechsetName.empty() && Result.TechsetName[0] == ',') Result.TechsetName.erase(0, 1);
        }

        AddMaterialImages<TImage>(Result, Data.ImageTablePtr, Data.ImageCount,
                                  [](uint64_t Image) { return ImageNameAt<uint32_t>(Image, sizeof(TGfxImage) - 4); }, SemanticUsage);
        return Result;
    }

    // A Treyarch image (BO3, BO4, CW) as a DDS: the chosen streamed mip (Extract(ResultSize), at
    // Width x Height) or, when it has none, the mip loaded in the zone; the sRGB BC formats read
    // as their UNORM twins, a normal map marked for expansion when "patchnormals" is on
    template <typename TImage, typename FExtract>
    std::unique_ptr<XImageDDS> DecodeTreyarchImage(const TImage& Info, bool Streamed, uint32_t Width, uint32_t Height, ImageUsageType Usage, FExtract Extract)
    {
        auto Format = Info.ImageFormat;
        switch (Format)
        {
        case 72: Format = 71; break;
        case 75: Format = 74; break;
        case 78: Format = 77; break;
        case 99: Format = 98; break;
        }

        uint32_t ResultSize = 0;
        std::unique_ptr<uint8_t[]> ImageData = nullptr;

        if (!Streamed)
        {
            Width = Info.LoadedMipWidth;
            Height = Info.LoadedMipHeight;

            uintptr_t ImageMemoryResult = 0;
            auto ImageMemoryBuffer = CoDAssets::GameInstance->Read(Info.LoadedMipPtr, Info.LoadedMipSize, ImageMemoryResult);

            if (ImageMemoryBuffer != nullptr)
            {
                ImageData = std::make_unique<uint8_t[]>((uint32_t)ImageMemoryResult);
                std::memcpy(ImageData.get(), ImageMemoryBuffer, ImageMemoryResult);
                ResultSize = (uint32_t)ImageMemoryResult;
                delete[] ImageMemoryBuffer;
            }
        }
        else
        {
            ImageData = Extract(ResultSize);
        }

        if (ImageData == nullptr)
            return nullptr;

        return TranslateNormalPatched(ImageData, ResultSize, Width, Height, (uint8_t)Format, Usage);
    }
}

