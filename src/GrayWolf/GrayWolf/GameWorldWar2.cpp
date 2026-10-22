#include "stdafx.h"

#include "GameWorldWar2.h"

#include "CoDAssets.h"
#include "CoDRawImageTranslator.h"
#include "CoDXModelTranslator.h"
#include "PAKSupport.h"

#include "Strings.h"
#include "FileSystems.h"
#include "MemoryReader.h"
#include "BinaryReader.h"
#include "SettingsManager.h"
#include "HalfFloats.h"
#include "Sound.h"
#include "Hashing.h"

#include "DBGameGenerics.h"
#include "GameReaders.h"

#include <filesystem>
#include <format>

// The language folders and the prefix of their files (french/fra_soundfile<N>.pak)
static const std::map<std::string, std::string> Localizations
{
    { "english",               "eng_" },
    { "french",                "fra_" },
    { "german",                "deu_" },
    { "italian",               "ita_" },
    { "spanish",               "spa_" },
    { "russian",               "rus_" },
    { "polish",                "pol_" },
    { "portuguese",            "por_" },
    { "japanese_full",         "jpf_" },
    { "japanese_partial",      "jpp_" },
    { "traditional_chinese",   "tch_" },
    { "simplified_chinese",    "sch_" },
    { "arabic",                "ara_" },
    { "czech",                 "cze_" },
    { "spanishna",             "sna_" },
    { "korean",                "kor_" },
    { "english_safe",          "ens_" },
};

