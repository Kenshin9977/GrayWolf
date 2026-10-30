#include "stdafx.h"

#include "CoDXAnimTranslator.h"

#include "HalfFloats.h"
#include "Strings.h"

#include "DBGameGenerics.h"

#include "ICoDGame.h"

#include <format>

namespace
{
    // A delta key's frame index: FrameSize bytes at Cursor
    uint32_t ReadFrameIndex(uint64_t& Cursor, uint32_t FrameSize)
    {
        uint32_t FrameIndex = 0;
        if (FrameSize == 1)
        {
            FrameIndex = CoDAssets::GameInstance->Read<uint8_t>(Cursor);
            Cursor += 1;
        }
        else if (FrameSize == 2)
        {
            FrameIndex = CoDAssets::GameInstance->Read<uint16_t>(Cursor);
            Cursor += 2;
        }
        return FrameIndex;
    }

    // A root rotation key as the game packs it: two (2D) or four (3D) shorts
    Quaternion RotationKey(const Quat2Data& Data, AnimationKeyTypes Type)
    {
        switch (Type)
        {
        case AnimationKeyTypes::DivideBySize: return Quaternion(0, 0, ((float)Data.RotationZ / 32768.f), ((float)Data.RotationW / 32768.f));
        case AnimationKeyTypes::HalfFloat: return Quaternion(0, 0, HalfFloats::ToFloat((uint16_t)Data.RotationZ), HalfFloats::ToFloat((uint16_t)Data.RotationW));
        default: return VectorPacking::QuatPacking2DA(*(uint32_t*)&Data);
        }
    }

    Quaternion RotationKey(const QuatData& Data, AnimationKeyTypes Type)
    {
        switch (Type)
        {
        case AnimationKeyTypes::DivideBySize:
            return Quaternion(((float)Data.RotationX / 32768.f), ((float)Data.RotationY / 32768.f), ((float)Data.RotationZ / 32768.f), ((float)Data.RotationW / 32768.f));
        case AnimationKeyTypes::HalfFloat:
            return Quaternion(HalfFloats::ToFloat((uint16_t)Data.RotationX), HalfFloats::ToFloat((uint16_t)Data.RotationY), HalfFloats::ToFloat((uint16_t)Data.RotationZ),
                              HalfFloats::ToFloat((uint16_t)Data.RotationW));
        default: return VectorPacking::QuatPackingA(*(uint64_t*)&Data);
        }
    }

    bool IsRotationKeyType(AnimationKeyTypes Type)
    {
        return Type == AnimationKeyTypes::DivideBySize || Type == AnimationKeyTypes::HalfFloat || Type == AnimationKeyTypes::QuatPackingA;
    }

    // The frame index of a bone's key: a byte, else a short from the data shorts, or from the long
    // indices for a bone with 0x40 keys or more
    uint32_t ReadKeyFrame(const std::unique_ptr<XAnim_t>& Animation, uint32_t FrameSize, uint32_t FrameCount)
    {
        if (FrameSize == 1)
            return ReadFrameIndex(Animation->DataBytesPtr, 1);
        if (FrameSize == 2)
        {
            if (FrameCount < 0x40 || Animation->LongIndiciesPtr == 0)
                return ReadFrameIndex(Animation->DataShortsPtr, 2);
            return ReadFrameIndex(Animation->LongIndiciesPtr, 2);
        }
        return 0;
    }

    // A bone's key count (frames - 1), past the inline frame indices that may follow it (short
    // frames, 0x40 keys or more), which end at the animation's frame count
    uint32_t ReadKeyCount(const std::unique_ptr<XAnim_t>& Animation, uint32_t FrameSize)
    {
        uint32_t FrameCount = CoDAssets::GameInstance->Read<uint16_t>(Animation->DataShortsPtr);
        Animation->DataShortsPtr += 2;

        if (FrameSize == 2 && Animation->SupportsInlineIndicies && FrameCount >= 0x40)
        {
            uint16_t InlineIndex = 0;
            do
            {
                InlineIndex = CoDAssets::GameInstance->Read<uint16_t>(Animation->DataShortsPtr);
                Animation->DataShortsPtr += 2;
            } while (InlineIndex != Animation->FrameCount);
        }
        return FrameCount;
    }

    // A translated bone's index: a byte or a short (BoneTypeSize)
    uint32_t ReadBoneIndex(const std::unique_ptr<XAnim_t>& Animation, uint32_t BoneTypeSize)
    {
        if (BoneTypeSize == 1)
            return ReadFrameIndex(Animation->DataBytesPtr, 1);
        if (BoneTypeSize == 2)
            return ReadFrameIndex(Animation->DataShortsPtr, 2);
        return 0;
    }

