#include "stdafx.h"

#include "GameAdvancedWarfare.h"

#include "GameReaders.h"
#include "PAKSupport.h"

// Traces every image decode to GH_AWTRACE when that variable names a file: tells which step
// (handler call, payload read, block translation, write) failed.
static void AwTrace(const char* Format, ...)
{
    static FILE* Trace = nullptr;
    static bool Tried = false;

    if (!Tried)
    {
        Tried = true;
        char* Path = nullptr;
        size_t Length = 0;
        if (_dupenv_s(&Path, &Length, "GH_AWTRACE") == 0 && Path != nullptr)
            fopen_s(&Trace, Path, "w");
        free(Path);
    }

    if (Trace == nullptr) return;

    va_list Args;
    va_start(Args, Format);
    vfprintf(Trace, Format, Args);
    va_end(Args);
    fflush(Trace);
}

// A loaded sound's copy in soundfile<Index>.pak (LoadedSound + 8): the game reads its samples
// from there at load; the zone keeps zeros in their place.
#pragma pack(push, 1)
struct AWLoadedSoundPackage
{
    uint8_t Localized;
    uint8_t Flags;
    int16_t Index;
    uint8_t Padding[4];
    uint64_t Offset;
    uint64_t Size;
};
#pragma pack(pop)

void GameAdvancedWarfare::AddAliasSounds(const AWSoundAlias& Alias, const std::string& SoundName, std::set<uint64_t>& Unique, std::vector<CoDAsset_t*>& Out,
                                         bool FromPackages)
{
    // a loaded sound read from its package rather than from memory
    const auto FromPackage = [FromPackages](uint64_t LoadedSoundPtr, CoDSound_t* Sound)
    {
        if (!FromPackages) return;
        const auto Package = CoDAssets::GameInstance->Read<AWLoadedSoundPackage>(LoadedSoundPtr + 8);
        if (Package.Index <= 0) return;
        Sound->IsFileEntry = true;
        Sound->PackageIndex = (uint16_t)Package.Index;
        Sound->AssetPointer = Package.Offset;
        Sound->AssetSize = Package.Size;
        Sound->IsLocalized = Package.Localized > 0;
    };

    for (uint32_t j = 0; j < Alias.EntryCount; j++)
    {
        auto SoundAliasEntry = CoDAssets::GameInstance->Read<AWSoundAliasEntry>(Alias.EntriesPtr + (j * sizeof(AWSoundAliasEntry)));
        auto SoundFileSpec = CoDAssets::GameInstance->Read<AWSoundAliasFileSpec>(SoundAliasEntry.FileSpecPtr);
        if (SoundFileSpec.Type == 1)
        {
            auto LoadedSoundPtr = CoDAssets::GameInstance->Read<uint64_t>(SoundAliasEntry.FileSpecPtr + 8);
            if (Unique.insert(LoadedSoundPtr).second == false)
                continue;
            auto LoadedSoundInfo = CoDAssets::GameInstance->Read<AWLoadedSound>(LoadedSoundPtr);
            auto LoadedSoundName = CoDAssets::GameInstance->ReadNullTerminatedString(LoadedSoundInfo.NamePtr);
            auto LoadedSound = GameReaders::LoadedSound(LoadedSoundInfo, LoadedSoundName, GameReaders::LoadedFormat(LoadedSoundInfo.Format));
            FromPackage(LoadedSoundPtr, LoadedSound);
            Out.push_back(LoadedSound);
        }
        else if (SoundFileSpec.Type == 2)
        {
            if (Unique.insert(SoundAliasEntry.FileSpecPtr).second == false)
                continue;
            auto StreamedSoundInfo = CoDAssets::GameInstance->Read<AWStreamedSound>(SoundAliasEntry.FileSpecPtr + 8);
            if (StreamedSoundInfo.Exists)
                Out.push_back(GameReaders::StreamedSound(StreamedSoundInfo, SoundName, j));
        }
        else if (SoundFileSpec.Type == 3)
        {
            auto PrimedAudioInfo = CoDAssets::GameInstance->Read<AWPrimedSound>(SoundAliasEntry.FileSpecPtr + 8);
            if (Unique.insert(PrimedAudioInfo.LoadedSoundPtr).second == false)
                continue;
            if (PrimedAudioInfo.Exists)
            {
                auto LoadedSoundInfo = CoDAssets::GameInstance->Read<AWLoadedSound>(PrimedAudioInfo.LoadedSoundPtr);
                auto LoadedSoundName = CoDAssets::GameInstance->ReadNullTerminatedString(LoadedSoundInfo.NamePtr);
                Out.push_back(GameReaders::PrimedSound(LoadedSoundInfo, LoadedSoundName, PrimedAudioInfo, GameReaders::LoadedFormat(LoadedSoundInfo.Format)));
            }
        }
    }
}

std::unique_ptr<XAnim_t> GameAdvancedWarfare::ReadXAnim(const CoDAnim_t* Animation)
{
    return GameReaders::ReadAnim<AWXAnim, AWXAnimDeltaParts>(Animation, 4, { "viewmodel_", "vm_", "va_" });
}

std::unique_ptr<XModel_t> GameAdvancedWarfare::ReadXModel(const CoDModel_t* Model)
{
    return GameReaders::ReadModel<AWXModel, AWXModelSurface, uint64_t>(Model, 4, ReadXMaterial);
}

thread_local GameReaders::ImagesElsewhereFn GameAdvancedWarfare::ImagesElsewhere;

const XMaterial_t GameAdvancedWarfare::ReadXMaterial(uint64_t MaterialPointer)
{
    // Image name pointer: end of image - 8
    return GameReaders::ReadNamedMaterial<AWXMaterial, AWXMaterialImage>(MaterialPointer, sizeof(AWGfxImage) - 8, ImagesElsewhere,
                                                                         GameReaders::SemanticUsage);
}

std::unique_ptr<XImageDDS> GameAdvancedWarfare::TranslateImage(std::unique_ptr<uint8_t[]> Data, uint32_t Size, uint8_t Format, uint8_t MapType,
                                                               uint32_t Width, uint32_t Height, ImageUsageType Usage, uint8_t Levels)
{
    if (Data == nullptr) return nullptr;

    auto Result = GameReaders::TranslatePakImage(std::move(Data), Size, Format, MapType, Width, Height, Usage, Levels);
    AwTrace("  translate %s (size=%u %ux%u fmt=%u)\n", Result ? "ok" : "NULL", Size, Width, Height, (unsigned)Format);
    return Result;
}

std::unique_ptr<XImageDDS> GameAdvancedWarfare::DecodePakImage(const AWGfxImage& Info, const std::string& PakPath, uint64_t Start, uint64_t End,
                                                               uint32_t Width, uint32_t Height, ImageUsageType Usage)
{
    uint32_t ResultSize = 0;
    auto ImageData = PAKSupport::AWExtractImagePackage(PakPath, Start, End - Start, ResultSize);
    if (ImageData == nullptr)
    {
        AwTrace("  BAIL no payload\n");
        return nullptr;
    }
    return TranslateImage(std::move(ImageData), ResultSize, Info.ImageFormat, Info.MapType, Width, Height, Usage);
}