void GameWorldWar2::AddAliasSounds(const WWIISoundAlias& Alias, std::set<uint64_t>& Unique, std::vector<CoDAsset_t*>& Out, std::vector<uint64_t>* Sources)
{
    // the entry count is a u8, the byte after it another count
    const uint32_t Count = (uint32_t)(Alias.AliasEntryCount & 0xFF);
    for (uint32_t i = 0; i < Count; i++)
    {
        auto SoundEntryHeader = CoDAssets::GameInstance->Read<WWIISoundEntry>(Alias.AliasEntrysPtr + i * sizeof(WWIISoundEntry));
        auto SoundFileBase = CoDAssets::GameInstance->Read<WWIISoundFileBase>(SoundEntryHeader.SoundFilePtr);
        const uint64_t Spec = SoundEntryHeader.SoundFilePtr;

        SoundEntryHeader.SoundFilePtr += sizeof(WWIISoundFileBase);

        if (SoundFileBase.SoundFileExists == 0)
            continue;

        // Sound file types: 1 = loaded, 2 = primed, 3 = streamed
        if (SoundFileBase.SoundFileType == 1)
        {
            auto UniqueSoundEntry = CoDAssets::GameInstance->Read<uint64_t>(SoundEntryHeader.SoundFilePtr);

            if (UniqueSoundEntry == 0 || Unique.insert(UniqueSoundEntry).second == false)
                continue;

            auto LoadedSoundHeader = CoDAssets::GameInstance->Read<WWIILoadedSoundFile>(UniqueSoundEntry);

            auto SoundName = CoDAssets::GameInstance->ReadNullTerminatedString(LoadedSoundHeader.SoundFileName);

            auto LoadedSound = new CoDSound_t();
            LoadedSound->AssetName = FileSystems::GetFileName(SoundName);
            LoadedSound->FullPath = FileSystems::GetDirectoryName(SoundName);
            LoadedSound->FrameCount = LoadedSoundHeader.FrameCount;
            LoadedSound->FrameRate = LoadedSoundHeader.FrameRate;
            LoadedSound->AssetPointer = LoadedSoundHeader.SoundDataPtr;
            LoadedSound->AssetSize = LoadedSoundHeader.SoundDataSize;
            LoadedSound->ChannelsCount = LoadedSoundHeader.ChannelCount;
            LoadedSound->DataType = (LoadedSoundHeader.Format == 6) ? SoundDataTypes::FLAC_WithHeader : SoundDataTypes::WAV_NeedsHeader;
            LoadedSound->IsFileEntry = false;
            LoadedSound->AssetStatus = WraithAssetStatus::Loaded;
            LoadedSound->Length = LoadedSound->FrameRate ? (uint32_t)(1000.0f * (float)(LoadedSound->FrameCount / (float)(LoadedSound->FrameRate))) : 0;

            Out.push_back(LoadedSound);
            if (Sources) Sources->push_back(UniqueSoundEntry);
        }
        else if (SoundFileBase.SoundFileType == 2)
        {
            if (Unique.insert(SoundEntryHeader.SoundFilePtr).second == false)
                continue;

            auto LoadedSoundHeader = CoDAssets::GameInstance->Read<WWIIPrimedSoundFile>(SoundEntryHeader.SoundFilePtr);

            auto SoundName = CoDAssets::GameInstance->ReadNullTerminatedString(LoadedSoundHeader.SoundFileName);

            auto LoadedSound = new CoDSound_t();
            LoadedSound->AssetName = SoundName;
            LoadedSound->FullPath = SoundName;
            LoadedSound->FrameCount = LoadedSoundHeader.FrameCount;
            LoadedSound->FrameRate = LoadedSoundHeader.FrameRate;
            LoadedSound->AssetPointer = LoadedSoundHeader.PackFileOffset;
            LoadedSound->AssetSize = LoadedSoundHeader.PackFileSize;
            LoadedSound->ChannelsCount = LoadedSoundHeader.ChannelCount;
            LoadedSound->DataType = (LoadedSoundHeader.Format == 6) ? SoundDataTypes::FLAC_WithHeader : SoundDataTypes::WAV_NeedsHeader;
            LoadedSound->IsFileEntry = true;
            LoadedSound->IsLocalized = (LoadedSoundHeader.IsLocalized > 0);
            LoadedSound->PackageIndex = LoadedSoundHeader.PackFileIndex;
            LoadedSound->AssetStatus = WraithAssetStatus::Loaded;
            LoadedSound->Length = LoadedSound->FrameRate ? (uint32_t)(1000.0f * (float)(LoadedSound->FrameCount / (float)(LoadedSound->FrameRate))) : 0;

            Out.push_back(LoadedSound);
            if (Sources) Sources->push_back(Spec);
        }
        else if (SoundFileBase.SoundFileType == 3)
        {
            if (Unique.insert(SoundEntryHeader.SoundFilePtr).second == false)
                continue;

            auto LoadedSoundHeader = CoDAssets::GameInstance->Read<WWIIStreamedSoundFile>(SoundEntryHeader.SoundFilePtr);
            auto ExtendedSoundInfo = CoDAssets::GameInstance->Read<WWIIStreamedSoundInfo>(LoadedSoundHeader.ExtendedInfoPtr);

            auto SoundPath = CoDAssets::GameInstance->ReadNullTerminatedString(LoadedSoundHeader.SoundFilePath);
            auto SoundName = CoDAssets::GameInstance->ReadNullTerminatedString(LoadedSoundHeader.SoundFileName);

            auto LoadedSound = new CoDSound_t();
            LoadedSound->AssetName = SoundName;
            LoadedSound->FullPath = SoundPath;
            LoadedSound->FrameCount = ExtendedSoundInfo.FrameCount;
            LoadedSound->FrameRate = ExtendedSoundInfo.FrameRate;
            LoadedSound->AssetPointer = LoadedSoundHeader.PackFileOffset;
            LoadedSound->AssetSize = LoadedSoundHeader.PackFileSize;
            LoadedSound->ChannelsCount = ExtendedSoundInfo.ChannelCount;
            LoadedSound->DataType = (ExtendedSoundInfo.Format == 6) ? SoundDataTypes::FLAC_WithHeader : SoundDataTypes::WAV_NeedsHeader;
            LoadedSound->IsFileEntry = true;
            LoadedSound->IsLocalized = (LoadedSoundHeader.IsLocalized > 0);
            LoadedSound->PackageIndex = LoadedSoundHeader.PackFileIndex;
            LoadedSound->AssetStatus = WraithAssetStatus::Loaded;
            LoadedSound->Length = LoadedSound->FrameRate ? (uint32_t)(1000.0f * (float)(LoadedSound->FrameCount / (float)(LoadedSound->FrameRate))) : 0;

            Out.push_back(LoadedSound);
            if (Sources) Sources->push_back(Spec);
        }
    }
}