    void AddRotation(const std::unique_ptr<WraithAnim>& Anim, const std::string& Tag, uint32_t Frame, const Quaternion& Rotation)
    {
        Anim->AddRotationKey(Tag, Frame, Rotation.X, Rotation.Y, Rotation.Z, Rotation.W);
    }

    // One bone's animated rotations: its key count, then FrameCount + 1 TKey (Quat2Data or
    // QuatData) in the random shorts, each with its frame index
    template <typename TKey>
    void RotationKeys(const std::unique_ptr<WraithAnim>& Anim, const std::unique_ptr<XAnim_t>& Animation, const std::string& Tag, uint32_t FrameSize)
    {
        const uint32_t FrameCount = ReadKeyCount(Animation, FrameSize);

        uintptr_t ResultSize = 0;
        const uintptr_t DataSize = (FrameCount + 1) * sizeof(TKey);
        auto KeyData = CoDAssets::GameInstance->Read(Animation->RandomDataShortsPtr, DataSize, ResultSize);
        Animation->RandomDataShortsPtr += DataSize;

        if (ResultSize == DataSize && KeyData != nullptr)
        {
            if (IsRotationKeyType(Animation->RotationType))
            {
                auto& Frames = Anim->AnimationRotationKeys[Tag];
                Frames.reserve(Frames.size() + FrameCount + 1);
            }
            const auto Keys = (const TKey*)KeyData;
            for (uint32_t f = 0; f < (FrameCount + 1); f++)
            {
                const uint32_t FrameIndex = ReadKeyFrame(Animation, FrameSize, FrameCount);
                if (IsRotationKeyType(Animation->RotationType))
                    AddRotation(Anim, Tag, FrameIndex, RotationKey(Keys[f], Animation->RotationType));
            }
        }

        delete[] KeyData;
    }

    // One bone's static rotation: a TKey in the data shorts
    template <typename TKey>
    void StaticRotation(const std::unique_ptr<WraithAnim>& Anim, const std::unique_ptr<XAnim_t>& Animation, const std::string& Tag)
    {
        const auto RotationData = CoDAssets::GameInstance->Read<TKey>(Animation->DataShortsPtr);
        Animation->DataShortsPtr += sizeof(TKey);

        if (IsRotationKeyType(Animation->RotationType))
            AddRotation(Anim, Tag, 0, RotationKey(RotationData, Animation->RotationType));
    }

    // One bone's animated translations: its index, key count, min and size vectors (data ints),
    // then FrameCount + 1 TComponent triples at Keys (bytes: normal, shorts: precise)
    template <typename TComponent>
    void TranslationKeys(const std::unique_ptr<WraithAnim>& Anim, const std::unique_ptr<XAnim_t>& Animation, const std::vector<std::string>& TagNames, uint32_t FrameSize,
                         uint32_t BoneTypeSize, uint64_t& Keys)
    {
        const uint32_t BoneID = ReadBoneIndex(Animation, BoneTypeSize);
        const uint32_t FrameCount = ReadKeyCount(Animation, FrameSize);

        auto MinVec = CoDAssets::GameInstance->Read<Vector3>(Animation->DataIntsPtr);
        Animation->DataIntsPtr += 12;
        auto SizeVec = CoDAssets::GameInstance->Read<Vector3>(Animation->DataIntsPtr);
        Animation->DataIntsPtr += 12;

        uintptr_t ResultSize = 0;
        const uintptr_t DataSize = (FrameCount + 1) * 3 * sizeof(TComponent);
        auto KeyData = CoDAssets::GameInstance->Read(Keys, DataSize, ResultSize);
        Keys += DataSize;

        if (ResultSize == DataSize && KeyData != nullptr)
        {
            if (Animation->TranslationType == AnimationKeyTypes::MinSizeTable)
            {
                auto& Frames = Anim->AnimationPositionKeys[TagNames[BoneID]];
                Frames.reserve(Frames.size() + FrameCount + 1);
            }
            const auto Components = (const TComponent*)KeyData;
            for (uint32_t f = 0; f < (FrameCount + 1); f++)
            {
                const uint32_t FrameIndex = ReadKeyFrame(Animation, FrameSize, FrameCount);

                if (Animation->TranslationType == AnimationKeyTypes::MinSizeTable)
                {
                    float TranslationX = (SizeVec.X * (float)Components[(f * 3)]) + MinVec.X;
                    float TranslationY = (SizeVec.Y * (float)Components[(f * 3) + 1]) + MinVec.Y;
                    float TranslationZ = (SizeVec.Z * (float)Components[(f * 3) + 2]) + MinVec.Z;
                    Anim->AddTranslationKey(TagNames[BoneID], FrameIndex, TranslationX, TranslationY, TranslationZ);
                }
            }
        }

        delete[] KeyData;
    }
}

