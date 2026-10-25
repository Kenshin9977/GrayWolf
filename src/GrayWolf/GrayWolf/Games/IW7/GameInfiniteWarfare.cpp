#include "stdafx.h"

#include "GameInfiniteWarfare.h"

#include "GameReaders.h"

std::unique_ptr<XAnim_t> GameInfiniteWarfare::ReadXAnim(const CoDAnim_t* Animation)
{
    return GameReaders::ReadAnim<IWXAnim, IWXAnimDeltaParts>(Animation, 4, { "viewmodel_", "vm_" });
}

std::unique_ptr<XModel_t> GameInfiniteWarfare::ReadXModel(const CoDModel_t* Model)
{
    return GameReaders::ReadModel<IWXModel, IWXModelSurface, uint64_t>(Model, 4, ReadXMaterial);
}

thread_local GameReaders::ImagesElsewhereFn GameInfiniteWarfare::ImagesElsewhere;
thread_local std::function<bool(const std::string&, std::string&, int&)> GameInfiniteWarfare::DetailsElsewhere;

const XMaterial_t GameInfiniteWarfare::ReadXMaterial(uint64_t MaterialPointer)
{
    auto MaterialData = CoDAssets::GameInstance->Read<IWXMaterial>(MaterialPointer);
    const auto RawName = CoDAssets::GameInstance->ReadNullTerminatedString(MaterialData.NamePtr);

    XMaterial_t Result(MaterialData.ImageCount);
    Result.MaterialName = FileSystems::GetFileNameWithoutExtension(RawName);
    // its sort key and its techset's name (the techset pointer at 0x38, its name first): the material
    // adapter's alpha mode
    Result.SortKey = MaterialData.SortKey;
    if (const auto Techset = CoDAssets::GameInstance->Read<uint64_t>(MaterialPointer + 0x38))
    {
        Result.TechsetName = CoDAssets::GameInstance->ReadNullTerminatedString(CoDAssets::GameInstance->Read<uint64_t>(Techset));
        if (!Result.TechsetName.empty() && Result.TechsetName[0] == ',') Result.TechsetName.erase(0, 1);
    }

    const auto UsageOf = [](uint32_t Semantic) { return Semantic == 0xA0AB1041 ? ImageUsageType::DiffuseMap : ImageUsageType::Unknown; };
    // Image name pointer: end of image - 8
    GameReaders::AddMaterialImages<IWXMaterialImage>(Result, MaterialData.ImageTablePtr, MaterialData.ImageCount,
                                                     [](uint64_t Image) { return GameReaders::ImageNameAt<uint64_t>(Image, sizeof(IWGfxImage) - 8); }, UsageOf);

    if (MaterialData.ImageCount == 0 && !RawName.empty() && RawName[0] == ',')
    {
        GameReaders::AddImagesElsewhere(Result, ImagesElsewhere, RawName.substr(1), UsageOf);
        // a header's own sort key and techset are not the material's (0 and none on mp_weapon_view_iw7_m1_tr's 62)
        Result.SortKey = -1;
        Result.TechsetName.clear();
        if (DetailsElsewhere) DetailsElsewhere(RawName.substr(1), Result.TechsetName, Result.SortKey);
    }

    return Result;
}