std::unique_ptr<XModel_t> GameWorldWar2::ReadXModel(const CoDModel_t* Model)
{
    if (!CoDAssets::GameInstance->IsRunning()) return nullptr;

    const auto ModelHeaderData = CoDAssets::GameInstance->Read<WWIIXModelBase>(Model->AssetPointer);
    const auto ModelData = CoDAssets::GameInstance->Read<WWIIXModel>(ModelHeaderData.XModelPtr);

    auto ModelAsset = std::make_unique<XModel_t>(ModelData.NumLods);
    GameReaders::CopyModelBones(*ModelAsset, Model->AssetName, ModelData, 4, 2);
    ModelAsset->BoneRotationData = BoneDataTypes::DivideBySize;
    ModelAsset->IsModelStreamed = true;
    ModelAsset->BlendShapeNamesPtr = CoDAssets::GameInstance->Read<uint64_t>(ModelHeaderData.XModelPtr + 736);
    ModelAsset->BlendShapeCount = CoDAssets::GameInstance->Read<uint16_t>(ModelHeaderData.XModelPtr + 732);

    uint64_t Handles = ModelHeaderData.MaterialHandlesPtr;
    for (uint32_t i = 0; i < ModelData.NumLods; i++)
    {
        const auto& Lod = ModelData.ModelLods[i];
        ModelAsset->ModelLods.emplace_back(Lod.NumSurfs);
        ModelAsset->ModelLods[i].LodDistance = Lod.LodDistance;

        GameReaders::ReadSurfaces<WWIIXModelSurface, uint64_t>(ModelAsset->ModelLods[i], Lod.SurfsPtr, Lod.NumSurfs, Handles, ReadXMaterial,
                                                               [](XModelSubmesh_t& Submesh, const WWIIXModelSurface& Info)
                                                               {
                                                                   Submesh.VertListcount = Info.VertListCount;
                                                                   Submesh.RigidWeightsPtr = Info.RigidWeightsPtr;
                                                                   Submesh.VertexCount = Info.VertexCount;
                                                                   Submesh.FaceCount = Info.FacesCount;
                                                                   // the stream surface, read by LoadXModel
                                                                   Submesh.VertexPtr = Info.XStreamSurfacePtr;
                                                               });
    }
    return ModelAsset;
}

// An image whose pixels are resident (not in a .pak) is laid out differently from the running
// game's, so WWIIGfxImage and the live reader above don't apply:
// +0 name, +32 pixels (resident), +72 format (WWII enum, not DXGI), +76 width, +78 height,
// +84 size (whole mip chain), +88 mapType (3=2D/4=3D/5=cube), +92 levelCount.
namespace
{
    // WWII format enum -> DXGI. Format 2 is uncompressed RGBA (size/(width*height) == 4). Unknown
    // formats return 0 rather than guessing BC1: a wrong guess writes a plausible-looking garbage file.
    uint8_t WW2ImageFormat(uint8_t Format)
    {
        switch (Format)
        {
        case 1:
        case 2:  return 28;   // RGBA, uncompressed
        case 7:
        case 8:  return 71;   // BC1
        case 9:
        case 10: return 74;   // BC2
        case 11:
        case 12: return 77;   // BC3
        case 13: return 80;   // BC4
        case 15: return 84;   // BC5
        case 16: return 95;   // BC6
        case 18:
        case 19: return 98;   // BC7
        // 25 is deliberately absent: it is a 3D colour lookup table at 256 bytes per texel
        // (hdrclut_clut_identity, 32x32 for 262144 bytes), not a 2D image at all.
        default: return 0;
        }
    }
}

std::unique_ptr<XImageDDS> GameWorldWar2::DecodePakImage(const std::string& PakPath, uint64_t Offset, uint32_t Width, uint32_t Height,
                                                         uint8_t Format, bool Cube, ImageUsageType Usage)
{
    const uint8_t Dxgi = WW2ImageFormat(Format);
    if (Dxgi == 0 || Width == 0 || Height == 0) return nullptr;
    uint32_t ResultSize = 0;
    auto ImageData = PAKSupport::AWExtractImagePackage(PakPath, Offset, 0, ResultSize);
    if (ImageData == nullptr || ResultSize == 0) return nullptr;
    auto Result = CoDRawImageTranslator::TranslateBC(ImageData, ResultSize, Width, Height, Dxgi, 1, Cube);
    if (Result != nullptr && Usage == ImageUsageType::NormalMap && SettingsManager::GetSetting("patchnormals", "true") == "true")
        Result->ImagePatchType = ImagePatch::Normal_Expand;
    return Result;
}

