#pragma once

// The packed, streamed XAnim of MW2 (2022) and MW3 (2023): the header names the bones and
// notetracks, the keys live in package objects (one per stream info), and a per-frame bit table
// says which buffer holds each key. Each game keeps its own structs (TAnim, TDelta, TNotetrack,
// TStreamInfo); the reading is one.

#include <intrin.h>

#include <cstdint>
#include <functional>
#include <exception>
#include <memory>
#include <vector>

#include "CoDAssets.h"

namespace PackedXAnim
{
    struct BufferState
    {
        uint32_t* PackedPerFrameInfo;
        // The current buffer index requested from the packed buffer.
        size_t BufferIndex;
        // The offset within the buffer from the packed buffer.
        size_t BufferOffset;
        size_t OffsetCount;
        // The current offset within the packed buffer.
        size_t PackedPerFrameOffset;
    };

    inline size_t CalculateBufferOffset(const BufferState* State, const size_t Index, const size_t Count)
    {
        if (Count == 0)
            return 0;

        size_t Mask = Index + Count - 1;
        size_t Start = Index >> 5;
        size_t End = Mask >> 5;
        size_t Result = 0;

        for (size_t i = Start; i <= End; i++)
        {
            if (State->PackedPerFrameInfo[i] == 0)
                continue;

            uint32_t MaskA = 0xFFFFFFFF;
            uint32_t MaskB = 0xFFFFFFFF;

            // At the start or end, trailing bit masks
            if (i == Start)
                MaskA = 0xFFFFFFFF >> (Index & 0x1F);
            if (i == End)
                MaskB = 0xFFFFFFFF << (31 - (Mask & 0x1F));

            Result += __popcnt(State->PackedPerFrameInfo[i] & MaskA & MaskB);
        }

        return Result;
    }

    template <typename T>
    void IncrementBuffers(BufferState* State, int TableSize, const size_t ElemCount, std::vector<T*>& Buffers)
    {
        if (TableSize < 4 || State->OffsetCount == 1)
        {
            Buffers[0] += ElemCount * TableSize;
        }
        else
        {
            for (size_t i = 0; i < State->OffsetCount; i++)
            {
                Buffers[i] += ElemCount * CalculateBufferOffset(State, State->PackedPerFrameOffset, TableSize);
                State->PackedPerFrameOffset += TableSize;
            }
        }
    }

    inline void CalculateBufferIndex(BufferState* State, const size_t TableSize, const size_t KeyFrameIndex)
    {
        if (TableSize < 4 || State->OffsetCount == 1)
        {
            State->BufferIndex = 0;
            State->BufferOffset = KeyFrameIndex;
            return;
        }

        for (size_t i = 0; i < State->OffsetCount; i++)
        {
            if (((0x80000000 >> ((KeyFrameIndex + (i * TableSize) + State->PackedPerFrameOffset) & 0x1F)) &
                 State->PackedPerFrameInfo[(KeyFrameIndex + (i * TableSize) + State->PackedPerFrameOffset) >> 5]) != 0)
            {
                State->BufferIndex = i;
                State->BufferOffset = CalculateBufferOffset(State, State->PackedPerFrameOffset + TableSize * State->BufferIndex, KeyFrameIndex);
                return;
            }
        }

        // Shouldn't happen on a valid xanim.
        throw std::exception("");
    }

