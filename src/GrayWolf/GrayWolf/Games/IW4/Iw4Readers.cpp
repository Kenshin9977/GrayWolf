#include "stdafx.h"

#include "Games/IW4/Iw4Readers.h"

#include "CoDAssets.h"
#include "ExportWriters.h"
#include "FileSystems.h"

#include <algorithm>

namespace Iw4Family
{
    namespace
    {
        constexpr int64_t MaxLods = 4;
        constexpr int64_t LodDist = 0x0, LodSurfCount = 0x4, LodSurfIndex = 0x6, LodModelSurfs = 0x8;
        constexpr int64_t TextureHash = 0x0, TextureSemantic = 0x7, TextureImage = 0x8;
        constexpr uint8_t SemanticWater = 11;   // MaterialTextureDef: a water_t, not an image

        template <class T>
        T Get(uint64_t Address) { return CoDAssets::GameInstance->Read<T>(Address); }

        uint64_t Ptr(uint64_t Address, const Layout& L)
        {
            return L.PointerSize == 4 ? (uint64_t)Get<uint32_t>(Address) : Get<uint64_t>(Address);
        }
    }

    std::unique_ptr<XModel_t> ReadXModel(const CoDModel_t* Model, const Layout& L)
    {
        const uint64_t P = Model->AssetPointer;
        const uint8_t LodCount = (uint8_t)std::min<int64_t>(Get<uint8_t>(P + L.ModelLodCount), MaxLods);
        auto Out = std::make_unique<XModel_t>(LodCount);
        Out->ModelName = Model->AssetName;
        Out->BoneCount = Get<uint8_t>(P + L.ModelBones);
        Out->RootBoneCount = Get<uint8_t>(P + L.ModelRootBones);
        Out->BoneRotationData = BoneDataTypes::DivideBySize;
        Out->BoneIDsPtr = Ptr(P + L.ModelBoneNames, L);
        Out->BoneIndexSize = 2;
        Out->BoneParentsPtr = Ptr(P + L.ModelParents, L);
        Out->BoneParentSize = 1;
        Out->RotationsPtr = Ptr(P + L.ModelQuats, L);
        Out->TranslationsPtr = Ptr(P + L.ModelTrans, L);
        Out->BaseMatriciesPtr = Ptr(P + L.ModelBaseMat, L);
        const uint64_t Materials = Ptr(P + L.ModelMaterials, L);
        for (uint8_t Lod = 0; Lod < LodCount; Lod++)
        {
            const uint64_t Info = P + L.ModelLods + Lod * L.LodSize;
            const uint16_t SurfCount = Get<uint16_t>(Info + LodSurfCount);
            const uint16_t SurfIndex = Get<uint16_t>(Info + LodSurfIndex);
            uint64_t ModelSurfs = Ptr(Info + LodModelSurfs, L);
            if (L.StreamedSurfs && ModelSurfs && Ptr(ModelSurfs + L.ModelSurfsSurfs, L) == 0)
                ModelSurfs = L.StreamedSurfs(CoDAssets::GameInstance->ReadNullTerminatedString(Ptr(ModelSurfs, L)));
            Out->ModelLods.emplace_back(SurfCount);
            auto& Ref = Out->ModelLods.back();
            Ref.LodDistance = Get<float>(Info + LodDist);
            const uint64_t Surfs = ModelSurfs ? Ptr(ModelSurfs + L.ModelSurfsSurfs, L) : 0;
            for (uint16_t S = 0; Surfs && S < SurfCount; S++)
            {
                const uint64_t Surf = Surfs + S * L.SurfSize;
                Ref.Submeshes.emplace_back();
                auto& Sub = Ref.Submeshes.back();
                Sub.VertListcount = Get<uint32_t>(Surf + L.SurfVertListCount);
                Sub.RigidWeightsPtr = Ptr(Surf + L.SurfVertList, L);
                Sub.VertexCount = Get<uint16_t>(Surf + L.SurfVerts);
                Sub.FaceCount = Get<uint16_t>(Surf + L.SurfTris);
                Sub.VertexPtr = Ptr(Surf + L.SurfVerts0, L);
                Sub.FacesPtr = Ptr(Surf + L.SurfTriIndices, L);
                for (int K = 0; K < 4; K++) Sub.WeightCounts[K] = (uint16_t)Get<int16_t>(Surf + L.SurfBlendCounts + K * 2);
                Sub.WeightsPtr = Ptr(Surf + L.SurfBlend, L);
                const uint64_t Material = Materials ? Ptr(Materials + (uint64_t)(SurfIndex + S) * L.PointerSize, L) : 0;
                Ref.Materials.emplace_back(Material ? ReadXMaterial(Material, L) : XMaterial_t(0));
            }
        }
        return Out;
    }

