#include "stdafx.h"

#include "SeAnimReader.h"
#include "WraithAnim.h"

#include <fstream>
#include <cmath>
#include <cstring>
#include <initializer_list>

namespace
{
    // A forward-only cursor with bounds checks: any over-read trips Bad.
    struct Cursor
    {
        const uint8_t* P; size_t N, O = 0; bool Bad = false;
        Cursor(const uint8_t* p, size_t n) : P(p), N(n) {}
        uint8_t U8()  { if (O + 1 > N) { Bad = true; return 0; } return P[O++]; }
        uint16_t U16() { uint16_t v = 0; if (O + 2 > N) { Bad = true; return 0; } std::memcpy(&v, P + O, 2); O += 2; return v; }
        uint32_t U32() { uint32_t v = 0; if (O + 4 > N) { Bad = true; return 0; } std::memcpy(&v, P + O, 4); O += 4; return v; }
        float F32()   { float v = 0;    if (O + 4 > N) { Bad = true; return 0; } std::memcpy(&v, P + O, 4); O += 4; return v; }
        std::string CStr() { std::string s; while (O < N && P[O] != 0) s.push_back((char)P[O++]); if (O < N) O++; else Bad = true; return s; }
    };
}

bool ReadSeAnim(const std::string& Path, SeAnimClip& Out)
{
    std::ifstream In(Path, std::ios::binary);
    if (!In) return false;
    std::vector<uint8_t> Bytes((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
    if (Bytes.size() < 36 || std::memcmp(Bytes.data(), "SEAnim", 6) != 0) return false;

    Cursor C(Bytes.data(), Bytes.size());
    C.O = 6;
    C.U16(); // version
    C.U16(); // headerSize - NOT trusted: this codebase's writer declares 0x1C (28) but actually
             // writes a 36-byte fixed header (through notificationCount); fixed offsets are used below.
    const uint8_t AnimType = C.U8();
    const uint8_t Flags = C.U8();
    const uint8_t DataPresent = C.U8();
    const uint8_t DataProp = C.U8();
    C.U16(); // reserved
    Out.FrameRate = C.F32();
    Out.FrameCount = C.U32();
    const uint32_t BoneCount = C.U32();
    const uint8_t ModifierCount = C.U8();
    C.U8(); C.U8(); C.U8(); // 3 reserved bytes
    const uint32_t NotificationCount = C.U32();
    if (C.Bad) return false;

    if (DataProp & 0x1) return false; // SEANIM_PRECISION_HIGH (double vectors) - unsupported, unused by our writer

    Out.Type = (SeAnimBoneType)AnimType;
    Out.Looping = (Flags & 1) != 0;

    // frame_t size depends on FrameCount, and is reused for every per-bone key COUNT field too (not
    // sized by the key count itself) - see SEAnimExport.cpp's FrameCountBuffer branches.
    const int FrameSz = (Out.FrameCount <= 0xFF) ? 1 : (Out.FrameCount <= 0xFFFF) ? 2 : 4;
    auto ReadFrameIdx = [&]() -> uint32_t {
        if (FrameSz == 1) return C.U8();
        if (FrameSz == 2) return C.U16();
        return C.U32();
    };

    // A key holding a value no pose has (not finite, or a translation of a billion units: an
    // exporter's garbage) is dropped, its neighbours stand in: posed, it threw the viewer's framing out
    const auto Sane = [](std::initializer_list<float> V) {
        for (const float F : V) if (!std::isfinite(F) || std::fabs(F) > 1e9f) return false;
        return true;
    };

    // A count the rest of the file cannot hold is a corrupt header: refused before it sizes a vector
    const auto Fits = [&](uint64_t Count, uint64_t Each) { return Count <= (C.N - C.O) / Each; };

    if (!Fits(BoneCount, 2)) return false;   // a name's terminator and a flags byte each
    Out.Bones.resize(BoneCount);
    for (uint32_t i = 0; i < BoneCount; i++) { Out.Bones[i].Name = C.CStr(); if (C.Bad) return false; }

    // Modifier bone-index size: byte if BoneCount fits, else short - no 4-byte fallback (matches the
    // writer, which never emits one either; CoD skeletons don't approach 65536 bones in practice).
    const int ModIdxSz = (BoneCount <= 0xFF) ? 1 : 2;
    for (uint8_t i = 0; i < ModifierCount; i++)
    {
        const uint32_t Idx = (ModIdxSz == 1) ? C.U8() : C.U16();
        const uint8_t Ty = C.U8();
        if (C.Bad) return false;
        if (Idx < Out.Bones.size()) { Out.Bones[Idx].HasTypeOverride = true; Out.Bones[Idx].TypeOverride = (SeAnimBoneType)Ty; }
    }

    // Per-bone keyframe blocks, in the same order as the bone tag list above.
    for (uint32_t i = 0; i < BoneCount; i++)
    {
        C.U8(); // bone flags - unused (reserved for future per-bone options; none defined yet)
        if (C.Bad) return false;

        if (DataPresent & 0x1) // SEANIM_BONE_LOC
        {
            const uint32_t Count = ReadFrameIdx();
            if (!Fits(Count, FrameSz + 12)) return false;
            Out.Bones[i].PosKeys.reserve(Count);
            for (uint32_t k = 0; k < Count; k++)
            {
                const uint32_t Frame = ReadFrameIdx();
                const float X = C.F32(), Y = C.F32(), Z = C.F32();
                if (Sane({ X, Y, Z })) Out.Bones[i].PosKeys.push_back({ Frame, { X, Y, Z } });
            }
        }
        if (DataPresent & 0x2) // SEANIM_BONE_ROT
        {
            const uint32_t Count = ReadFrameIdx();
            if (!Fits(Count, FrameSz + 16)) return false;
            Out.Bones[i].RotKeys.reserve(Count);
            for (uint32_t k = 0; k < Count; k++)
            {
                const uint32_t Frame = ReadFrameIdx();
                const float X = C.F32(), Y = C.F32(), Z = C.F32(), W = C.F32();
                if (Sane({ X, Y, Z, W })) Out.Bones[i].RotKeys.push_back({ Frame, { X, Y, Z, W } });
            }
        }
        if (DataPresent & 0x4) // SEANIM_BONE_SCALE
        {
            const uint32_t Count = ReadFrameIdx();
            if (!Fits(Count, FrameSz + 12)) return false;
            Out.Bones[i].ScaleKeys.reserve(Count);
            for (uint32_t k = 0; k < Count; k++)
            {
                const uint32_t Frame = ReadFrameIdx();
                const float X = C.F32(), Y = C.F32(), Z = C.F32();
                if (Sane({ X, Y, Z })) Out.Bones[i].ScaleKeys.push_back({ Frame, { X, Y, Z } });
            }
        }
        if (C.Bad) return false;
    }

    // Notifications: read past them (frame + null-terminated name each), discarded.
    if (DataPresent & 0x40) // SEANIM_PRESENCE_NOTE
    {
        for (uint32_t i = 0; i < NotificationCount; i++)
        {
            ReadFrameIdx();
            C.CStr();
            if (C.Bad) return false;
        }
    }

    return true;
}

bool BuildWraithAnimFromSeAnimClip(const SeAnimClip& Clip, WraithAnim& Out)
{
    if (Clip.Bones.empty()) return false;

    Out = WraithAnim();
    Out.AnimType = (WraithAnimationType)Clip.Type; // same numeric values by design, see the header
    Out.Looping = Clip.Looping;
    Out.FrameRate = Clip.FrameRate;

    for (const auto& Track : Clip.Bones)
    {
        if (Track.HasTypeOverride)
            Out.AddBoneModifier(Track.Name, (WraithAnimationType)Track.TypeOverride);

        for (const auto& K : Track.PosKeys)
            Out.AddTranslationKey(Track.Name, K.Frame, K.Value.x, K.Value.y, K.Value.z);
        for (const auto& K : Track.RotKeys)
            Out.AddRotationKey(Track.Name, K.Frame, K.Value.x, K.Value.y, K.Value.z, K.Value.w);
        for (const auto& K : Track.ScaleKeys)
            Out.AddScaleKey(Track.Name, K.Frame, K.Value.x, K.Value.y, K.Value.z);
    }

    for (const auto& Notify : Clip.Notifications)
        for (uint32_t Frame : Notify.second)
            Out.AddNoteTrack(Notify.first, Frame);

    return true;
}