    // The header of a packed XAnim: counts, bone names and notetracks, read later by Loader
    // (nullptr without stream info)
    template <typename TAnim, typename TDelta, typename TNotetrack>
    std::unique_ptr<XAnim_t> Read(const CoDAnim_t* Animation, std::function<void(const std::unique_ptr<XAnim_t>&, std::unique_ptr<WraithAnim>&)> Loader)
    {
        if (!CoDAssets::GameInstance->IsRunning()) return nullptr;

        auto AnimData = CoDAssets::GameInstance->Read<TAnim>(Animation->AssetPointer);

        // No stream info: an anim of no bone has none (a camera's or a notetrack's: MWIII's void,
        // void_loop, iw8_mp_end_game_*_cam_*), written with its notetracks and delta parts; with
        // bones, never encountered, not handled; with neither notetracks nor delta parts, a header of
        // its hash only (another zone's anim, its zone's IsReference): nothing to write
        if (AnimData.DataInfo.StreamInfoPtr == 0 &&
            (AnimData.TotalBoneCount != 0 || (AnimData.NotetrackCount == 0 && AnimData.DeltaPartsPtr == 0)))
        {
            return nullptr;
        }

        auto Anim = std::make_unique<XAnim_t>();

        Anim->AnimationName = Animation->AssetName;
        Anim->FrameCount = AnimData.FrameCount;
        Anim->FrameRate = AnimData.Framerate;

        Anim->LoopingAnimation = (AnimData.Padding2[4] & 1) != 0;
        Anim->AdditiveAnimation = AnimData.AssetType == 0x6;

        const auto AnimDeltaData = CoDAssets::GameInstance->Read<TDelta>(AnimData.DeltaPartsPtr);

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
        Anim->NotificationCount            = AnimData.NotetrackCount;

        // The keys are read by Loader (streamed, packed: not the standard reader)
        Anim->ReaderFunction = Loader;
        Anim->ReaderInformationPointer = Animation->AssetPointer;
        Anim->Reader = std::make_unique<CoDXAnimReader>();

        for (size_t b = 0; b < AnimData.TotalBoneCount; ++b)
        {
            const uint32_t BoneID = CoDAssets::GameInstance->Read<uint32_t>(AnimData.BoneIDsPtr + b * 4);
            Anim->Reader->BoneNames.push_back(CoDAssets::GetHashedString("bone", (uint64_t)BoneID));
        }

        for (size_t n = 0; n < AnimData.NotetrackCount; ++n)
        {
            const auto NoteTrack = CoDAssets::GameInstance->Read<TNotetrack>(AnimData.NotificationsPtr + n * sizeof(TNotetrack));
            Anim->Reader->Notetracks.push_back({ CoDAssets::GameStringHandler(NoteTrack.Name), (size_t)(NoteTrack.Time * (float)Anim->FrameCount) });
        }

        Anim->DeltaTranslationPtr = AnimDeltaData.DeltaTranslationsPtr;
        Anim->Delta2DRotationsPtr = AnimDeltaData.Delta2DRotationsPtr;
        Anim->Delta3DRotationsPtr = AnimDeltaData.Delta3DRotationsPtr;

        Anim->RotationType = AnimationKeyTypes::DivideBySize;
        Anim->TranslationType = AnimationKeyTypes::MinSizeTable;

        Anim->SupportsInlineIndicies = true;

        return Anim;
    }