    std::unique_ptr<XAnim_t> ReadXAnim(const CoDAnim_t* Animation, const Layout& L)
    {
        const uint64_t P = Animation->AssetPointer;
        auto Anim = std::make_unique<XAnim_t>();
        Anim->AnimationName = Animation->AssetName;
        Anim->FrameCount = Get<uint16_t>(P + L.AnimFrames);
        Anim->FrameRate = Get<float>(P + L.AnimRate);
        Anim->ViewModelAnimation = _strnicmp(Animation->AssetName.c_str(), "viewmodel_", 10) == 0;
        Anim->LoopingAnimation = (Get<uint8_t>(P + L.AnimFlags) & L.LoopMask) != 0;
        // boneCount[PART_TYPE_COUNT] then notifyCount
        uint8_t Counts[11];
        for (int K = 0; K < 11; K++) Counts[K] = Get<uint8_t>(P + L.AnimCounts + K);
        Anim->NoneRotatedBoneCount = Counts[0];
        Anim->TwoDRotatedBoneCount = Counts[1];
        Anim->NormalRotatedBoneCount = Counts[2];
        Anim->TwoDStaticRotatedBoneCount = Counts[3];
        Anim->NormalStaticRotatedBoneCount = Counts[4];
        Anim->NormalTranslatedBoneCount = Counts[5];
        Anim->PreciseTranslatedBoneCount = Counts[6];
        Anim->StaticTranslatedBoneCount = Counts[7];
        Anim->NoneTranslatedBoneCount = Counts[8];
        Anim->TotalBoneCount = Counts[9];
        Anim->NotificationCount = Counts[10];
        const auto Pointer = [&](int K) { return Ptr(P + L.AnimPointers + K * L.PointerSize, L); };
        Anim->BoneIDsPtr = Pointer(0);
        Anim->BoneIndexSize = 2;
        Anim->DataBytesPtr = Pointer(1);
        Anim->DataShortsPtr = Pointer(2);
        Anim->DataIntsPtr = Pointer(3);
        Anim->RandomDataShortsPtr = Pointer(4);
        Anim->RandomDataBytesPtr = Pointer(5);
        Anim->RandomDataIntsPtr = Pointer(6);
        Anim->LongIndiciesPtr = Pointer(7);
        Anim->NotificationsPtr = Pointer(8);
        if (const uint64_t Delta = Ptr(P + L.AnimDelta, L))   // {trans, quat2, quat}
        {
            Anim->DeltaTranslationPtr = Ptr(Delta, L);
            Anim->Delta2DRotationsPtr = Ptr(Delta + L.PointerSize, L);
            Anim->Delta3DRotationsPtr = Ptr(Delta + 2 * L.PointerSize, L);
        }
        Anim->RotationType = AnimationKeyTypes::DivideBySize;
        Anim->TranslationType = AnimationKeyTypes::MinSizeTable;
        Anim->SupportsInlineIndicies = true;
        return Anim;
    }

    XMaterial_t ReadXMaterial(uint64_t MaterialPointer, const Layout& L)
    {
        const uint8_t Count = Get<uint8_t>(MaterialPointer + L.MaterialTextureCount);
        const uint64_t Table = Ptr(MaterialPointer + L.MaterialTextureTable, L);
        const std::string RawName = CoDAssets::GameInstance->ReadNullTerminatedString(Ptr(MaterialPointer + L.MaterialName, L));
        // a name-only header (a model's handle to a material the zone, or another, defines whole):
        // the game swaps it for that material by name once loaded
        const bool Header = Table == 0 && RawName.size() > 1 && RawName[0] == ',';
        if (Header && L.MaterialByName)
            if (const uint64_t Whole = L.MaterialByName(RawName.substr(1)); Whole != 0 && Whole != MaterialPointer)
                return ReadXMaterial(Whole, L);
        const auto UsageOf = [](uint32_t Hash) {
            switch (Hash)
            {
            case 0xA0AB1041: return ImageUsageType::DiffuseMap;    // colorMap
            case 0x59D30D0F: return ImageUsageType::NormalMap;     // normalMap
            case 0x34ECCCB3: return ImageUsageType::SpecularMap;   // specularMap
            default: return ImageUsageType::Unknown;
            }
        };
        XMaterial_t Result(Count);
        Result.MaterialName = FileSystems::GetFileNameWithoutExtension(RawName);
        for (uint8_t M = 0; Table && M < Count; M++)
        {
            const uint64_t Def = Table + M * L.TextureDefSize;
            if (Get<uint8_t>(Def + TextureSemantic) == SemanticWater) continue;
            const uint32_t Hash = Get<uint32_t>(Def + TextureHash);
            const uint64_t Image = Ptr(Def + TextureImage, L);
            if (Image == 0) continue;
            const std::string Name = CoDAssets::GameInstance->ReadNullTerminatedString(Ptr(Image + L.ImageName, L));
            Result.Images.emplace_back(UsageOf(Hash), Hash, Image, Name);
        }
        // a material another zone defines: its images by name, that zone's
        std::vector<std::pair<uint32_t, std::string>> Elsewhere;
        if (Header && L.MaterialImagesByName && L.MaterialImagesByName(RawName.substr(1), Elsewhere))
            for (const auto& [Hash, Name] : Elsewhere)
                Result.Images.emplace_back(UsageOf(Hash), Hash, ExportWriters::ImageElsewhere, Name);
        return Result;
    }
}