std::unique_ptr<WraithAnim> CoDXAnimTranslator::TranslateXAnim(const std::unique_ptr<XAnim_t>& Animation)
{
    auto Anim = std::make_unique<WraithAnim>();

    Anim->AssetName = Animation->AnimationName;
    Anim->FrameRate = Animation->FrameRate;
    Anim->Looping = Animation->LoopingAnimation;

    // Workaround: ViewModel local-space transform matrices come through zeroed (unusable in
    // Maya etc.), so they're derived on export from global-space instead - which means the
    // anim data has to be marked absolute and the hierarchy past the base gun joint relative.

    Anim->AnimType = WraithAnimationType::Relative;
    if (Animation->ViewModelAnimation)
    {
        auto* Adapter = CoDAssets::Game;
        switch (Adapter ? Adapter->GetViewmodelBoneConvention() : ViewmodelBoneConvention::None)
        {
        case ViewmodelBoneConvention::Standard:
            Anim->AddBoneModifier("j_gun", WraithAnimationType::Relative);
            Anim->AddBoneModifier("j_gun1", WraithAnimationType::Relative);
            break;
        case ViewmodelBoneConvention::BO3BO4:
            Anim->AddBoneModifier("tag_weapon", WraithAnimationType::Relative);
            Anim->AddBoneModifier("tag_weapon_le", WraithAnimationType::Relative);
            break;
        case ViewmodelBoneConvention::InfiniteWarfare:
            Anim->AddBoneModifier("tag_weapon", WraithAnimationType::Relative);
            Anim->AddBoneModifier("tag_weapon1", WraithAnimationType::Relative);
            Anim->AddBoneModifier("j_gun", WraithAnimationType::Relative);
            Anim->AddBoneModifier("j_gun1", WraithAnimationType::Relative);
            break;
        default:
            break;
        }

        Anim->AnimType = WraithAnimationType::Absolute;
    }
    if (Animation->DeltaTranslationPtr != 0 || Animation->Delta2DRotationsPtr != 0 || Animation->Delta3DRotationsPtr != 0)
    {
        Anim->AnimType = WraithAnimationType::Delta;
        Anim->DeltaTagName = "tag_origin";
    }
    if (Animation->AdditiveAnimation)
    {
        // Set type to additive (Overrides absolute)
        Anim->AnimType = WraithAnimationType::Additive;
    }

    if (Animation->ReaderFunction != nullptr)
    {
        Animation->ReaderFunction(Animation, Anim);

        for (auto& notetrack : Animation->Reader->Notetracks)
        {
            Anim->AddNoteTrack(notetrack.first, (uint32_t)notetrack.second);
        }
    }
    else
    {
        uint32_t FrameSize = (Animation->FrameCount > 255) ? 2 : 1;
        uint32_t BoneTypeSize = (Animation->TotalBoneCount > 255) ? 2 : 1;

        // Override the bone_t size if need be for various games
        if (Animation->BoneTypeSize > 0) { BoneTypeSize = Animation->BoneTypeSize; }

        std::vector<std::string> TagNames;
        TagNames.reserve(Animation->TotalBoneCount);
        for (uint32_t i = 0; i < Animation->TotalBoneCount; i++)
        {
            uint64_t BoneID = 0;

            switch (Animation->BoneIndexSize)
            {
            case 2: BoneID = CoDAssets::GameInstance->Read<uint16_t>(Animation->BoneIDsPtr); break;
            case 4: BoneID = CoDAssets::GameInstance->Read<uint32_t>(Animation->BoneIDsPtr); break;
            }

            TagNames.emplace_back(CoDAssets::GameStringHandler(BoneID));
            Animation->BoneIDsPtr += Animation->BoneIndexSize;
        }

        // BO4 can't tell what's a vm anymore
#ifdef _DEBUG
        bool HasTagTorso = false;
        for (auto& tag : TagNames) { if (tag == "tag_torso") { HasTagTorso = true; break; } }
        if (HasTagTorso && Animation->ViewModelAnimation == false) { printf("Animation has tag_torso but ISN'T a viewmodel anim: %s\n", Animation->AnimationName.c_str()); }
#endif

        // Stage 0: zero-rotated bones (the first set) reset to identity; NoneTranslated
        // bones are not reset and stay at scene position.
        uint32_t Bone = 0;
        for (uint32_t i = 0; i < Animation->NoneRotatedBoneCount; i++, Bone++)
        {
            Anim->AddRotationKey(TagNames[Bone], 0, 0, 0, 0, 1.0f);
        }

        // Stages 1 to 4, one after the other: 2D, 3D, 2D static and 3D static rotations
        for (uint32_t i = 0; i < Animation->TwoDRotatedBoneCount; i++, Bone++)
            RotationKeys<Quat2Data>(Anim, Animation, TagNames[Bone], FrameSize);
        for (uint32_t i = 0; i < Animation->NormalRotatedBoneCount; i++, Bone++)
            RotationKeys<QuatData>(Anim, Animation, TagNames[Bone], FrameSize);
        for (uint32_t i = 0; i < Animation->TwoDStaticRotatedBoneCount; i++, Bone++)
            StaticRotation<Quat2Data>(Anim, Animation, TagNames[Bone]);
        for (uint32_t i = 0; i < Animation->NormalStaticRotatedBoneCount; i++, Bone++)
            StaticRotation<QuatData>(Anim, Animation, TagNames[Bone]);

        // Stages 5 and 6: normal (byte) then precise (short) translations
        for (uint32_t i = 0; i < Animation->NormalTranslatedBoneCount; i++)
            TranslationKeys<uint8_t>(Anim, Animation, TagNames, FrameSize, BoneTypeSize, Animation->RandomDataBytesPtr);
        for (uint32_t i = 0; i < Animation->PreciseTranslatedBoneCount; i++)
            TranslationKeys<uint16_t>(Anim, Animation, TagNames, FrameSize, BoneTypeSize, Animation->RandomDataShortsPtr);

        // Stage 7: Static Translations
        // Static Translations appear directly after the "Precise Translations"
        for (uint32_t i = 0; i < Animation->StaticTranslatedBoneCount; i++)
        {
            auto Coords = CoDAssets::GameInstance->Read<Vector3>(Animation->DataIntsPtr);
            Animation->DataIntsPtr += 12;

            const uint32_t BoneID = ReadBoneIndex(Animation, BoneTypeSize);
            Anim->AddTranslationKey(TagNames[BoneID], 0, Coords.X, Coords.Y, Coords.Z);
        }
    }

    // Stage 8: Delta translation data, handled on a per-game basis
    // Delta translations appear in their own pointer, but have some game specific structures
    if (Animation->DeltaTranslationPtr != 0)
    {
        auto* DeltaAdapter = CoDAssets::Game;
        switch (DeltaAdapter ? DeltaAdapter->GetDeltaBitness() : DeltaBitness::None)
        {
        case DeltaBitness::Bits32:
            DeltaTranslations32(Anim, (Animation->FrameCount > 255) ? 2 : 1, Animation);
            break;
        case DeltaBitness::Bits64:
            DeltaTranslations64(Anim, (Animation->FrameCount > 255) ? 2 : 1, Animation);
            break;
        default:
            break;
        }
    }

    // Stage 9: Delta 2D rotation data, handled on a per-game basis
    // Delta 2D rotations appear in their own pointer, but have some game specific structures
    if (Animation->Delta2DRotationsPtr != 0)
    {
        auto* Delta2DAdapter = CoDAssets::Game;
        switch (Delta2DAdapter ? Delta2DAdapter->GetDeltaBitness() : DeltaBitness::None)
        {
        case DeltaBitness::Bits32:
            Delta2DRotations32(Anim, (Animation->FrameCount > 255) ? 2 : 1, Animation);
            break;
        case DeltaBitness::Bits64:
            Delta2DRotations64(Anim, (Animation->FrameCount > 255) ? 2 : 1, Animation);
            break;
        default:
            break;
        }
    }

    // Stage 10: Delta 3D rotation data, handled on a per-game basis
    // Delta 3D rotations appear in their own pointer, but have some game specific structures
    if (Animation->Delta3DRotationsPtr != 0)
    {
        auto* Delta3DAdapter = CoDAssets::Game;
        switch (Delta3DAdapter ? Delta3DAdapter->GetDeltaBitness() : DeltaBitness::None)
        {
        case DeltaBitness::Bits32:
            Delta3DRotations32(Anim, (Animation->FrameCount > 255) ? 2 : 1, Animation);
            break;
        case DeltaBitness::Bits64:
            Delta3DRotations64(Anim, (Animation->FrameCount > 255) ? 2 : 1, Animation);
            break;
        default:
            break;
        }
    }

    // Stage 11: Notifications
    // Notifications appear in their own pointer, but have some game specific structures
    {
        auto* NotetrackAdapter = CoDAssets::Game;
        switch (NotetrackAdapter ? NotetrackAdapter->GetNotetrackFormat() : NotetrackFormat::None)
        {
        case NotetrackFormat::Standard:
            NotetracksStandard(Anim, Animation);
            break;
        case NotetrackFormat::BO3:
            NotetracksBO3(Anim, Animation);
            break;
        case NotetrackFormat::BO4:
            NotetracksBO4(Anim, Animation);
            break;
        case NotetrackFormat::CW:
            NotetracksCW(Anim, Animation);
            break;
        case NotetrackFormat::VG:
            NotetracksVG(Anim, Animation);
            break;
        default:
            break;
        }
    }

    // Stage 12: Blendshapes, this is only supported by some newer games
    {
        auto* BlendShapeAdapter = CoDAssets::Game;
        switch (BlendShapeAdapter ? BlendShapeAdapter->GetBlendShapeFormat() : BlendShapeFormat::None)
        {
        case BlendShapeFormat::BO4:
            BlendShapesBO4(Anim, Animation);
            break;
        case BlendShapeFormat::MW:
            BlendShapesMW(Anim, Animation);
            break;
        default:
            break;
        }
    }

    return Anim;
}