std::unique_ptr<XImageDDS> GameWorldWar2::LoadResidentXImage(const XImage_t& Image)
{
    const auto Pixels = CoDAssets::GameInstance->Read<uint64_t>(Image.ImagePtr + 32);
    const auto Size   = CoDAssets::GameInstance->Read<uint32_t>(Image.ImagePtr + 84);
    const auto Width  = CoDAssets::GameInstance->Read<uint16_t>(Image.ImagePtr + 76);
    const auto Height = CoDAssets::GameInstance->Read<uint16_t>(Image.ImagePtr + 78);
    const auto Format = CoDAssets::GameInstance->Read<uint8_t>(Image.ImagePtr + 72);
    const auto MapType = CoDAssets::GameInstance->Read<uint8_t>(Image.ImagePtr + 88);

    // Traced to GH_WW2TRACE when set, so the formats that fail can be counted rather than guessed.
    const auto Trace = [&](const char* Outcome)
    {
        static FILE* File = nullptr;
        static bool Tried = false;

        if (!Tried)
        {
            Tried = true;
            char* Path = nullptr;
            size_t Length = 0;
            if (_dupenv_s(&Path, &Length, "GH_WW2TRACE") == 0 && Path != nullptr)
                fopen_s(&File, Path, "w");
            free(Path);
        }

        if (File == nullptr) return;

        fprintf(File, "%s\t%u\t%u\t%u\t%u\t%s\t", Image.ImageName.c_str(), (unsigned)Format,
                (unsigned)Width, (unsigned)Height, Size, Outcome);

        // The header itself for the ones that come back empty: their pixels are somewhere else,
        // and the bytes this layout calls padding are where to look.
        static int Dumped = 0;
        if (std::string(Outcome) != "ok" && Dumped++ < 12)
        {
            uint8_t Raw[128]{};
            if (CoDAssets::GameInstance->Read(Raw, Image.ImagePtr, sizeof(Raw)) == sizeof(Raw))
                for (size_t i = 0; i < sizeof(Raw); i++)
                    fprintf(File, "%02X", Raw[i]);
        }

        fprintf(File, "\n");
        fflush(File);
    };

    if (Pixels == 0 || Size == 0 || Width == 0 || Height == 0)
    {
        Trace("empty");
        return nullptr;
    }

    const auto Dxgi = WW2ImageFormat(Format);
    if (Dxgi == 0)
    {
        Trace("unknown_format");
        return nullptr;
    }

    auto ImageData = std::make_unique<uint8_t[]>(Size);
    if (CoDAssets::GameInstance->Read(ImageData.get(), Pixels, Size) != Size)
    {
        Trace("short_read");
        return nullptr;
    }

    // Level count 1: the buffer holds the whole mip chain, only the top one is wanted.
    auto Result = CoDRawImageTranslator::TranslateBC(
        ImageData, Size, Width, Height, Dxgi, 1, MapType == 5);

    Trace(Result ? "ok" : "translate_null");

    if (Result != nullptr && Image.ImageUsage == ImageUsageType::NormalMap &&
        SettingsManager::GetSetting("patchnormals", "true") == "true")
    {
        Result->ImagePatchType = ImagePatch::Normal_Expand;
    }

    return Result;
}

std::unique_ptr<XSound> GameWorldWar2::ReadXSound(const CoDSound_t* Sound)
{
    // WAV data needs a WAV header
    auto Result = std::make_unique<XSound>();

    uint32_t DataOffset = 0;

    if (Sound->DataType == SoundDataTypes::WAV_NeedsHeader)
    {
        Result->DataBuffer = new int8_t[Sound->AssetSize + Sound::GetMaximumWAVHeaderSize()];
        Result->DataType = SoundDataTypes::WAV_WithHeader;
        Result->DataSize = (uint32_t)(Sound->AssetSize + Sound::GetMaximumWAVHeaderSize());

        DataOffset += Sound::GetMaximumWAVHeaderSize();

        Sound::WriteWAVHeaderToStream(Result->DataBuffer, (uint32_t)Sound->FrameRate, (uint32_t)Sound->ChannelsCount, (uint32_t)Sound->AssetSize);
    }
    else if (Sound->DataType == SoundDataTypes::FLAC_NeedsHeader)
    {
        Result->DataBuffer = new int8_t[Sound->AssetSize + Sound::GetMaximumFLACHeaderSize()];
        Result->DataType = SoundDataTypes::FLAC_WithHeader;
        Result->DataSize = (uint32_t)(Sound->AssetSize + Sound::GetMaximumFLACHeaderSize());

        DataOffset += Sound::GetMaximumFLACHeaderSize();

        Sound::WriteFLACHeaderToStream(Result->DataBuffer, Sound->FrameRate, Sound->ChannelsCount, Sound->FrameCount);
    }
    else
    {
        Result->DataBuffer = new int8_t[Sound->AssetSize];
        Result->DataType = SoundDataTypes::FLAC_WithHeader;
        Result->DataSize = (uint32_t)(Sound->AssetSize);
    }

    if (!Sound->IsFileEntry)
    {
        uintptr_t ResultRead = 0;
        auto TempBuffer = CoDAssets::GameInstance->Read(Sound->AssetPointer, Sound->AssetSize, ResultRead);

        if (TempBuffer != nullptr)
        {
            std::memcpy(Result->DataBuffer + DataOffset, TempBuffer, ResultRead);

            delete[] TempBuffer;
        }
        else
        {
            return nullptr;
        }
    }
    else
    {
        const std::string PackagePath = SoundPackagePath(Sound);
        if (PackagePath.empty())
            return nullptr;

        auto Reader = BinaryReader();
        Reader.Open(PackagePath);

        Reader.SetPosition(Sound->AssetPointer);

        uint64_t ResultRead = 0;
        Reader.Read((uint8_t*)Result->DataBuffer + DataOffset, Sound->AssetSize, ResultRead);
    }

    return Result;
}

