#include "stdafx.h"

#include "GameVanguard.h"

#include "CoDAssets.h"
#include "ModernIwReaders.h"
#include "CoDRawImageTranslator.h"
#include "DBGameFiles.h"

#include "SABSupport.h"

#include "Strings.h"
#include "FileSystems.h"
#include "MemoryReader.h"
#include "SettingsManager.h"
#include "HalfFloats.h"

#include <filesystem>
#include <format>
#include <fstream>

WraithNameIndex GameVanguard::StringCache = WraithNameIndex();
WraithNameIndex GameVanguard::AssetNameCache = WraithNameIndex();

struct VGGfxRigidVerts
{
    uint16_t BoneIndex;
    uint16_t VertexCount;
    uint16_t FacesCount;
    uint16_t FacesIndex;
};

#pragma pack(push, 1)
struct VGGfxStreamVertex
{
    uint64_t PackedPosition; // Packed 21bits, scale + offset in Mesh Info
    uint32_t BiNormal;
    uint16_t UVUPosition;
    uint16_t UVVPosition;
    uint32_t NormalQuaternion;
};
#pragma pack(pop)

struct VGGfxStreamFace
{
    uint16_t Index1;
    uint16_t Index2;
    uint16_t Index3;
};

uint64_t VGHashSoundString(const std::string& Value)
{
    uint64_t Result = 0xCBF29CE484222325;

    for (uint32_t i = 0; i < Value.length(); i++)
    {
        Result ^= tolower(Value[i]);
        Result *= 0x100000001B3;
    }

    return Result;
}

static_assert(sizeof(VGXAssetPoolData) == 0x18, "Invalid Pool Data Size (Expected 0x18)");

struct VGGfxImage
{
    uint64_t NamePtr;
    uint8_t Unk00[12];
    uint8_t Unk010;
    uint8_t Unk011;
    uint8_t Unk012;
    uint8_t Unk013;
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
    uint8_t UnkByte6;
    uint8_t UnkByte7;
    uint64_t Unk04;
    uint64_t MipMapsPtr;
    uint64_t PrimedMipPtr;
    uint64_t LoadedImagePtr;
};

#pragma pack(push, 1)
struct VGGfxMip
{
    uint64_t HashID;
    uint8_t Padding[8];
    uint32_t Size;
    uint16_t Width;
    uint16_t Height;
};
#pragma pack(pop)

template <size_t Size>
struct VGGfxMipArray
{
    VGGfxMip MipMaps[Size];

    size_t GetImageSize(size_t i)
    {
        if (i == 0)
            return (size_t)(MipMaps[i].Size >> 4);
        else
            return (size_t)((MipMaps[i].Size >> 4) - (MipMaps[i - 1].Size >> 4));
    }

    size_t GetMipCount()
    {
        return Size;
    }
};