namespace
{
    // Notetracks of Stride bytes each: the name's string index at +0, the time (0-1) at +4
    void NamedNotetracks(const std::unique_ptr<WraithAnim>& Anim, const std::unique_ptr<XAnim_t>& Animation, uint32_t Stride)
    {
        for (uint32_t i = 0; i < Animation->NotificationCount; i++)
        {
            auto NotificationTag = CoDAssets::GameStringHandler(CoDAssets::GameInstance->Read<uint32_t>(Animation->NotificationsPtr));
            uint32_t NotificationFrame = (uint32_t)((float)Animation->FrameCount * CoDAssets::GameInstance->Read<float>(Animation->NotificationsPtr + 4));

            if (!Strings::IsNullOrWhiteSpace(NotificationTag))
            {
                Anim->AddNoteTrack(NotificationTag, NotificationFrame);
            }

            Animation->NotificationsPtr += Stride;
        }
    }

    // Notetracks of Stride bytes each with a 60-bit hash at +0 (BO4, CW): sounds and rumbles are
    // named by it, a vox by its tag, the others by the hash when there is one
    void HashedNotetracks(const std::unique_ptr<WraithAnim>& Anim, const std::unique_ptr<XAnim_t>& Animation, uint32_t TagAt, uint32_t TypeAt, uint32_t TimeAt,
                          uint32_t Stride)
    {
        for (uint32_t i = 0; i < Animation->NotificationCount; i++)
        {
            auto NotificaionHash = CoDAssets::GameInstance->Read<uint64_t>(Animation->NotificationsPtr) & 0xFFFFFFFFFFFFFFF;
            auto NotificationTag = CoDAssets::GameStringHandler(CoDAssets::GameInstance->Read<uint32_t>(Animation->NotificationsPtr + TagAt));
            auto NotificationType = CoDAssets::GameStringHandler(CoDAssets::GameInstance->Read<uint32_t>(Animation->NotificationsPtr + TypeAt));
            uint32_t NotificationFrame = (uint32_t)((float)Animation->FrameCount * CoDAssets::GameInstance->Read<float>(Animation->NotificationsPtr + TimeAt));

            // Append the removed prefixes to the notetrack
            if (NotificationType == "sound")
                NotificationTag = std::format("sndnt#{:x}", NotificaionHash);
            else if (NotificationType == "rumble")
                NotificationTag = std::format("rmbnt#{:x}", NotificaionHash);
            else if (NotificationType == "vox")
                NotificationTag = "vox#" + NotificationTag;
            else if (NotificaionHash != 0)
                NotificationTag = std::format("hash_{:x}", NotificaionHash);

            if (!Strings::IsNullOrWhiteSpace(NotificationTag))
            {
                Anim->AddNoteTrack(NotificationTag, NotificationFrame);
            }

            Animation->NotificationsPtr += Stride;
        }
    }
}

