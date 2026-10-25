#include "stdafx.h"

#include "GameBlackOps2.h"
#include "Games/T5/GameBlackOps.h"

#include "CoDAssets.h"
#include "GameReaders.h"
#include "CoDIWITranslator.h"

#include "FileSystems.h"
#include "SettingsManager.h"

// These read the zone the disk path serves as game memory (Games/T6/DiskGame.cpp)
std::unique_ptr<XAnim_t> GameBlackOps2::ReadXAnim(const CoDAnim_t* Animation)
{
    return GameReaders::ReadAnim<BO2XAnim, BO2XAnimDeltaParts>(Animation, 2, { "viewmodel_" });
}

std::unique_ptr<XModel_t> GameBlackOps2::ReadXModel(const CoDModel_t* Model)
{
    if (!CoDAssets::GameInstance->IsRunning()) return nullptr;

    const auto ModelData = CoDAssets::GameInstance->Read<BO2XModel>(Model->AssetPointer);
    auto ModelAsset = std::make_unique<XModel_t>(ModelData.NumLods);
    GameReaders::CopyModelBones(*ModelAsset, Model->AssetName, ModelData, 2, 1);
    ModelAsset->BoneRotationData = BoneDataTypes::DivideBySize;

    for (uint32_t i = 0; i < ModelData.NumLods; i++)
    {
        const auto& Lod = ModelData.ModelLods[i];
        ModelAsset->ModelLods.emplace_back(Lod.NumSurfs);
        ModelAsset->ModelLods[i].LodDistance = Lod.LodDistance;

        // a LOD's surfaces and material handles start at its SurfacesIndex in the model's arrays
        uint64_t Handles = ModelData.MaterialHandlesPtr + (Lod.SurfacesIndex * sizeof(uint32_t));
        GameReaders::ReadSurfaces<BO2XModelSurface, uint32_t>(ModelAsset->ModelLods[i], ModelData.SurfacesPtr + (Lod.SurfacesIndex * sizeof(BO2XModelSurface)),
                                                             Lod.NumSurfs, Handles, ReadXMaterial,
                                                             [](XModelSubmesh_t& Submesh, const BO2XModelSurface& Info)
                                                             {
                                                                 GameReaders::CopySurface(Submesh, Info);
                                                                 // vertListCount is unsigned (an fxanim surface has more than 127 rigid bone lists)
                                                                 Submesh.VertListcount = (uint8_t)Info.VertListCount;
                                                             });
    }
    return ModelAsset;
}

const XMaterial_t GameBlackOps2::ReadXMaterial(uint64_t MaterialPointer)
{
    auto MaterialData = CoDAssets::GameInstance->Read<BO2XMaterial>(MaterialPointer);

    XMaterial_t Result(MaterialData.ImageCount);
    Result.MaterialName = FileSystems::GetFileNameWithoutExtension(CoDAssets::GameInstance->ReadNullTerminatedString(MaterialData.NamePtr));

    const auto UsageOf = [](uint32_t Semantic)
    {
        switch (Semantic)
        {
        case 0xF039EC2D: return ImageUsageType::DiffuseMap;
        case 0x942CBFF0: return ImageUsageType::NormalMap;
        case 0x8C297E80: return ImageUsageType::SpecularMap;
        default: return GameReaders::SemanticUsage(Semantic);
        }
    };
    // GfxImage's name is 8 bytes before its end
    GameReaders::AddMaterialImages<BO2XMaterialImage>(Result, MaterialData.ImageTablePtr, MaterialData.ImageCount,
                                                      [](uint64_t Image) { return GameReaders::ImageNameAt<uint32_t>(Image, sizeof(BO2GfxImage) - 8); }, UsageOf);

    GameBlackOps::AddBlendMode(Result, MaterialData.StateBitsTablePtr, MaterialData.StateBitsCount);
    return Result;
}

std::unique_ptr<XImageDDS> GameBlackOps2::LoadXImage(const XImage_t& Image)
{
    auto ImageInfo = CoDAssets::GameInstance->Read<BO2GfxImage>(Image.ImagePtr);
    if (ImageInfo.Streamed <= 0 || !CoDAssets::GamePackageCache) return nullptr;

    // the .ipak entry's key: the two halves of the image's 64-bit hash
    uint32_t ResultSize = 0;
    auto ImageData = CoDAssets::GamePackageCache->ExtractPackageObject((((uint64_t)ImageInfo.KeyUpper) << 32 | ImageInfo.KeyLower), ResultSize);
    if (ImageData == nullptr) return nullptr;

    auto Result = CoDIWITranslator::TranslateIWI(ImageData, ResultSize);
    if (Result != nullptr && Image.ImageUsage == ImageUsageType::NormalMap && (SettingsManager::GetSetting("patchnormals", "true") == "true"))
    {
        Result->ImagePatchType = ImagePatch::Normal_Expand;
    }
    return Result;
}