    // The keys of a packed XAnim Read described
    template <typename TAnim, typename TStreamInfo>
    void Load(const std::unique_ptr<XAnim_t>& Anim, std::unique_ptr<WraithAnim>& ResultAnim)
    {
        auto animHeader = CoDAssets::GameInstance->Read<TAnim>(Anim->ReaderInformationPointer);
        auto animBuffers = std::vector<std::unique_ptr<uint8_t[]>>();
        auto indicesBuffer = std::make_unique<uint8_t[]>(animHeader.FrameCount >= 0x100 ? (size_t)animHeader.IndexCount * 2 : (size_t)animHeader.IndexCount);

        ResultAnim->AnimType = WraithAnimationType::Relative;

        // DataBytes and DataShorts are always in buffer 0; random data spans every buffer
        uint8_t* dataByte = (uint8_t*)animBuffers.data();
        int16_t* dataShort = (int16_t*)animBuffers.data();
        int32_t* dataInt = (int32_t*)animBuffers.data();
        uint16_t* indices = (uint16_t*)indicesBuffer.get();
        auto randomDataBytes = std::vector<uint8_t*>();
        auto randomDataShorts = std::vector<int16_t*>();

        // Per-channel data offsets/buffers, for an xanim with multiple buffers
        auto animPackedInfo = std::make_unique<uint32_t[]>(animHeader.DataInfo.PackedInfoCount);
        CoDAssets::GameInstance->Read((uint8_t*)animPackedInfo.get(), animHeader.DataInfo.PackedInfoPtr, (size_t)animHeader.DataInfo.PackedInfoCount * 4);

        CoDAssets::GameInstance->Read(indicesBuffer.get(), animHeader.IndicesPtr, animHeader.FrameCount >= 0x100 ? (size_t)animHeader.IndexCount * 2 : (size_t)animHeader.IndexCount);

        uint32_t bufferSize = 0;

        // an anim of no bone has no stream info (Read): no buffer to fetch
        const size_t Offsets = animHeader.DataInfo.StreamInfoPtr != 0 ? animHeader.DataInfo.OffsetCount : 0;
        for (size_t i = 0; i < Offsets; i++)
        {
            auto StreamInfo = CoDAssets::GameInstance->Read<TStreamInfo>(animHeader.DataInfo.StreamInfoPtr + i * 16);

            animBuffers.push_back(CoDAssets::GamePackageCache->ExtractPackageObject(StreamInfo.StreamKey, StreamInfo.Size, bufferSize));

            if (i == 0)
            {
                // -1: no data of that kind to copy
                if (animHeader.DataInfo.DataByteOffset != -1)
                    dataByte = animBuffers[i].get() + animHeader.DataInfo.DataByteOffset;
                if (animHeader.DataInfo.DataShortOffset != -1)
                    dataShort = (int16_t*)(animBuffers[i].get() + animHeader.DataInfo.DataShortOffset);
                if (animHeader.DataInfo.DataIntOffset != -1)
                    dataInt = (int32_t*)(animBuffers[i].get() + animHeader.DataInfo.DataIntOffset);
            }

            auto RandomDataAOffset = CoDAssets::GameInstance->Read<uint32_t>(animHeader.DataInfo.OffsetPtr2 + i * 4);
            auto RandomDataBOffset = CoDAssets::GameInstance->Read<uint32_t>(animHeader.DataInfo.OffsetPtr + i * 4);

            if (RandomDataAOffset != -1)
                randomDataBytes.push_back(animBuffers[i].get() + RandomDataAOffset);
            else
                randomDataBytes.push_back(nullptr);

            if (RandomDataBOffset != -1)
                randomDataShorts.push_back((int16_t*)(animBuffers[i].get() + RandomDataBOffset));
            else
                randomDataShorts.push_back(nullptr);
        }

        BufferState state{};

        state.OffsetCount = Offsets;
        state.PackedPerFrameInfo = animPackedInfo.get();

        size_t currentBoneIndex = 0;
        size_t currentSize = Anim->NoneRotatedBoneCount;
        bool byteFrames = Anim->FrameCount < 0x100;

        // Stage 0: Zero-Rotated bones
        // Zero-rotated bones must be reset to identity, they are the first set of bones
        // Note: NoneTranslated bones aren't reset, they remain scene position
        while (currentBoneIndex < currentSize)
        {
            ResultAnim->AddRotationKey(Anim->Reader->BoneNames[currentBoneIndex++], 0, 0, 0, 0, 1.0f);
        }

        currentSize += Anim->TwoDRotatedBoneCount;

        // 2D Bones
        while (currentBoneIndex < currentSize)
        {
            auto tableSize = *dataShort++;

            if (tableSize >= 0x40 && !byteFrames)
                dataShort += (tableSize - 1 >> 8) + 2;

            for (int i = 0; i < tableSize + 1; i++)
            {
                uint32_t frame = 0;

                if (byteFrames)
                {
                    frame = *dataByte++;
                }
                else
                {
                    frame = tableSize >= 0x40 ? *indices++ : *dataShort++;
                }

                CalculateBufferIndex(&state, (size_t)tableSize + 1, i);

                auto randomDataShort = randomDataShorts[state.BufferIndex] + 2 * state.BufferOffset;

                float RZ = (float)randomDataShort[0] * 0.000030518509f;
                float RW = (float)randomDataShort[1] * 0.000030518509f;

                ResultAnim->AddRotationKey(Anim->Reader->BoneNames[currentBoneIndex], frame, 0, 0, RZ, RW);
            }

            IncrementBuffers(&state, tableSize + 1, 2, randomDataShorts);
            currentBoneIndex++;
        }

        currentSize += Anim->NormalRotatedBoneCount;

        // 3D Rotations
        while (currentBoneIndex < currentSize)
        {
            auto tableSize = *dataShort++;

            if (tableSize >= 0x40 && !byteFrames)
                dataShort += (tableSize - 1 >> 8) + 2;

            for (int i = 0; i < tableSize + 1; i++)
            {
                uint32_t frame = 0;

                if (byteFrames)
                {
                    frame = *dataByte++;
                }
                else
                {
                    frame = tableSize >= 0x40 ? *indices++ : *dataShort++;
                }

                CalculateBufferIndex(&state, (size_t)tableSize + 1, i);

                auto randomDataShort = randomDataShorts[state.BufferIndex] + 4 * state.BufferOffset;

                float RX = (float)randomDataShort[0] * 0.000030518509f;
                float RY = (float)randomDataShort[1] * 0.000030518509f;
                float RZ = (float)randomDataShort[2] * 0.000030518509f;
                float RW = (float)randomDataShort[3] * 0.000030518509f;

                ResultAnim->AddRotationKey(Anim->Reader->BoneNames[currentBoneIndex], frame, RX, RY, RZ, RW);
            }

            IncrementBuffers(&state, tableSize + 1, 4, randomDataShorts);
            currentBoneIndex++;
        }

        currentSize += Anim->TwoDStaticRotatedBoneCount;

        // Stage 3: 2D Static Rotations
        // 2D Static Rotations appear directly after the "3D Rotations"
        while (currentBoneIndex < currentSize)
        {
            float RZ = (float)*dataShort++ * 0.000030518509f;
            float RW = (float)*dataShort++ * 0.000030518509f;
            ResultAnim->AddRotationKey(Anim->Reader->BoneNames[currentBoneIndex], 0, 0, 0, RZ, RW);
            currentBoneIndex++;
        }

        currentSize += Anim->NormalStaticRotatedBoneCount;

        // Stage 4: 3D Static Rotations
        // 3D Static Rotations appear directly after the "2D Static Rotations"
        while (currentBoneIndex < currentSize)
        {
            float RX = (float)*dataShort++ * 0.000030518509f;
            float RY = (float)*dataShort++ * 0.000030518509f;
            float RZ = (float)*dataShort++ * 0.000030518509f;
            float RW = (float)*dataShort++ * 0.000030518509f;
            ResultAnim->AddRotationKey(Anim->Reader->BoneNames[currentBoneIndex], 0, RX, RY, RZ, RW);
            currentBoneIndex++;
        }

        // Translations: a short bone index each
        const auto Translations = [&](size_t Count, bool Precise)
        {
            for (size_t b = 0; b < Count; b++)
            {
                auto boneIndex = *dataShort++;
                auto tableSize = *dataShort++;

                if (tableSize >= 0x40 && !byteFrames)
                    dataShort += (tableSize - 1 >> 8) + 2;

                float minsVecX = *(float*)dataInt++;
                float minsVecY = *(float*)dataInt++;
                float minsVecZ = *(float*)dataInt++;
                float frameVecX = *(float*)dataInt++;
                float frameVecY = *(float*)dataInt++;
                float frameVecZ = *(float*)dataInt++;

                for (int i = 0; i < tableSize + 1; i++)
                {
                    int frame = 0;

                    if (byteFrames)
                    {
                        frame = *dataByte++;
                    }
                    else
                    {
                        frame = tableSize >= 0x40 ? *indices++ : *dataShort++;
                    }

                    CalculateBufferIndex(&state, (size_t)tableSize + 1, i);

                    float TranslationX, TranslationY, TranslationZ;
                    if (Precise)
                    {
                        auto randomDataShort = randomDataShorts[state.BufferIndex] + 3 * state.BufferOffset;
                        TranslationX = (frameVecX * (float)(uint16_t)randomDataShort[0]) + minsVecX;
                        TranslationY = (frameVecY * (float)(uint16_t)randomDataShort[1]) + minsVecY;
                        TranslationZ = (frameVecZ * (float)(uint16_t)randomDataShort[2]) + minsVecZ;
                    }
                    else
                    {
                        auto randomDataByte = randomDataBytes[state.BufferIndex] + 3 * state.BufferOffset;
                        TranslationX = (frameVecX * (float)randomDataByte[0]) + minsVecX;
                        TranslationY = (frameVecY * (float)randomDataByte[1]) + minsVecY;
                        TranslationZ = (frameVecZ * (float)randomDataByte[2]) + minsVecZ;
                    }
                    ResultAnim->AddTranslationKey(Anim->Reader->BoneNames[boneIndex], frame, TranslationX, TranslationY, TranslationZ);
                }

                if (Precise)
                    IncrementBuffers(&state, tableSize + 1, 3, randomDataShorts);
                else
                    IncrementBuffers(&state, tableSize + 1, 3, randomDataBytes);
            }
        };
        Translations(Anim->NormalTranslatedBoneCount, false);
        Translations(Anim->PreciseTranslatedBoneCount, true);

        // Stage 7: Static Translations
        // Static Translations appear directly after the "Precise Translations"
        for (size_t b = 0; b < Anim->StaticTranslatedBoneCount; b++)
        {
            auto vec = *(Vector3*)dataInt; dataInt += 3;
            auto boneIndex = *dataShort++;

            ResultAnim->AddTranslationKey(Anim->Reader->BoneNames[boneIndex], 0, vec.X, vec.Y, vec.Z);
        }
    }
}