void CoDXAnimTranslator::NotetracksStandard(const std::unique_ptr<WraithAnim>& Anim, const std::unique_ptr<XAnim_t>& Animation)
{
    NamedNotetracks(Anim, Animation, 8);
}

void CoDXAnimTranslator::NotetracksBO3(const std::unique_ptr<WraithAnim>& Anim, const std::unique_ptr<XAnim_t>& Animation)
{
    for (uint32_t i = 0; i < Animation->NotificationCount; i++)
    {
        auto NotificationTag = CoDAssets::GameStringHandler(CoDAssets::GameInstance->Read<uint32_t>(Animation->NotificationsPtr + 8));
        auto NotificationType = CoDAssets::GameStringHandler(CoDAssets::GameInstance->Read<uint32_t>(Animation->NotificationsPtr));
        uint32_t NotificationFrame = (uint32_t)((float)Animation->FrameCount * CoDAssets::GameInstance->Read<float>(Animation->NotificationsPtr + 4));

        // Append the removed prefixes to the notetrack
        if (NotificationType == "sound")
            NotificationTag = "sndnt#" + NotificationTag;
        else if (NotificationType == "rumble")
            NotificationTag = "rmbnt#" + NotificationTag;
        else if (NotificationType == "vox")
            NotificationTag = "vox#" + NotificationTag;

        if (!Strings::IsNullOrWhiteSpace(NotificationTag))
        {
            Anim->AddNoteTrack(NotificationTag, NotificationFrame);
        }

        Animation->NotificationsPtr += 16;
    }
}