bool GameVanguard::LoadAssets()
{
    bool NeedsAnims = (SettingsManager::GetSetting("showxanim", "true") == "true");
    bool NeedsModels = (SettingsManager::GetSetting("showxmodel", "true") == "true");
    bool NeedsImages = (SettingsManager::GetSetting("showximage", "false") == "true");
    bool NeedsSounds = (SettingsManager::GetSetting("showxsounds", "false") == "true");
    bool NeedsRawFiles = (SettingsManager::GetSetting("showxrawfiles", "false") == "true");
    bool NeedsMaterials = (SettingsManager::GetSetting("showxmtl", "false") == "true");
    bool NeedsExtInfo = (SettingsManager::GetSetting("needsextinfo", "true") == "true");

    if (NeedsModels)
    {
        auto Pool = CoDAssets::GameInstance->Read<ps::XAssetPool64>(ps::state->PoolsAddress + 9 * sizeof(ps::XAssetPool64));
        ps::PoolParser64(Pool.Root, CoDAssets::ParasyteRequest, [&NeedsExtInfo](ps::XAsset64& Asset)
        {
            auto ModelResult = CoDAssets::GameInstance->Read<VGXModel>(Asset.Header);
            auto ModelName = FileSystems::GetFileName(CoDAssets::GameInstance->ReadNullTerminatedString(ModelResult.NamePtr));
            auto LoadedModel = new CoDModel_t();
            LoadedModel->AssetName = ModelName;
            LoadedModel->AssetPointer = Asset.Header;
            // Some models have thousands of bones with no info (no parent list): no bones
            if ((ModelResult.NumBones + ModelResult.UnkBoneCount) > 1 && ModelResult.ParentListPtr == 0)
                LoadedModel->BoneCount = 0;
            else
                LoadedModel->BoneCount = ModelResult.NumBones + ModelResult.UnkBoneCount;
            LoadedModel->LodCount = ModelResult.NumLods;
            LoadedModel->AssetStatus = Asset.Temp == 1 ? WraithAssetStatus::Placeholder : WraithAssetStatus::Loaded;
            if (NeedsExtInfo)
            {
                for (size_t i = 0; i < LoadedModel->BoneCount; i++)
                {
                    auto BoneIndex = CoDAssets::GameInstance->Read<uint32_t>(ModelResult.BoneIDsPtr + i * 4);

                    LoadedModel->BoneNames.emplace_back(CoDAssets::GameStringHandler(BoneIndex));
                }
            }
            CoDAssets::GameAssets->LoadedAssets.push_back(LoadedModel);
        });
    }

    if (NeedsAnims)
    {
        auto Pool = CoDAssets::GameInstance->Read<ps::XAssetPool64>(ps::state->PoolsAddress + 7 * sizeof(ps::XAssetPool64));
        ps::PoolParser64(Pool.Root, CoDAssets::ParasyteRequest, [](ps::XAsset64& Asset)
        {
            auto AnimResult = CoDAssets::GameInstance->Read<VGXAnim>(Asset.Header);
            auto AnimName = CoDAssets::GameInstance->ReadNullTerminatedString(AnimResult.NamePtr);

            auto LoadedAnim = new CoDAnim_t();
            LoadedAnim->AssetName    = AnimName;
            LoadedAnim->AssetPointer = Asset.Header;
            LoadedAnim->Framerate    = AnimResult.Framerate;
            LoadedAnim->FrameCount   = AnimResult.NumFrames;
            LoadedAnim->AssetStatus  = Asset.Temp == 1 ? WraithAssetStatus::Placeholder : WraithAssetStatus::Loaded;
            LoadedAnim->BoneCount    = AnimResult.TotalBoneCount;
            CoDAssets::GameAssets->LoadedAssets.push_back(LoadedAnim);
        });
    }

    if (NeedsImages)
    {
        auto Pool = CoDAssets::GameInstance->Read<ps::XAssetPool64>(ps::state->PoolsAddress + 19 * sizeof(ps::XAssetPool64));
        ps::PoolParser64(Pool.Root, CoDAssets::ParasyteRequest, [](ps::XAsset64& Asset)
        {
            auto ImageResult = CoDAssets::GameInstance->Read<VGGfxImage>(Asset.Header);
            auto ImageName = FileSystems::GetFileName(CoDAssets::GameInstance->ReadNullTerminatedString(ImageResult.NamePtr));

            if(ImageResult.MipMapsPtr > 0)
            {

                auto LoadedImage = new CoDImage_t();
                LoadedImage->AssetName = ImageName;
                LoadedImage->AssetPointer = Asset.Header;
                LoadedImage->Width = (uint16_t)ImageResult.Width;
                LoadedImage->Height = (uint16_t)ImageResult.Height;
                LoadedImage->Format = ImageResult.ImageFormat;
                LoadedImage->AssetStatus = WraithAssetStatus::Loaded;

                CoDAssets::GameAssets->LoadedAssets.push_back(LoadedImage);
            }
        });
    }

    if (NeedsSounds)
    {
#if _DEBUG
        std::ofstream Writer("vg_sound_names.csv");
#endif

        auto Pool = CoDAssets::GameInstance->Read<ps::XAssetPool64>(ps::state->PoolsAddress + 22 * sizeof(ps::XAssetPool64));
#if _DEBUG
        ps::PoolParser64(Pool.Root, CoDAssets::ParasyteRequest, [&Writer](ps::XAsset64& Asset)
#else
        ps::PoolParser64(Pool.Root, CoDAssets::ParasyteRequest, [](ps::XAsset64& Asset)
#endif
        {
            auto SoundResult = CoDAssets::GameInstance->Read<VGSoundBank>(Asset.Header);
            auto SoundBankInfo = CoDAssets::GameInstance->Read<VGSoundBankInfo>(SoundResult.SoundBankPtr);

            auto SkipBlankAudio = SettingsManager::GetSetting("skipblankaudio", "false") == "true";

            if (SoundBankInfo.BankFilePointer > 0)
            {
                auto Header = CoDAssets::GameInstance->Read<SABFileHeader>(SoundBankInfo.BankFilePointer);

                // Verify magic first, same for all SAB files
                // The magic is ('2UX#')
                if (Header.Magic != 0x23585532)
                {
                    return;
                }

                auto NamesOffset = CoDAssets::GameInstance->Read<uint64_t>(SoundBankInfo.BankFilePointer + 0x250);

                for (size_t i = 0; i < Header.EntriesCount; i++)
                {
                    auto Name = CoDAssets::GameInstance->ReadNullTerminatedString(SoundBankInfo.BankFilePointer + NamesOffset + i * 128);
                    AssetNameCache.NameDatabase[VGHashSoundString(Name)] = Name;
                }

                for (uint32_t i = 0; i < Header.EntriesCount; i++)
                {
                    auto Entry = CoDAssets::GameInstance->Read<SABv17Entry>(SoundBankInfo.BankFilePointer + Header.EntryTableOffset + i * sizeof(SABv17Entry));

                    if (SkipBlankAudio && Entry.Size <= 0)
                    {
                        continue;
                    }

                    auto LoadedSound = new CoDSound_t();

                    LoadedSound->FrameRate = Entry.FrameRate;
                    LoadedSound->FrameCount = Entry.FrameCount;
                    LoadedSound->ChannelsCount = Entry.ChannelCount;
                    LoadedSound->AssetPointer = SoundBankInfo.BankFilePointer + (Entry.Offset + Entry.SeekTableLength);
                    LoadedSound->AssetSize = Entry.Size;
                    LoadedSound->AssetStatus = WraithAssetStatus::Loaded;
                    LoadedSound->IsFileEntry = false;
                    LoadedSound->Length = (uint32_t)(1000.0f * (float)(LoadedSound->FrameCount / (float)(LoadedSound->FrameRate)));
                    LoadedSound->DataType = SoundDataTypes::Opus_Interleaved;

                    auto NameResult = AssetNameCache.NameDatabase.find(Entry.Key);

                    if (NameResult != AssetNameCache.NameDatabase.end())
                    {
                        LoadedSound->AssetName = FileSystems::GetFileNamePurgeExtensions(NameResult->second);
                        LoadedSound->FullPath = FileSystems::GetDirectoryName(NameResult->second);
                    }
                    else
                    {
                        LoadedSound->AssetName = FileSystems::GetFileNamePurgeExtensions(std::format("xsound_{:x}", Entry.Key));
                        LoadedSound->FullPath = FileSystems::GetDirectoryName(LoadedSound->AssetName);
                    }

#if _DEBUG
                    Writer << std::format("{:x},{}\n", Entry.Key, (std::filesystem::path(LoadedSound->FullPath) / LoadedSound->AssetName).string());
#endif 

                    CoDAssets::GameAssets->LoadedAssets.push_back(LoadedSound);
                }
            }

            if (SoundResult.StreamedSoundsPtr > 0 && SoundResult.StreamedSoundCount > 0)
            {
                for (size_t i = 0; i < SoundResult.StreamedSoundCount; i++)
                {
                    auto Entry = CoDAssets::GameInstance->Read<SABv17Entry>(SoundResult.StreamedSoundsPtr + i * sizeof(SABv17Entry));

                    if (SkipBlankAudio && Entry.Size <= 0)
                    {
                        continue;
                    }

                    auto LoadedSound = new CoDSound_t();

                    LoadedSound->FrameRate = Entry.FrameRate;
                    LoadedSound->FrameCount = Entry.FrameCount;
                    LoadedSound->ChannelsCount = Entry.ChannelCount;
                    LoadedSound->AssetPointer = SoundResult.StreamedSoundsPtr + i * sizeof(SABv17Entry);
                    LoadedSound->AssetSize = Entry.Size;
                    LoadedSound->AssetStatus = WraithAssetStatus::Loaded;
                    LoadedSound->IsFileEntry = false;
                    LoadedSound->Length = (uint32_t)(1000.0f * (float)(LoadedSound->FrameCount / (float)(LoadedSound->FrameRate)));
                    LoadedSound->DataType = SoundDataTypes::Opus_Interleaved_Streamed;

                    auto NameResult = AssetNameCache.NameDatabase.find(Entry.Key);

                    if (NameResult != AssetNameCache.NameDatabase.end())
                    {
                        LoadedSound->AssetName = FileSystems::GetFileNamePurgeExtensions(NameResult->second);
                        LoadedSound->FullPath = FileSystems::GetDirectoryName(NameResult->second);
                    }
                    else
                    {
                        LoadedSound->AssetName = FileSystems::GetFileNamePurgeExtensions(std::format("xsound_{:x}", Entry.Key));
                        LoadedSound->FullPath = FileSystems::GetDirectoryName(LoadedSound->AssetName);
                    }

#if _DEBUG
                    Writer << std::format("{:x},{}\n", Entry.Key, (std::filesystem::path(LoadedSound->FullPath) / LoadedSound->AssetName).string());
#endif 

                    CoDAssets::GameAssets->LoadedAssets.push_back(LoadedSound);
                }
            }
        });
    }

    if (NeedsRawFiles)
    {
        auto Pool = CoDAssets::GameInstance->Read<ps::XAssetPool64>(ps::state->PoolsAddress + 23 * sizeof(ps::XAssetPool64));
        ps::PoolParser64(Pool.Root, CoDAssets::ParasyteRequest, [](ps::XAsset64& Asset)
        {
            auto SoundResult = CoDAssets::GameInstance->Read<VGSoundBank>(Asset.Header);
            auto RawfileName = CoDAssets::GameInstance->ReadNullTerminatedString(SoundResult.NamePtr) + ".sabs";

            // Actually the streamer info pool
            auto Info = CoDAssets::GameInstance->Read<VGSoundBankInfo>(SoundResult.SoundBankPtr);

            auto LoadedRawfile = new CoDRawFile_t();
            LoadedRawfile->AssetName = FileSystems::GetFileName(RawfileName);
            LoadedRawfile->RawFilePath = FileSystems::GetDirectoryName(RawfileName);
            LoadedRawfile->AssetPointer = Asset.Header;
            LoadedRawfile->AssetSize = Info.BankFileSize;
            LoadedRawfile->RawDataPointer = Info.BankFilePointer;
            LoadedRawfile->AssetStatus = WraithAssetStatus::Loaded;

            CoDAssets::GameAssets->LoadedAssets.push_back(LoadedRawfile);
        });
    }

    if (NeedsMaterials)
    {
        auto Pool = CoDAssets::GameInstance->Read<ps::XAssetPool64>(ps::state->PoolsAddress + 11 * sizeof(ps::XAssetPool64));
        ps::PoolParser64(Pool.Root, CoDAssets::ParasyteRequest, [](ps::XAsset64& Asset)
        {
            auto MaterialResult = CoDAssets::GameInstance->Read<VGXMaterial>(Asset.Header);
            auto MaterialName = CoDAssets::GameInstance->ReadNullTerminatedString(MaterialResult.NamePtr);

            auto LoadedMaterial = new CoDMaterial_t();
            LoadedMaterial->AssetName = FileSystems::GetFileName(Strings::Replace(MaterialName, "*", ""));
            LoadedMaterial->AssetPointer = Asset.Header;
            LoadedMaterial->ImageCount = MaterialResult.ImageCount;
            LoadedMaterial->AssetStatus = WraithAssetStatus::Loaded;
            CoDAssets::GameAssets->LoadedAssets.push_back(LoadedMaterial);
        });
    }

    return true;
}

std::unique_ptr<XAnim_t> GameVanguard::ReadXAnim(const CoDAnim_t* Animation)
{
    if (CoDAssets::GameInstance->IsRunning())
    {
        auto Anim = std::make_unique<XAnim_t>();

        auto AnimData = CoDAssets::GameInstance->Read<VGXAnim>(Animation->AssetPointer);

        Anim->AnimationName = Animation->AssetName;
        Anim->FrameCount = AnimData.NumFrames;
        Anim->FrameRate = AnimData.Framerate;

        // No point, breaks it in SETools, wait for Cast to implement full Additive support
        if (AnimData.AssetType == 0x6)
        {
            Anim->AdditiveAnimation = true;
        }
        Anim->LoopingAnimation = (AnimData.Flags & 1);

        auto AnimDeltaData = CoDAssets::GameInstance->Read<VGXAnimDeltaParts>(*(uint64_t*)&AnimData.PaddingNew[0]);

        Anim->BoneIDsPtr = AnimData.BoneIDsPtr;
        Anim->DataBytesPtr = AnimData.DataBytePtr;
        Anim->DataShortsPtr = AnimData.DataShortPtr;
        Anim->DataIntsPtr = AnimData.DataIntPtr;
        Anim->RandomDataBytesPtr = AnimData.RandomDataBytePtr;
        Anim->RandomDataShortsPtr = AnimData.RandomDataShortPtr;
        Anim->RandomDataIntsPtr = AnimData.RandomDataIntPtr;
        Anim->LongIndiciesPtr = AnimData.LongIndiciesPtr;
        Anim->NotificationsPtr = AnimData.NotificationsPtr;

        Anim->BoneIndexSize = 4;

        Anim->NoneRotatedBoneCount         = AnimData.NoneRotatedBoneCount;
        Anim->TwoDRotatedBoneCount         = AnimData.TwoDRotatedBoneCount;
        Anim->NormalRotatedBoneCount       = AnimData.NormalRotatedBoneCount;
        Anim->TwoDStaticRotatedBoneCount   = AnimData.TwoDStaticRotatedBoneCount;
        Anim->NormalStaticRotatedBoneCount = AnimData.NormalStaticRotatedBoneCount;
        Anim->NormalTranslatedBoneCount    = AnimData.NormalTranslatedBoneCount;
        Anim->PreciseTranslatedBoneCount   = AnimData.PreciseTranslatedBoneCount;
        Anim->StaticTranslatedBoneCount    = AnimData.StaticTranslatedBoneCount;
        Anim->NoneTranslatedBoneCount      = AnimData.NoneTranslatedBoneCount;
        Anim->TotalBoneCount               = AnimData.TotalBoneCount;
        Anim->NotificationCount            = AnimData.NotificationCount;
        Anim->BlendShapeWeightCount        = *(uint16_t*)&AnimData.Padding3[3];
        Anim->BlendShapeNamesPtr           = *(uint64_t*)&AnimData.Padding3[11];
        Anim->BlendShapeWeightsPtr         = *(uint64_t*)&AnimData.Padding3[19];

        Anim->DeltaTranslationPtr = AnimDeltaData.DeltaTranslationsPtr;
        Anim->Delta2DRotationsPtr = AnimDeltaData.Delta2DRotationsPtr;
        Anim->Delta3DRotationsPtr = AnimDeltaData.Delta3DRotationsPtr;

        Anim->RotationType = AnimationKeyTypes::DivideBySize;
        Anim->TranslationType = AnimationKeyTypes::MinSizeTable;

        Anim->SupportsInlineIndicies = true;

        return Anim;
    }
    return nullptr;
}

std::unique_ptr<XModel_t> GameVanguard::ReadXModel(const CoDModel_t* Model)
{
    if (CoDAssets::GameInstance->IsRunning())
    {
        auto ModelData = CoDAssets::GameInstance->Read<VGXModel>(Model->AssetPointer);

        auto ModelAsset = std::make_unique<XModel_t>(ModelData.NumLods);

        ModelAsset->ModelName = Model->AssetName;

        // Some models have thousands of bones with no info (no parent list): no bones
        if ((ModelData.NumBones + ModelData.UnkBoneCount) > 1 && ModelData.ParentListPtr == 0)
        {
            ModelAsset->BoneCount = 0;
            ModelAsset->RootBoneCount = 0;
        }
        else
        {
            ModelAsset->BoneCount = ModelData.NumBones + ModelData.UnkBoneCount;
            ModelAsset->RootBoneCount = ModelData.NumRootBones;
        }

        ModelAsset->BoneRotationData = BoneDataTypes::DivideBySize;

        ModelAsset->IsModelStreamed = true;

        ModelAsset->BoneIDsPtr = ModelData.BoneIDsPtr;
        ModelAsset->BoneIndexSize = 4;

        ModelAsset->BoneParentsPtr = ModelData.ParentListPtr;
        ModelAsset->BoneParentSize = 2;

        ModelAsset->RotationsPtr = ModelData.RotationsPtr;
        ModelAsset->TranslationsPtr = ModelData.TranslationsPtr;

        ModelAsset->BaseMatriciesPtr = ModelData.BaseMatriciesPtr;

        ModelAsset->BlendShapeNamesPtr = *(uint64_t*)&ModelData.Padding4[64];

        for (uint32_t i = 0; i < ModelData.NumLods; i++)
        {
            auto ModelLod = CoDAssets::GameInstance->Read<VGXModelLod>(ModelData.ModelLodsPtr + i * sizeof(VGXModelLod));

            ModelAsset->ModelLods.emplace_back(ModelLod.NumSurfs);
            auto& LodReference = ModelAsset->ModelLods[i];

            LodReference.LodDistance = ModelLod.LodDistance;

            LodReference.LODStreamInfoPtr = ModelLod.MeshPtr;

            auto XSurfacePtr = ModelLod.SurfsPtr;

            for (uint32_t s = 0; s < ModelLod.NumSurfs; s++)
            {
                LodReference.Submeshes.emplace_back();
                auto& SubmeshReference = LodReference.Submeshes[s];

                auto SurfaceInfo = CoDAssets::GameInstance->Read<VGXModelSurface>(XSurfacePtr);

                SubmeshReference.RigidWeightsPtr  = SurfaceInfo.RigidWeightsPtr;
                SubmeshReference.VertListcount    = SurfaceInfo.VertListCount;
                SubmeshReference.VertexCount      = SurfaceInfo.VertexCount;
                SubmeshReference.FaceCount        = SurfaceInfo.FacesCount;
                SubmeshReference.VertexPtr        = SurfaceInfo.Offsets[0];
                SubmeshReference.VertexUVsPtr     = SurfaceInfo.Offsets[1];
                SubmeshReference.VertexNormalsPtr = SurfaceInfo.Offsets[2];
                SubmeshReference.FacesPtr         = SurfaceInfo.Offsets[3];
                SubmeshReference.VertexColorPtr   = SurfaceInfo.Offsets[6];

                if (SurfaceInfo.NewScale != -1)
                {
                    SubmeshReference.Scale = SurfaceInfo.NewScale;
                    SubmeshReference.XOffset = 0;
                    SubmeshReference.YOffset = 0;
                    SubmeshReference.ZOffset = 0;
                }
                else
                {
                    SubmeshReference.Scale = fmaxf(fmaxf(SurfaceInfo.Min, SurfaceInfo.Scale), SurfaceInfo.Max);
                    SubmeshReference.XOffset = SurfaceInfo.XOffset;
                    SubmeshReference.YOffset = SurfaceInfo.YOffset;
                    SubmeshReference.ZOffset = SurfaceInfo.ZOffset;
                }

                SubmeshReference.WeightCounts[0] = SurfaceInfo.WeightCounts[0];
                SubmeshReference.WeightCounts[1] = SurfaceInfo.WeightCounts[1];
                SubmeshReference.WeightCounts[2] = SurfaceInfo.WeightCounts[2];
                SubmeshReference.WeightCounts[3] = SurfaceInfo.WeightCounts[3];
                SubmeshReference.WeightCounts[4] = SurfaceInfo.WeightCounts[4];
                SubmeshReference.WeightCounts[5] = SurfaceInfo.WeightCounts[5];
                SubmeshReference.WeightCounts[6] = SurfaceInfo.WeightCounts[6];
                SubmeshReference.WeightCounts[7] = SurfaceInfo.WeightCounts[7];
                SubmeshReference.WeightsPtr = SurfaceInfo.WeightsPtr;
                SubmeshReference.BlendShapesPtr = *(uint64_t*)&SurfaceInfo.Padding7[8];

                auto MaterialHandle = CoDAssets::GameInstance->Read<uint64_t>(ModelData.MaterialHandlesPtr);
                LodReference.Materials.emplace_back(ReadXMaterial(MaterialHandle));

                XSurfacePtr += sizeof(VGXModelSurface);
                ModelData.MaterialHandlesPtr += sizeof(uint64_t);
            }
        }

        return ModelAsset;
    }
    return nullptr;
}

std::unique_ptr<XImageDDS> GameVanguard::ReadXImage(const CoDImage_t* Image)
{
    return LoadXImage(XImage_t(ImageUsageType::DiffuseMap, 0, Image->AssetPointer, Image->AssetName));
}

std::unique_ptr<XSound> GameVanguard::ReadXSound(const CoDSound_t* Sound)
{
    std::unique_ptr<uint8_t[]> SoundBuffer = nullptr;
    size_t OpusOffset = 0, Held = (size_t)Sound->AssetSize;

    if (Sound->DataType == SoundDataTypes::Opus_Interleaved_Streamed)
    {
        auto Entry = CoDAssets::GameInstance->Read<SABv17Entry>(Sound->AssetPointer);

        uint32_t ResultSize = 0;
        SoundBuffer = CoDAssets::GamePackageCache->ExtractPackageObject(Entry.Offset, Entry.Size + Entry.SeekTableLength, ResultSize);
        // Set offset, skip seek table
        OpusOffset = Entry.SeekTableLength;
        Held = ResultSize;
    }
    else
    {
        SoundBuffer = std::make_unique<uint8_t[]>((size_t)Sound->AssetSize);

        if (CoDAssets::GameInstance->Read(SoundBuffer.get(), Sound->AssetPointer, Sound->AssetSize) != (size_t)Sound->AssetSize)
            return nullptr;
    }

    if (SoundBuffer != nullptr)
    {
        return SABSupport::OpusPackets(SoundBuffer.get(), Held, OpusOffset, (size_t)Sound->AssetSize, Sound->FrameRate, Sound->ChannelsCount, Sound->FrameCount,
                                       SABSupport::OpusPreSkipUnmeasured);
    }
    
    return nullptr;
}

void GameVanguard::TranslateRawfile(const CoDRawFile_t * Rawfile, const std::string & ExportPath)
{
    std::error_code Ec;
    std::string ExportFolder = ExportPath;

    if (SettingsManager::GetSetting("keeprawpath", "true") == "true" && !Rawfile->RawFilePath.empty())
    {
        ExportFolder = (std::filesystem::path(ExportFolder) / Rawfile->RawFilePath).string();
    }

    std::filesystem::create_directories(ExportFolder, Ec);

    auto Bank = CoDAssets::GameInstance->Read<VGSoundBank>(Rawfile->AssetPointer);

    // Actually the streamer info pool
    auto Info = CoDAssets::GameInstance->Read<VGSoundBankInfo>(Bank.SoundBankPtr);

    // Always stream: the data in memory is unreliable
    uint32_t ResultSize = 0;
    std::unique_ptr<uint8_t[]> Data = CoDAssets::GamePackageCache->ExtractPackageObject(Info.StreamKey, ResultSize);

    if (Data != nullptr)
    {
        auto Writer = BinaryWriter();
        if (Writer.Create((std::filesystem::path(ExportFolder) / Rawfile->AssetName).string()))
        {
            Writer.Write(Data.get(), (uint32_t)Info.BankFileSize);
        }
    }
}

const XMaterial_t GameVanguard::ReadXMaterial(uint64_t MaterialPointer)
{
    auto MaterialData = CoDAssets::GameInstance->Read<VGXMaterial>(MaterialPointer);

    XMaterial_t Result(MaterialData.ImageCount);
    Result.MaterialName = FileSystems::GetFileNameWithoutExtension(CoDAssets::GameInstance->ReadNullTerminatedString(MaterialData.NamePtr));
    Result.MaterialName = Strings::Replace(Result.MaterialName, "*", "");

    for (uint32_t m = 0; m < MaterialData.ImageCount; m++)
    {
        auto ImageInfo = CoDAssets::GameInstance->Read<VGXMaterialImage>(MaterialData.ImageTablePtr);
        // Image name pointer: end of image - 8
        auto ImageName = CoDAssets::GameInstance->ReadNullTerminatedString(CoDAssets::GameInstance->Read<uint64_t>(ImageInfo.ImagePtr));

        auto DefaultUsage = ImageUsageType::Unknown;
        switch (ImageInfo.Type)
        {
        case 0:
            DefaultUsage = ImageUsageType::DiffuseMap;
            break;
        }

        Result.Images.emplace_back(DefaultUsage, ImageInfo.Type, ImageInfo.ImagePtr, ImageName);

        MaterialData.ImageTablePtr += sizeof(VGXMaterialImage);
    }

    return Result;
}

void GameVanguard::PrepareVertexWeights(std::vector<WeightsData>& Weights, const XModelSubmesh_t & Submesh)
{
    uint32_t WeightDataIndex = 0;
    if (Submesh.RigidWeightsPtr > 0)
        ModernIwReaders::ReadRigidWeights<VGGfxRigidVerts>(Weights, WeightDataIndex, Submesh);

    if (Submesh.WeightsPtr > 0)
    {
        auto WeightsData = ModernIwReaders::ReadSkinnedWeightData(Submesh);
        ModernIwReaders::ReadSkinnedWeights(WeightsData, Weights, WeightDataIndex, Submesh);
    }
}

std::unique_ptr<XImageDDS> GameVanguard::LoadXImage(const XImage_t& Image)
{
    auto ImageInfo = CoDAssets::GameInstance->Read<VGGfxImage>(Image.ImagePtr);

    if (ImageInfo.MipMapsPtr == 0)
        return nullptr;

    VGGfxMipArray<32> Mips{};
    size_t MipCount = std::min((size_t)ImageInfo.UnkByte6, (size_t)32);

    if (CoDAssets::GameInstance->Read((uint8_t*)Mips.MipMaps, ImageInfo.MipMapsPtr, MipCount * sizeof(VGGfxMip)) != (MipCount * sizeof(VGGfxMip)))
        return nullptr;

    // Fallback: the best mip present in the local files, in case the CDN is not viable
    size_t Fallback = 0;
    size_t HighestIndex = (size_t)ImageInfo.UnkByte6 - 1;

    for (size_t i = 0; i < MipCount; i++)
    {
        if (CoDAssets::GamePackageCache->Exists(Mips.MipMaps[i].HashID))
        {
            Fallback = i;
        }
    }

    switch (ImageInfo.ImageFormat)
    {
    case 0:  ImageInfo.ImageFormat = 71; break;
    case 1:  ImageInfo.ImageFormat = 61; break;
    case 2:  ImageInfo.ImageFormat = 65; break;
    case 3:  ImageInfo.ImageFormat = 65; break;
    case 4:  ImageInfo.ImageFormat = 49; break;
    case 5:  ImageInfo.ImageFormat = 49; break;
    case 6:  ImageInfo.ImageFormat = 28; break;
    case 7:  ImageInfo.ImageFormat = 28; break;
    case 8:  ImageInfo.ImageFormat = 71; break;
    case 9:  ImageInfo.ImageFormat = 71; break;
    case 10: ImageInfo.ImageFormat = 63; break;
    case 11: ImageInfo.ImageFormat = 51; break;
    case 12: ImageInfo.ImageFormat = 56; break;
    case 13: ImageInfo.ImageFormat = 35; break;
    case 14: ImageInfo.ImageFormat = 11; break;
    case 15: ImageInfo.ImageFormat = 58; break;
    case 16: ImageInfo.ImageFormat = 54; break;
    case 17: ImageInfo.ImageFormat = 34; break;
    case 18: ImageInfo.ImageFormat = 10; break;
    case 19: ImageInfo.ImageFormat = 41; break;
    case 20: ImageInfo.ImageFormat = 16; break;
    case 21: ImageInfo.ImageFormat = 2;  break;
    case 22: ImageInfo.ImageFormat = 40; break;
    case 23: ImageInfo.ImageFormat = 20; break;
    case 24: ImageInfo.ImageFormat = 62; break;
    case 25: ImageInfo.ImageFormat = 57; break;
    case 26: ImageInfo.ImageFormat = 42; break;
    case 27: ImageInfo.ImageFormat = 17; break;
    case 28: ImageInfo.ImageFormat = 3;  break;
    case 29: ImageInfo.ImageFormat = 25; break;
    case 30: ImageInfo.ImageFormat = 85; break;
    case 31: ImageInfo.ImageFormat = 85; break;
    case 32: ImageInfo.ImageFormat = 24; break;
    case 33: ImageInfo.ImageFormat = 67; break;
    case 34: ImageInfo.ImageFormat = 67; break;
    case 35: ImageInfo.ImageFormat = 71; break;
    case 36: ImageInfo.ImageFormat = 71; break;
    case 37: ImageInfo.ImageFormat = 71; break;
    case 38: ImageInfo.ImageFormat = 74; break;
    case 39: ImageInfo.ImageFormat = 77; break;
    case 40: ImageInfo.ImageFormat = 77; break;
    case 41: ImageInfo.ImageFormat = 80; break;
    case 42: ImageInfo.ImageFormat = 84; break;
    case 43: ImageInfo.ImageFormat = 84; break;
    case 44: ImageInfo.ImageFormat = 95; break;
    case 45: ImageInfo.ImageFormat = 96; break;
    case 46: ImageInfo.ImageFormat = 98; break;
    case 47: ImageInfo.ImageFormat = 98; break;
    default: ImageInfo.ImageFormat = 71; break;
    }

    std::unique_ptr<uint8_t[]> ImageData = nullptr;
    size_t ImageSize = 0;

    if (Fallback != HighestIndex && CoDAssets::OnDemandCache != nullptr)
    {
        uint32_t PackageSize = 0;
        ImageData = CoDAssets::OnDemandCache->ExtractPackageObject(Mips.MipMaps[HighestIndex].HashID, Mips.GetImageSize(HighestIndex), PackageSize);
        ImageSize = PackageSize;
    }

    // Mips above the fallback: try the on-demand version from the CDN
    if (Fallback != HighestIndex && CoDAssets::CDNDownloader != nullptr)
    {
        ImageData = CoDAssets::CDNDownloader->ExtractCDNObject(Mips.MipMaps[HighestIndex].HashID, Mips.GetImageSize(HighestIndex), ImageSize);
    }

    if (ImageData == nullptr)
    {
        uint32_t PackageSize = 0;
        ImageData = CoDAssets::GamePackageCache->ExtractPackageObject(Mips.MipMaps[Fallback].HashID, Mips.GetImageSize(Fallback), PackageSize);
        ImageSize = PackageSize;
        HighestIndex = Fallback;
    }

    if (ImageData != nullptr)
    {
        auto Result = CoDRawImageTranslator::TranslateBC(
            ImageData,
            ImageSize,
            Mips.MipMaps[HighestIndex].Width,
            Mips.MipMaps[HighestIndex].Height,
            ImageInfo.ImageFormat);

        return Result;
    }

    return nullptr;
}

Vector3 VanguardTransformNormal(Quaternion quat, Vector3 up)
{
    // Generate the normal by rotating up around the normal value
    const Vector3 a(
        quat.Y * up.Z - quat.Z * up.Y + up.X * quat.W,
        quat.Z * up.X - quat.X * up.Z + up.Y * quat.W,
        quat.X * up.Y - quat.Y * up.X + up.Z * quat.W);
    const Vector3 b(
        quat.Y * a.Z - quat.Z * a.Y,
        quat.Z * a.X - quat.X * a.Z,
        quat.X * a.Y - quat.Y * a.X);
    return Vector3(
        up.X + b.X + b.X,
        up.Y + b.Y + b.Y,
        up.Z + b.Z + b.Z);
}

// Unpacks a 4-D Quaternion Normal
Vector3 VanguardUnpackNormalQuat(uint32_t packedQuat)
{
    auto Up = Vector3(0, 0, 1.0f);
    auto LargestComponent = packedQuat >> 30;

    auto x = ((((packedQuat >> 00) & 0x3FF) / 511.5f) - 1.0f) / 1.4142135f;
    auto y = ((((packedQuat >> 10) & 0x3FF) / 511.5f) - 1.0f) / 1.4142135f;
    auto z = ((((packedQuat >> 20) & 0x1FF) / 255.5f) - 1.0f) / 1.4142135f;
    auto w = sqrtf(1 - x * x - y * y - z * z);

    switch (LargestComponent)
    {
    case 0: return VanguardTransformNormal(Quaternion(w, x, y, z), Up);
    case 1: return VanguardTransformNormal(Quaternion(x, w, y, z), Up);
    case 2: return VanguardTransformNormal(Quaternion(x, y, w, z), Up);
    case 3: return VanguardTransformNormal(Quaternion(x, y, z, w), Up);
    default: return Vector3(1.0f, 0.0f, 0.0f);
    }
}

struct VGBlendShapeInfo
{
    uint16_t ShapeCount;
    uint16_t NameCount;
    uint16_t Unk1;
    uint64_t WeightNames;
    uint64_t UnkPtr;
};

struct VGBlendShapeMap
{
    uint16_t NameIndex;
    uint16_t ShapeIndex;
    float UnknownOne;
};

struct MeshBlendShapesData
{
    uint64_t VertexShapes;
    uint64_t UnkDataPtr;
    uint32_t VertexShapesDataSize;
    uint16_t ShapeCount;
    uint16_t UnkCount;
    uint16_t NumVertexExtended;
    uint16_t NumVerts;
    uint16_t UnkValue0;
    uint16_t UnkValue;
};

void GameVanguard::LoadXModel(const std::unique_ptr<XModel_t>& Model, const XModelLod_t& ModelLOD, const std::unique_ptr<WraithModel>& ResultModel)
{
    auto ScaleConstant = (1.0f / 0x1FFFFF) * 2.0f;
    bool ExportColors = (SettingsManager::GetSetting("exportvtxcolor", "true") == "true");
    auto MeshInfo = CoDAssets::GameInstance->Read<VGXModelMesh>(ModelLOD.LODStreamInfoPtr);
    auto BufferInfo = CoDAssets::GameInstance->Read<VGXModelMeshBufferInfo>(MeshInfo.MeshBufferPointer);

    std::unique_ptr<uint8_t[]> MeshDataBuffer = nullptr;
    uint64_t MeshDataBufferSize = 0;

    // Even a streamed mesh can be in memory if loaded
    if (BufferInfo.BufferPtr != 0)
    {
        uintptr_t ResultSize = 0;
        MeshDataBuffer = std::make_unique<uint8_t[]>(BufferInfo.BufferSize);
        auto TemporaryBuffer = CoDAssets::GameInstance->Read(BufferInfo.BufferPtr, BufferInfo.BufferSize, ResultSize);

        if (TemporaryBuffer != nullptr)
        {
            MeshDataBuffer = std::make_unique<uint8_t[]>(BufferInfo.BufferSize);
            std::memcpy(MeshDataBuffer.get(), TemporaryBuffer, (size_t)ResultSize);
            MeshDataBufferSize = ResultSize;
            delete[] TemporaryBuffer;
        }
    }
    else
    {
        uint32_t ResultSize = 0;
        MeshDataBuffer = CoDAssets::GamePackageCache->ExtractPackageObject(MeshInfo.LODStreamKey, BufferInfo.BufferSize, ResultSize);
        MeshDataBufferSize = ResultSize;
    }

    if (MeshDataBuffer != nullptr)
    {
        ResultModel->PrepareSubmeshes((uint32_t)ModelLOD.Submeshes.size());

        // Readers for all data types, global so that we can use them when streaming...
        MemoryReader VertexPosReader;
        MemoryReader VertexNormReader;
        MemoryReader VertexUVReader;
        MemoryReader VertexColorReader;
        MemoryReader VertexWeightReader;
        MemoryReader FaceIndiciesReader;

        std::unique_ptr<uint32_t[]> ShapeIndices = nullptr;

        if (Model->BlendShapeNamesPtr != 0)
        {
            auto BlendShapeInfo = CoDAssets::GameInstance->Read<VGBlendShapeInfo>(Model->BlendShapeNamesPtr);

            ShapeIndices = std::make_unique<uint32_t[]>(BlendShapeInfo.ShapeCount);

            for (size_t i = 0; i < BlendShapeInfo.NameCount; i++)
            {
                ResultModel->BlendShapes.push_back(CoDAssets::GameStringHandler(CoDAssets::GameInstance->Read<uint32_t>(BlendShapeInfo.WeightNames + i * sizeof(uint32_t))));
            }

            for (size_t i = 0; i < BlendShapeInfo.ShapeCount; i++)
            {
                auto ShapeMap = CoDAssets::GameInstance->Read<VGBlendShapeMap>(BlendShapeInfo.UnkPtr + i * sizeof(VGBlendShapeMap));
                ShapeIndices[ShapeMap.ShapeIndex] = ShapeMap.NameIndex;
            }
        }

        for (auto& Submesh : ModelLOD.Submeshes)
        {
            auto& Mesh = ResultModel->AddSubmesh();

            // COD has 1 material per submesh
            Mesh.AddMaterial(Submesh.MaterialIndex);

            Mesh.PrepareMesh(Submesh.VertexCount, Submesh.FaceCount);

            // Pre-allocate vertex weights (Data defaults to weight 1.0 on bone 0)
            auto VertexWeights = std::vector<WeightsData>(Submesh.VertexCount);

            if (ResultModel->BoneCount() > 1) PrepareVertexWeights(VertexWeights, Submesh);

            VertexPosReader.Setup((int8_t*)MeshDataBuffer.get() + Submesh.VertexPtr, Submesh.VertexCount * sizeof(uint64_t), true);
            VertexNormReader.Setup((int8_t*)MeshDataBuffer.get() + Submesh.VertexNormalsPtr, Submesh.VertexCount * sizeof(uint32_t), true);
            VertexUVReader.Setup((int8_t*)MeshDataBuffer.get() + Submesh.VertexUVsPtr, Submesh.VertexCount * sizeof(uint32_t), true);
            FaceIndiciesReader.Setup((int8_t*)MeshDataBuffer.get() + Submesh.FacesPtr, Submesh.FaceCount * sizeof(uint16_t) * 3, true);

            for (uint32_t i = 0; i < Submesh.VertexCount; i++)
            {
                auto& Vertex = Mesh.AddVertex();

                auto VertexPosition = VertexPosReader.Read<uint64_t>();

                Vertex.Position = Vector3(
                    (((((VertexPosition >> 00) & 0x1FFFFF) * ScaleConstant) - 1.0f) * Submesh.Scale) + Submesh.XOffset,
                    (((((VertexPosition >> 21) & 0x1FFFFF) * ScaleConstant) - 1.0f) * Submesh.Scale) + Submesh.YOffset,
                    (((((VertexPosition >> 42) & 0x1FFFFF) * ScaleConstant) - 1.0f) * Submesh.Scale) + Submesh.ZOffset);

                auto QTangent = VertexNormReader.Read<uint32_t>();

                Vertex.Normal = VanguardUnpackNormalQuat(QTangent);

                // Some models don't store colors: the pointer is checked below
                Vertex.Color[0] = 255;
                Vertex.Color[1] = 255;
                Vertex.Color[2] = 255;
                Vertex.Color[3] = 255;

                auto UVU = HalfFloats::ToFloat(VertexUVReader.Read<uint16_t>());
                auto UVV = HalfFloats::ToFloat(VertexUVReader.Read<uint16_t>());

                Vertex.AddUVLayer(UVU, UVV);

                auto& WeightValue = VertexWeights[i];

                for (uint32_t w = 0; w < WeightValue.WeightCount; w++)
                {
                    Vertex.AddVertexWeight(WeightValue.BoneValues[w], WeightValue.WeightValues[w]);
                }
            }

            if (ExportColors && Submesh.VertexColorPtr != 0xFFFFFFFF)
            {
                VertexColorReader.Setup((int8_t*)MeshDataBuffer.get() + Submesh.VertexColorPtr, Submesh.VertexCount * sizeof(uint32_t), true);

                for (uint32_t i = 0; i < Submesh.VertexCount; i++)
                {
                    // Some models don't store colors: the pointer is checked below
                    Mesh.Verticies[i].Color[0] = VertexColorReader.Read<uint8_t>();
                    Mesh.Verticies[i].Color[1] = VertexColorReader.Read<uint8_t>();
                    Mesh.Verticies[i].Color[2] = VertexColorReader.Read<uint8_t>();
                    Mesh.Verticies[i].Color[3] = VertexColorReader.Read<uint8_t>();
                }
            }

            for (uint32_t i = 0; i < Submesh.FaceCount; i++)
            {
                auto Face = FaceIndiciesReader.Read<VGGfxStreamFace>();

                Mesh.AddFace(Face.Index1, Face.Index2, Face.Index3);
            }

            // cs_blendshapes_no_tangent_frame.hlsl/cs_blendshapes_unified_main
            if (Submesh.BlendShapesPtr != 0)
            {
                uintptr_t ResultSize = 0;
                auto MeshBlendShapeInfo = CoDAssets::GameInstance->Read<MeshBlendShapesData>(Submesh.BlendShapesPtr);
                auto BlendShapeReader   = MemoryReader(CoDAssets::GameInstance->Read(MeshBlendShapeInfo.VertexShapes, MeshBlendShapeInfo.VertexShapesDataSize, ResultSize), MeshBlendShapeInfo.VertexShapesDataSize);
                auto NumVerts           = (size_t)MeshBlendShapeInfo.NumVerts;
                auto NumVertsExtended   = (size_t)MeshBlendShapeInfo.NumVertexExtended;
                auto NumVertsTotal      = NumVerts + NumVertsExtended;

                // Not sure if correct, I've never seen a model contain shape verts with extended data AND just positions
                for (size_t i = 0; i < NumVertsTotal; i++)
                {
                    BlendShapeReader.SetPosition(i * 8);

                    auto VertexIndex = BlendShapeReader.Read<uint16_t>();
                    auto ShapeCount  = BlendShapeReader.Read<uint16_t>();
                    auto DataOffset  = BlendShapeReader.Read<uint32_t>();

                    BlendShapeReader.SetPosition((uint64_t)DataOffset * (i < NumVertsExtended ? 16 : 8));

                    for (size_t j = 0; j < ShapeCount; j++)
                    {
                        auto VertexShapeIndex = BlendShapeReader.Read<uint16_t>();
                        auto X = HalfFloats::ToFloat(BlendShapeReader.Read<uint16_t>());
                        auto Y = HalfFloats::ToFloat(BlendShapeReader.Read<uint16_t>());
                        auto Z = HalfFloats::ToFloat(BlendShapeReader.Read<uint16_t>());

                        if (i < NumVertsExtended)
                            BlendShapeReader.Advance(8);

                        Mesh.Verticies[VertexIndex].BlendShapeDeltas.push_back(std::make_pair(
                            (uint32_t)ShapeIndices[VertexShapeIndex],
                            Vector3(
                                Z,
                                Y,
                                X
                            )));
                    }
                }
            }
        }
    }
}

std::string GameVanguard::LoadStringEntry(uint64_t Index)
{
    return CoDAssets::GameInstance->ReadNullTerminatedString(ps::state->StringsAddress + Index);
}
void GameVanguard::PerformInitialSetup()
{
    std::error_code Ec;
    StringCache.LoadIndex((std::filesystem::path(FileSystems::GetApplicationPath()) / "package_index\\fnv1a_string.wni").string());
    AssetNameCache.LoadIndex((std::filesystem::path(FileSystems::GetApplicationPath()) / "package_index\\fnv1a_xsounds_unverified.wni").string());
    auto OurPath = (std::filesystem::path(FileSystems::GetApplicationPath()) / "oo2core_8_win64.dll").string();
    if (!std::filesystem::is_regular_file(OurPath, Ec))
        std::filesystem::copy_file((std::filesystem::path(ps::state->GameDirectory) / "oo2core_8_win64.dll").string(), OurPath, std::filesystem::copy_options::overwrite_existing, Ec);
}