std::vector<uint8_t> GameWorldWar2::ReadSoundHead(const CoDSound_t* Sound, size_t N)
{
    std::vector<uint8_t> Head;
    N = (size_t)std::min<uint64_t>(N, Sound->AssetSize);
    if (N == 0) return Head;
    if (!Sound->IsFileEntry)
    {
        uintptr_t Read = 0;
        auto Buffer = CoDAssets::GameInstance->Read(Sound->AssetPointer, N, Read);
        if (Buffer == nullptr) return Head;
        Head.assign((const uint8_t*)Buffer, (const uint8_t*)Buffer + Read);
        delete[] Buffer;
        return Head;
    }
    const std::string PackagePath = SoundPackagePath(Sound);
    if (PackagePath.empty()) return Head;
    auto Reader = BinaryReader();
    if (!Reader.Open(PackagePath)) return Head;
    Reader.SetPosition(Sound->AssetPointer);
    Head.resize(N);
    uint64_t Read = 0;
    Reader.Read(Head.data(), N, Read);
    Head.resize((size_t)Read);
    return Head;
}

// A streamed sound's package: soundfile<N>.pak, or a language folder's <prefix>soundfile<N>.pak;
// "" when it is not there
std::string GameWorldWar2::SoundPackagePath(const CoDSound_t* Sound)
{
    std::error_code Ec;
    const std::filesystem::path Root = CoDAssets::GamePackageCache->GetPackagesPath();
    std::filesystem::path PackagePath;
    if (!Sound->IsLocalized)
        PackagePath = Root / std::format("soundfile{}.pak", Sound->PackageIndex);
    else
    {
        // the zone's language, else the first language folder the install has
        const auto Own = Localizations.find(SoundLanguage);
        if (Own != Localizations.end())
            PackagePath = Root / Own->first / std::format("{}soundfile{}.pak", Own->second, Sound->PackageIndex);
        else
            for (const auto& [Folder, Prefix] : Localizations)
                if (std::filesystem::is_directory(Root / Folder, Ec))
                {
                    PackagePath = Root / Folder / std::format("{}soundfile{}.pak", Prefix, Sound->PackageIndex);
                    break;
                }
    }
    if (PackagePath.empty() || !std::filesystem::is_regular_file(PackagePath, Ec))
        return std::string();
    return PackagePath.string();
}

thread_local std::string GameWorldWar2::SoundLanguage;

std::string GameWorldWar2::LanguageOfZone(const std::string& FastFilePath)
{
    const std::filesystem::path Ff(FastFilePath);
    const std::string Folder = Strings::ToLower(Ff.parent_path().filename().string());
    if (Localizations.contains(Folder)) return Folder;
    const std::string Name = Strings::ToLower(Ff.filename().string());
    for (const auto& [Language, Prefix] : Localizations)
        if (Name.starts_with(Prefix)) return Language;
    return std::string();
}

std::string GameWorldWar2::InstallOfZone(const std::string& FastFilePath)
{
    const std::filesystem::path Dir = std::filesystem::path(FastFilePath).parent_path();
    return Localizations.contains(Strings::ToLower(Dir.filename().string())) ? Dir.parent_path().string() : Dir.string();
}

std::unique_ptr<XAnim_t> GameWorldWar2::ReadXAnim(const CoDAnim_t* Animation)
{
    return GameReaders::ReadAnim<WWIIXAnim, WWIIXAnimDeltaParts>(Animation, 4, { "viewmodel_", "vm_", "va_" },
                                                                 [](XAnim_t& Anim, const WWIIXAnim& Data)
                                                                 {
                                                                     Anim.AdditiveAnimation = Data.AssetType == 0x6;
                                                                     // All bone type indicies are ushort
                                                                     Anim.BoneTypeSize = 2;
                                                                 });
}

thread_local GameReaders::ImagesElsewhereFn GameWorldWar2::ImagesElsewhere;

const XMaterial_t GameWorldWar2::ReadXMaterial(uint64_t MaterialPointer)
{
    auto MaterialData = CoDAssets::GameInstance->Read<WWIIXMaterial>(MaterialPointer);
    const auto RawName = CoDAssets::GameInstance->ReadNullTerminatedString(MaterialData.NamePtr);

    XMaterial_t Result(MaterialData.ImageCount);
    Result.MaterialName = FileSystems::GetFileNameWithoutExtension(RawName);
    Result.MaterialName = Strings::Replace(Result.MaterialName, "*", "");

    // Image name: first pointer in image
    GameReaders::AddMaterialImages<WWIIXMaterialImage>(Result, MaterialData.ImageTablePtr, MaterialData.ImageCount,
                                                       [](uint64_t Image) { return GameReaders::ImageNameAt<uint64_t>(Image, 0); }, GameReaders::SemanticUsage);

    // a name-only header: the zone defining the material names its images
    if (MaterialData.ImageCount == 0 && !RawName.empty() && RawName[0] == ',')
        GameReaders::AddImagesElsewhere(Result, ImagesElsewhere, RawName.substr(1), GameReaders::SemanticUsage);

    return Result;
}