void CoDXAnimTranslator::NotetracksBO4(const std::unique_ptr<WraithAnim>& Anim, const std::unique_ptr<XAnim_t>& Animation)
{
    HashedNotetracks(Anim, Animation, 0x10, 0x1C, 0x20, 0x28);
}

void CoDXAnimTranslator::NotetracksCW(const std::unique_ptr<WraithAnim>& Anim, const std::unique_ptr<XAnim_t>& Animation)
{
    HashedNotetracks(Anim, Animation, 0x8, 0x14, 0x18, 0x20);
}

void CoDXAnimTranslator::NotetracksVG(const std::unique_ptr<WraithAnim>& Anim, const std::unique_ptr<XAnim_t>& Animation)
{
    NamedNotetracks(Anim, Animation, 16);
}

// TODO: Move to header
struct XAnimBlendShapeTargetInfo
{
    uint32_t Name;
    uint32_t Index;
};

struct XAnimBlendShapeFrames
{
    uint32_t FrameCount;
    uint32_t Pad;
    uint64_t Frames;
    uint64_t Weights;
};

void CoDXAnimTranslator::BlendShapesBO4(const std::unique_ptr<WraithAnim>& Anim, const std::unique_ptr<XAnim_t>& Animation)
{
    for (uint32_t i = 0; i < Animation->BlendShapeWeightCount; i++)
    {
        auto BlendShapeInfo = CoDAssets::GameInstance->Read<XAnimBlendShapeTargetInfo>(Animation->BlendShapeNamesPtr + i * sizeof(XAnimBlendShapeTargetInfo));
        auto BlendShapeName = CoDAssets::GameStringHandler(BlendShapeInfo.Name);
        auto BlendShapeFrames = CoDAssets::GameInstance->Read<XAnimBlendShapeFrames>(Animation->BlendShapeWeightsPtr + BlendShapeInfo.Index * sizeof(XAnimBlendShapeFrames));

        auto Frames = std::make_unique<uint16_t[]>(BlendShapeFrames.FrameCount);
        auto Weights = std::make_unique<Vector3[]>(BlendShapeFrames.FrameCount);

        CoDAssets::GameInstance->Read((uint8_t*)Frames.get(), BlendShapeFrames.Frames, BlendShapeFrames.FrameCount * sizeof(uint16_t));
        CoDAssets::GameInstance->Read((uint8_t*)Weights.get(), BlendShapeFrames.Weights, BlendShapeFrames.FrameCount * sizeof(Vector3));

        for (uint32_t i = 0; i < Animation->FrameCount; i++)
        {
            // Look up the next shape index by frame
            size_t FrameIndex = 0;
            //https://github.com/Scobalula/Greyhound/commit/4c92e978a9d87a3b867ade9e8b3cc98521c928f5#diff-b3246cb1d3566246b2329f38c3bca00cf25806fb9ba32b06f0c1b1cb34c63fd6L956
            //Removing this in the above commit seems to have broken the blendshapes for BO4/CW
            for (size_t j = 0; j < BlendShapeFrames.FrameCount; j++)
            {
                if (i < Frames[j])
                {
                    FrameIndex = j - 1;
                    break;
                }
            }

            // Now we need this frame, and the next frame
            auto CurrentFrame  = (float)Frames[FrameIndex];
            auto& CurrentWeight = Weights[FrameIndex];
            auto NextFrame     = (float)Frames[FrameIndex + 1];
            auto& NextWeight    = Weights[FrameIndex + 1];

            // Perform weighted interpolation
            float frameDelta = (i - CurrentFrame) / (NextFrame - CurrentFrame);
            float v = 0.0f;

            v += CurrentWeight.X * ((1.0 - frameDelta) * (1.0 - frameDelta) * (1.0 - frameDelta));
            v += (CurrentWeight.Y * 3.0) * (1.0 - frameDelta) * (1.0 - frameDelta) * frameDelta;
            v += (CurrentWeight.Z * 3.0) * (1.0 - frameDelta) * frameDelta * frameDelta;
            v += NextWeight.X * frameDelta * frameDelta * frameDelta;

            Anim->AddBlendShapeKey(BlendShapeName, i, v, 0, 0);
        }
    }
}

