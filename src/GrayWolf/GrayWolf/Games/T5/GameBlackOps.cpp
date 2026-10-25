#include "stdafx.h"

#include "GameBlackOps.h"

#include "GameReaders.h"

void GameBlackOps::AddBlendMode(XMaterial_t& Material, uint64_t StateBits, uint32_t Count)
{
    if (Count == 0 || StateBits == 0) return;
    const auto Bits = CoDAssets::GameInstance->Read<BOXStateBits>(StateBits);
    uint32_t BlendBits[2] = { Bits.LoadBits0 & 0xF, (Bits.LoadBits0 >> 4) & 0xF };
    Material.Settings.emplace_back("blendMode", "uint", BlendBits, 2);
}

// These read the zone the disk path serves as game memory (Games/T5/DiskGame.cpp)
std::unique_ptr<XAnim_t> GameBlackOps::ReadXAnim(const CoDAnim_t* Animation)
{
    return GameReaders::ReadAnim<BOXAnim, BOXAnimDeltaParts>(Animation, 2, { "viewmodel_", "t5_viewmodel_" });
}

std::unique_ptr<XModel_t> GameBlackOps::ReadXModel(const CoDModel_t* Model)
{
    return GameReaders::ReadModel<BOXModel, BOXModelSurface, uint32_t>(Model, 2, ReadXMaterial,
                                                                      [](XModel_t& Asset, const BOXModel& Data) { Asset.BoneInfoPtr = Data.BoneInfoPtr; });
}

const XMaterial_t GameBlackOps::ReadXMaterial(uint64_t MaterialPointer)
{
    auto MaterialData = CoDAssets::GameInstance->Read<BOXMaterial>(MaterialPointer);

    XMaterial_t Result(MaterialData.ImageCount);
    Result.MaterialName = FileSystems::GetFileNameWithoutExtension(CoDAssets::GameInstance->ReadNullTerminatedString(MaterialData.NamePtr));

    const auto UsageOf = [](uint32_t Semantic)
    {
        switch (Semantic)
        {
        case 0x25D709F9: // Detail Materials
        case 0x48FF11AE: // Face Materials
            return ImageUsageType::DiffuseMap;
        case 0x942CBFF0:
            return ImageUsageType::NormalMap;
        default:
            return GameReaders::SemanticUsage(Semantic);
        }
    };
    // Image name pointer: end of image - 8
    GameReaders::AddMaterialImages<BOXMaterialImage>(Result, MaterialData.ImageTablePtr, MaterialData.ImageCount,
                                                     [](uint64_t Image) { return GameReaders::ImageNameAt<uint32_t>(Image, sizeof(BOGfxImage) - 8); }, UsageOf);

    // The blend mode (src/dst RGB blend factors) as a material setting, exported to
    // "<name>_settings.txt"
    AddBlendMode(Result, MaterialData.StateBitsTablePtr, MaterialData.StateBitsCount);
    return Result;
}

std::unique_ptr<XImageDDS> GameBlackOps::LoadXImage(const XImage_t& Image)
{
    return GameReaders::LoadIwdImage(Image);
}