#pragma pack(push, 1)
struct BlendshapeMeshInfo
{
    uint32_t VertexCount;
    float XScale;
    float YScale;
    float ZScale;
    float Scale;
    float Padding;
    uint64_t DataPtr1;
    uint64_t ViewPtr0;
    uint64_t ViewPtr1;
};
#pragma pack(pop)
#pragma pack(push, 1)
struct WW2BlendShapeVertex
{
    uint16_t A;
    uint16_t B;
    uint16_t VertexIndex;
    uint16_t C;
    uint16_t Unk1;
    uint16_t Unk2;
    uint16_t Unk3;
    uint16_t Unk4;
    float Scale;
};
#pragma pack(pop)

void GameWorldWar2::LoadXModel(const std::unique_ptr<XModel_t>& Model, const XModelLod_t& ModelLOD, const std::unique_ptr<WraithModel>& ResultModel)
{
    bool ExportColors = (SettingsManager::GetSetting("exportvtxcolor", "true") == "true");

    ResultModel->PrepareSubmeshes((uint32_t)ModelLOD.Submeshes.size());

#if _DEBUG
    for (size_t b = 0; b < Model->BlendShapeCount; b++)
    {
        ResultModel->BlendShapes.push_back(CoDAssets::GameStringHandler(CoDAssets::GameInstance->Read<uint32_t>(Model->BlendShapeNamesPtr + b * sizeof(uint32_t))));
    }
#endif

    for (auto& Submesh : ModelLOD.Submeshes)
    {
        auto& Mesh = ResultModel->AddSubmesh();

        // COD has 1 material per submesh
        Mesh.AddMaterial(Submesh.MaterialIndex);

        Mesh.PrepareMesh(Submesh.VertexCount, Submesh.FaceCount);

        // Readers for all data types, global so that we can use them when streaming...
        MemoryReader VertexPosReader;
        MemoryReader VertexNormReader;
        MemoryReader VertexUVReader;
        MemoryReader VertexColorReader;
        MemoryReader VertexWeightReader;
        MemoryReader FaceIndiciesReader;
        MemoryReader BlendShapesReader;

        std::unique_ptr<uint8_t[]> StreamDataBuffer = nullptr;

        auto StreamMeshInfo = CoDAssets::GameInstance->Read<WWIIXStreamSurface>(Submesh.VertexPtr);
        auto StreamMeshExtended = CoDAssets::GameInstance->Read<WWIIXStreamSurfaceInfo>(StreamMeshInfo.LoadedSurfaceInfoPtr);

        if (StreamMeshInfo.LoadedSurfaceInfoPtr == 0)
        {
            uint32_t ResultSize = 0;
            // Flag bit 0 clear: the zone holds the surface's buffer itself (UnknownPtr1, Unknown2
            // bytes), laid out as a streamed one. Otherwise stream it from the packages.
            if ((StreamMeshInfo.Unknown1 & 1) == 0 && StreamMeshInfo.UnknownPtr1 != 0 && StreamMeshInfo.Unknown2 != 0)
            {
                StreamDataBuffer = std::make_unique<uint8_t[]>(StreamMeshInfo.Unknown2);
                if (CoDAssets::GameInstance->Read(StreamDataBuffer.get(), StreamMeshInfo.UnknownPtr1, StreamMeshInfo.Unknown2) == StreamMeshInfo.Unknown2)
                    ResultSize = StreamMeshInfo.Unknown2;
            }
            else
            {
                auto Key = Hashing::HashXXHashString(std::string(StreamMeshInfo.SubmeshKeyHash));
                StreamDataBuffer = CoDAssets::GamePackageCache->ExtractPackageObject(Key, ResultSize);
            }

            if (StreamDataBuffer != nullptr && ResultSize > 0)
            {
                uint32_t CurrentPosition = 0;

                FaceIndiciesReader.Setup((int8_t*)(StreamDataBuffer.get() + CurrentPosition), Submesh.FaceCount * sizeof(uint16_t) * 3, true);
                CurrentPosition += (Submesh.FaceCount * sizeof(uint16_t) * 3);
                CurrentPosition = ((size_t)CurrentPosition + 0x7) & 0xFFFFFFFFFFFFFFF8;

                VertexPosReader.Setup((int8_t*)(StreamDataBuffer.get() + CurrentPosition), Submesh.VertexCount * sizeof(Vector3), true);
                CurrentPosition += (Submesh.VertexCount * sizeof(Vector3));
                CurrentPosition = ((size_t)CurrentPosition + 0x7) & 0xFFFFFFFFFFFFFFF8;

                CurrentPosition += (Submesh.VertexCount * sizeof(uint32_t));
                CurrentPosition = ((size_t)CurrentPosition + 0x7) & 0xFFFFFFFFFFFFFFF8;

                VertexColorReader.Setup((int8_t*)(StreamDataBuffer.get() + CurrentPosition), Submesh.VertexCount * sizeof(uint32_t), true);
                CurrentPosition += (Submesh.VertexCount * 4);
                CurrentPosition = ((size_t)CurrentPosition + 0x7) & 0xFFFFFFFFFFFFFFF8;

                VertexUVReader.Setup((int8_t*)(StreamDataBuffer.get() + CurrentPosition), Submesh.VertexCount * sizeof(uint32_t), true);
                CurrentPosition += (Submesh.VertexCount * sizeof(uint32_t));
                CurrentPosition = ((size_t)CurrentPosition + 0x7) & 0xFFFFFFFFFFFFFFF8;

                VertexNormReader.Setup((int8_t*)(StreamDataBuffer.get() + CurrentPosition), Submesh.VertexCount * sizeof(uint32_t), true);
                CurrentPosition += (Submesh.VertexCount * sizeof(uint32_t));
                CurrentPosition = ((size_t)CurrentPosition + 0x7) & 0xFFFFFFFFFFFFFFF8;

                CurrentPosition += (Submesh.VertexCount * sizeof(uint32_t));
                CurrentPosition = ((size_t)CurrentPosition + 0x7) & 0xFFFFFFFFFFFFFFF8;

                if (Submesh.VertListcount == 0)
                {
                    // Read them, 0x10 per vertex
                    VertexWeightReader.Setup((int8_t*)(StreamDataBuffer.get() + CurrentPosition), Submesh.VertexCount * 0x10, true);
                    CurrentPosition += (Submesh.VertexCount * 0x10);
                }
            }
            else
            {
#if _DEBUG
                printf("Found submesh that isn't loaded: %s\n", std::string(StreamMeshInfo.SubmeshKeyHash).c_str());
#endif
                continue;
            }
        }
        else
        {
            uintptr_t MemoryResult = 0;

            auto Buffer = CoDAssets::GameInstance->Read(StreamMeshExtended.VertexPositionsPtr, Submesh.VertexCount * sizeof(Vector3), MemoryResult);
            VertexPosReader.Setup(Buffer, MemoryResult);
            Buffer = CoDAssets::GameInstance->Read(StreamMeshExtended.VertexNormalsPtr, Submesh.VertexCount * sizeof(uint32_t), MemoryResult);
            VertexNormReader.Setup(Buffer, MemoryResult);
            Buffer = CoDAssets::GameInstance->Read(StreamMeshExtended.VertexUVsPtr, Submesh.VertexCount * sizeof(uint32_t), MemoryResult);
            VertexUVReader.Setup(Buffer, MemoryResult);
            Buffer = CoDAssets::GameInstance->Read(StreamMeshExtended.VertexColorsPtr, Submesh.VertexCount * sizeof(uint32_t), MemoryResult);
            VertexColorReader.Setup(Buffer, MemoryResult);

            if (Submesh.VertListcount == 0)
            {
                // Read them, 0x10 per vertex
                Buffer = CoDAssets::GameInstance->Read(StreamMeshExtended.VertexWeightsPtr, (size_t)Submesh.VertexCount * 0x10, MemoryResult);
                VertexWeightReader.Setup(Buffer, MemoryResult);
            }

            Buffer = CoDAssets::GameInstance->Read(StreamMeshExtended.FacesPtr, Submesh.FaceCount * sizeof(uint16_t) * 3, MemoryResult);
            FaceIndiciesReader.Setup(Buffer, MemoryResult);
        }

        // Pre-allocate vertex weights (Data defaults to weight 1.0 on bone 0)
        auto VertexWeights = std::vector<WeightsData>(Submesh.VertexCount);

        uint32_t WeightDataIndex = 0;

        for (uint32_t i = 0; i < Submesh.VertListcount; i++)
        {
            // Simple weights build, rigid, just apply the proper bone id
            auto RigidInfo = CoDAssets::GameInstance->Read<GfxRigidVerts64>(Submesh.RigidWeightsPtr + (i * sizeof(GfxRigidVerts64)));
            for (uint32_t w = 0; w < RigidInfo.VertexCount; w++)
            {
                VertexWeights[WeightDataIndex].BoneValues[0] = (RigidInfo.BoneIndex / 64);
                WeightDataIndex++;
            }
        }

        if (Submesh.VertListcount == 0)
        {
            for (uint32_t i = 0; i < Submesh.VertexCount; i++)
            {
                VertexWeights[i].BoneValues[0] = VertexWeightReader.Read<uint16_t>();
                VertexWeights[i].BoneValues[1] = VertexWeightReader.Read<uint16_t>();
                VertexWeights[i].BoneValues[2] = VertexWeightReader.Read<uint16_t>();
                VertexWeights[i].BoneValues[3] = VertexWeightReader.Read<uint16_t>();

                VertexWeights[i].WeightValues[1] = VertexWeightReader.Read<uint16_t>() / 65536.0f;
                VertexWeights[i].WeightValues[2] = VertexWeightReader.Read<uint16_t>() / 65536.0f;
                VertexWeights[i].WeightValues[3] = VertexWeightReader.Read<uint16_t>() / 65536.0f;
                VertexWeights[i].WeightValues[0] = (1.0f - (VertexWeights[i].WeightValues[1] + VertexWeights[i].WeightValues[2] + VertexWeights[i].WeightValues[3]));

                VertexWeights[i].WeightCount = VertexWeightReader.Read<uint8_t>();
                VertexWeightReader.Advance(1);
            }
        }

        for (uint32_t i = 0; i < Submesh.VertexCount; i++)
        {
            auto& Vertex = Mesh.AddVertex();

            Vertex.Position = VertexPosReader.Read<Vector3>();

            auto PackedNormal = VertexNormReader.Read<int32_t>();

            Vertex.Normal = Vector3(
                (float)((float)((float)(PackedNormal & 0x3FF) / 1023.0) * 2.0) - 1.0f,
                (float)((float)((float)((PackedNormal >> 10) & 0x3FF) / 1023.0) * 2.0) - 1.0f,
                (float)((float)((float)((PackedNormal >> 20) & 0x3FF) / 1023.0) * 2.0) - 1.0f);

            Vertex.Color[0] = ExportColors ? VertexColorReader.Read<uint8_t>() : 0xFF;
            Vertex.Color[1] = ExportColors ? VertexColorReader.Read<uint8_t>() : 0xFF;
            Vertex.Color[2] = ExportColors ? VertexColorReader.Read<uint8_t>() : 0xFF;
            Vertex.Color[3] = ExportColors ? VertexColorReader.Read<uint8_t>() : 0xFF;

            auto UVU = HalfFloats::ToFloat(VertexUVReader.Read<uint16_t>());
            auto UVV = HalfFloats::ToFloat(VertexUVReader.Read<uint16_t>());

            Vertex.AddUVLayer(UVU, UVV);

            auto& WeightValue = VertexWeights[i];

            for (uint32_t w = 0; w < WeightValue.WeightCount; w++)
            {
                Vertex.AddVertexWeight(WeightValue.BoneValues[w], WeightValue.WeightValues[w]);
            }
        }

        for (uint32_t i = 0; i < Submesh.FaceCount; i++)
        {
            auto& Face = Mesh.AddFace();

            Face.Index1 = FaceIndiciesReader.Read<uint16_t>();
            Face.Index2 = FaceIndiciesReader.Read<uint16_t>();
            Face.Index3 = FaceIndiciesReader.Read<uint16_t>();
        }

#if _DEBUG
        uint16_t BlendShapeCount = (StreamMeshInfo.TriIndiciesCount >> 16) & 0xFFFF;
        uintptr_t SizeRead = 0;

        for (uint16_t bv = 0; bv < BlendShapeCount; bv++)
        {
            auto Info = CoDAssets::GameInstance->Read<BlendshapeMeshInfo>(StreamMeshInfo.UnknownPtr2 + bv * sizeof(BlendshapeMeshInfo));
            // WW2 stores an entry for every blendshape but uses only some per mesh
            if (Info.VertexCount == 0)
            {
                continue;
            }
            auto ToRead = (size_t)Info.VertexCount * 20;
            MemoryReader ShapeVertsReader(CoDAssets::GameInstance->Read(Info.DataPtr1, ToRead, SizeRead), ToRead, false);
            if (SizeRead != ToRead)
            {
                continue;
            }

            for (size_t k = 0; k < Info.VertexCount; k++)
            {
                auto ShapeVert = ShapeVertsReader.Read<WW2BlendShapeVertex>();

                auto Offset = Vector3(
                    ShapeVert.A * Info.Scale + Info.XScale,
                    ShapeVert.B * Info.Scale + Info.YScale,
                    ShapeVert.C * Info.Scale + Info.ZScale);

                Mesh.Verticies[ShapeVert.VertexIndex].BlendShapeDeltas.push_back(std::make_pair(bv, Offset));
            }
        }
#endif
    }
}