void CoDXAnimTranslator::BlendShapesMW(const std::unique_ptr<WraithAnim>& Anim, const std::unique_ptr<XAnim_t>& Animation)
{
    auto ShapeWeights = std::make_unique<uint16_t[]>(Animation->BlendShapeWeightCount * ((uint64_t)Animation->FrameCount + 1));
    auto Result = CoDAssets::GameInstance->Read((uint8_t*)ShapeWeights.get(), Animation->BlendShapeWeightsPtr, Animation->BlendShapeWeightCount * ((uint64_t)Animation->FrameCount + 1) * sizeof(uint16_t));
    size_t ShapeIndex = 0;

    std::vector<std::string> Shapes(Animation->BlendShapeWeightCount);
    for (uint32_t i = 0; i < Animation->BlendShapeWeightCount; i++)
    {
        auto BlendShapeName = CoDAssets::GameStringHandler(CoDAssets::GameInstance->Read<uint32_t>(Animation->BlendShapeNamesPtr + i * sizeof(uint32_t)));
        Shapes[i] = BlendShapeName;
    }

    for (uint32_t j = 0; j < (Animation->FrameCount + 1); j++)
    {
        auto FrameValues = &ShapeWeights[(size_t)j * Animation->BlendShapeWeightCount];

        for (uint32_t i = 0; i < Animation->BlendShapeWeightCount; i++)
        {
            Anim->AddBlendShapeKey(Shapes[i], j, (FrameValues[i] * 0.00030518509) - 10.0f, 0, 0);
        }
    }
}


namespace
{
    // The root (tag_origin) translations: u16 frame count, u8 data size (bytes or shorts), padding
    // to HeaderSize, the min and size vectors; a count of 0 makes the min vector frame 0's key,
    // else a TPtr to the keys follows, then the frame indices
    template <typename TPtr>
    void DeltaTranslations(const std::unique_ptr<WraithAnim>& Anim, uint32_t FrameSize, uint64_t& Cursor, uint32_t HeaderSize)
    {
        uint32_t FrameCount = CoDAssets::GameInstance->Read<uint16_t>(Cursor);
        uint8_t DataSize = CoDAssets::GameInstance->Read<uint8_t>(Cursor + 2);
        Cursor += HeaderSize;

        auto MinVec = CoDAssets::GameInstance->Read<Vector3>(Cursor);
        Cursor += 12;
        auto SizeVec = CoDAssets::GameInstance->Read<Vector3>(Cursor);
        Cursor += 12;

        if (FrameCount == 0)
        {
            Anim->AddTranslationKey("tag_origin", 0, MinVec.X, MinVec.Y, MinVec.Z);
            return;
        }

        TPtr DeltaDataPtr = CoDAssets::GameInstance->Read<TPtr>(Cursor);
        Cursor += sizeof(TPtr);

        for (uint32_t i = 0; i < (FrameCount + 1); i++)
        {
            const uint32_t FrameIndex = ReadFrameIndex(Cursor, FrameSize);

            float XCoord = 0, YCoord = 0, ZCoord = 0;
            if (DataSize == 1)
            {
                XCoord = CoDAssets::GameInstance->Read<uint8_t>((uint64_t)DeltaDataPtr);
                YCoord = CoDAssets::GameInstance->Read<uint8_t>((uint64_t)DeltaDataPtr + 1);
                ZCoord = CoDAssets::GameInstance->Read<uint8_t>((uint64_t)DeltaDataPtr + 2);
                DeltaDataPtr += 3;
            }
            else
            {
                XCoord = CoDAssets::GameInstance->Read<uint16_t>((uint64_t)DeltaDataPtr);
                YCoord = CoDAssets::GameInstance->Read<uint16_t>((uint64_t)DeltaDataPtr + 2);
                ZCoord = CoDAssets::GameInstance->Read<uint16_t>((uint64_t)DeltaDataPtr + 4);
                DeltaDataPtr += 6;
            }

            float TranslationX = (SizeVec.X * XCoord) + MinVec.X;
            float TranslationY = (SizeVec.Y * YCoord) + MinVec.Y;
            float TranslationZ = (SizeVec.Z * ZCoord) + MinVec.Z;
            Anim->AddTranslationKey("tag_origin", FrameIndex, TranslationX, TranslationY, TranslationZ);
        }
    }

