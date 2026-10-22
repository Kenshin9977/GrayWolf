#include "stdafx.h"

#include "GameModernWarfareRM.h"

#include "GameReaders.h"
#include "Strings.h"

thread_local GameReaders::ImagesElsewhereFn GameModernWarfareRM::ImagesElsewhere;

void GameModernWarfareRM::AddAliasSounds(const MWRSoundAlias& SoundResult, const std::string& SoundName, std::set<uint64_t>& UniqueEntries, std::vector<CoDAsset_t*>& Out)
{
    for (uint32_t j = 0; j < SoundResult.EntryCount; j++)
    {
        auto SoundAliasEntry = CoDAssets::GameInstance->Read<MWRSoundAliasEntry>(SoundResult.EntriesPtr + (j * sizeof(MWRSoundAliasEntry)));

        auto SoundFileSpec = CoDAssets::GameInstance->Read<MWRSoundAliasFileSpec>(SoundAliasEntry.FileSpecPtr);
        if (SoundFileSpec.Type == 1)
        {
            auto LoadedSoundPtr = CoDAssets::GameInstance->Read<uint64_t>(SoundAliasEntry.FileSpecPtr + 8);
            if (UniqueEntries.insert(LoadedSoundPtr).second == false)
                continue;
            auto LoadedSoundInfo = CoDAssets::GameInstance->Read<AWLoadedSound>(LoadedSoundPtr);
            auto LoadedSoundName = CoDAssets::GameInstance->ReadNullTerminatedString(LoadedSoundInfo.NamePtr);

            auto LoadedSound = GameReaders::LoadedSound(LoadedSoundInfo, LoadedSoundName, GameReaders::LoadedFormat(LoadedSoundInfo.Format));
            // a pak index: the samples are in soundfile<index>.pak, the zone holds zeros for them;
            // the byte at 8 (the StreamFileName's isLocalized, as a streamed spec's first byte)
            // puts that pak in the language folder (french/fra_soundfile<index>.pak)
            if (LoadedSoundInfo.PackFileIndex > 0)
            {
                LoadedSound->IsFileEntry = true;
                LoadedSound->PackageIndex = (uint32_t)LoadedSoundInfo.PackFileIndex;
                LoadedSound->AssetPointer = LoadedSoundInfo.PackFileOffset;
                LoadedSound->IsLocalized = LoadedSoundInfo.Padding1[0] != 0;
            }            Out.push_back(LoadedSound);
        }
        else if (SoundFileSpec.Type == 2)
        {
            auto StreamedSoundInfo = CoDAssets::GameInstance->Read<AWStreamedSound>(SoundAliasEntry.FileSpecPtr + 8);
            if (StreamedSoundInfo.Exists)
                Out.push_back(GameReaders::StreamedSound(StreamedSoundInfo, SoundName, j));
        }
        else if (SoundFileSpec.Type == 3)
        {
            auto PrimedAudioInfo = CoDAssets::GameInstance->Read<AWPrimedSound>(SoundAliasEntry.FileSpecPtr + 8);
            if (UniqueEntries.insert(PrimedAudioInfo.LoadedSoundPtr).second == false)
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

std::unique_ptr<XAnim_t> GameModernWarfareRM::ReadXAnim(const CoDAnim_t* Animation)
{
    return GameReaders::ReadAnim<MWRXAnim, MWRXAnimDeltaParts>(Animation, 4, { "viewmodel_", "vm_", "h1_wpn", "h1_vm" });
}

std::unique_ptr<XModel_t> GameModernWarfareRM::ReadXModel(const CoDModel_t* Model)
{
    return GameReaders::ReadModel<MWRXModel, MWRXModelSurface, uint64_t>(Model, 4, ReadXMaterial);
}

XMaterial_t GameModernWarfareRM::ReadXMaterial(uint64_t MaterialPointer)
{
    auto MaterialData = CoDAssets::GameInstance->Read<MWRXMaterial>(MaterialPointer);

    XMaterial_t Result(MaterialData.ImageCount);
    const std::string RawName = CoDAssets::GameInstance->ReadNullTerminatedString(MaterialData.NamePtr);
    Result.MaterialName = FileSystems::GetFileNameWithoutExtension(RawName);
    Result.MaterialName = Strings::Replace(Result.MaterialName, "*", "");

    // Image name pointer: end of image - 8
    GameReaders::AddMaterialImages<MWRXMaterialImage>(Result, MaterialData.ImageTablePtr, MaterialData.ImageCount,
                                                      [](uint64_t Image) { return GameReaders::ImageNameAt<uint64_t>(Image, sizeof(MWRGfxImage) - 8); },
                                                      GameReaders::SemanticUsage);

    if (MaterialData.ImageCount == 0)
        GameReaders::AddImagesElsewhere(Result, ImagesElsewhere, RawName, GameReaders::SemanticUsage);

    return Result;
}