    // The root (tag_origin) rotations, TKey each (Quat2Data or QuatData): u16 frame count padded
    // to a TPtr; a count of 0 makes the next key frame 0's, else a TPtr to the keys follows, then
    // the frame indices
    template <typename TPtr, typename TKey>
    void DeltaRotations(const std::unique_ptr<WraithAnim>& Anim, uint32_t FrameSize, uint64_t& Cursor, AnimationKeyTypes Type)
    {
        uint32_t FrameCount = CoDAssets::GameInstance->Read<uint16_t>(Cursor);
        Cursor += sizeof(TPtr);

        if (FrameCount == 0)
        {
            auto RotationData = CoDAssets::GameInstance->Read<TKey>(Cursor);
            Cursor += sizeof(TKey);

            if (IsRotationKeyType(Type))
            {
                const auto Rotation = RotationKey(RotationData, Type);
                Anim->AddRotationKey("tag_origin", 0, Rotation.X, Rotation.Y, Rotation.Z, Rotation.W);
            }
            return;
        }

        TPtr DeltaDataPtr = CoDAssets::GameInstance->Read<TPtr>(Cursor);
        Cursor += sizeof(TPtr);

        for (uint32_t i = 0; i < (FrameCount + 1); i++)
        {
            const uint32_t FrameIndex = ReadFrameIndex(Cursor, FrameSize);

            auto RotationData = CoDAssets::GameInstance->Read<TKey>((uint64_t)DeltaDataPtr);
            DeltaDataPtr += sizeof(TKey);

            if (IsRotationKeyType(Type))
            {
                const auto Rotation = RotationKey(RotationData, Type);
                Anim->AddRotationKey("tag_origin", FrameIndex, Rotation.X, Rotation.Y, Rotation.Z, Rotation.W);
            }
        }
    }
}

// The 32-bit layouts store their root rotations divided by size
void CoDXAnimTranslator::DeltaTranslations32(const std::unique_ptr<WraithAnim>& Anim, uint32_t FrameSize, const std::unique_ptr<XAnim_t>& Animation)
{
    DeltaTranslations<uint32_t>(Anim, FrameSize, Animation->DeltaTranslationPtr, 4);
}

void CoDXAnimTranslator::DeltaTranslations64(const std::unique_ptr<WraithAnim>& Anim, uint32_t FrameSize, const std::unique_ptr<XAnim_t>& Animation)
{
    DeltaTranslations<uint64_t>(Anim, FrameSize, Animation->DeltaTranslationPtr, 8);
}

void CoDXAnimTranslator::Delta2DRotations32(const std::unique_ptr<WraithAnim>& Anim, uint32_t FrameSize, const std::unique_ptr<XAnim_t>& Animation)
{
    DeltaRotations<uint32_t, Quat2Data>(Anim, FrameSize, Animation->Delta2DRotationsPtr, AnimationKeyTypes::DivideBySize);
}

void CoDXAnimTranslator::Delta2DRotations64(const std::unique_ptr<WraithAnim>& Anim, uint32_t FrameSize, const std::unique_ptr<XAnim_t>& Animation)
{
    DeltaRotations<uint64_t, Quat2Data>(Anim, FrameSize, Animation->Delta2DRotationsPtr, Animation->RotationType);
}

void CoDXAnimTranslator::Delta3DRotations32(const std::unique_ptr<WraithAnim>& Anim, uint32_t FrameSize, const std::unique_ptr<XAnim_t>& Animation)
{
    DeltaRotations<uint32_t, QuatData>(Anim, FrameSize, Animation->Delta3DRotationsPtr, AnimationKeyTypes::DivideBySize);
}

void CoDXAnimTranslator::Delta3DRotations64(const std::unique_ptr<WraithAnim>& Anim, uint32_t FrameSize, const std::unique_ptr<XAnim_t>& Animation)
{
    DeltaRotations<uint64_t, QuatData>(Anim, FrameSize, Animation->Delta3DRotationsPtr, Animation->RotationType);
}
