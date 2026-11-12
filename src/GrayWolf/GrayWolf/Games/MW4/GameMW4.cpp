#include "stdafx.h"

#include "GameMW4.h"

#include "Games/BO7/GameBlackOps7.h"
#include "Games/IW/IwctxReaders.h"
#include "SABSupport.h"

// Every layout below is the one the mw4 tables walk (Wolff's profiles/mw4/tables.generated.txt, the
// loader named beside each struct) and the bytes of the working set's zones confirm (C:\tmp\mw4).

std::unique_ptr<XSound> GameMW4::ReadXSound(const CoDSound_t* Sound)
{
    // the table's test (0x1d24110): (u8 0x30 & 0x18) == 0 -> the in-zone stream info at 8, else the
    // .xsub key at 8 (mp_rew_escape: flags 0x08 and 0x10 hold a key at 8, 117 of 117; 0x00, 0x02
    // and 0x04 none, 4 942 of 4 942, their object's key at 0x10 as Black Ops 7's)
    static const IwctxReaders::SoundLayout Layout{ SoundFlags, SoundRate,
                                                   [](uint8_t Flags) {
                                                       IwctxReaders::SoundSource S;
                                                       if ((Flags & 0x18) != 0) S.Key = SoundKey;
                                                       else S.ZoneFrames = SoundKey, S.ZoneSize = 0x10, S.KeyElse = 0x10;
                                                       return S;
                                                   },
                                                   true, SABSupport::OpusPreSkipLookAhead };
    return IwctxReaders::ReadXSound(Sound, Layout);
}

// ---- xanim
namespace
{
    // XAnimParts (0x90, 0x1d8ae40): name hash 0, rate float 0x18 (0x1c = rate / u16 0x2a), the last
    // frame's index u16 0x2a, ten bone counts u16 from 0x30 (NoneRot, Rot2D, Rot3D, Static2D,
    // Static3D, Trans, TransPrecise, TransStatic, TransNone, Total: the five rotation counts and the
    // four translation counts each sum to Total on mp_rew_zodiac_entities' anims), flags u8 0x44 (bit
    // 0 looping), notetrack count u8 0x45, asset type u8 0x46: Black Ops 7's header four bytes later.
    // The part at 0x48 is Black Ops 7's (0x28: the bone list 0 / u16 8, part count u8 0xa, notetracks
    // 0x18 / u8 0x14, the part list 0x20), read by its sampler's rules (not checked against Modern
    // Warfare 4's own sampler). Not read: the eight-byte pointer list at 0x80 (u8 0x88 of them).
    constexpr IwctxReaders::XAnimLayout AnimLayout{ 0x18, 0x2a, 0x44, 0x46, 0x30, 0x52, 0x68, true, 0x45, 0x48, 0x50, IwctxReaders::ScriptString };
}

std::unique_ptr<XAnim_t> GameMW4::ReadXAnim(const CoDAnim_t* Animation)
{
    return IwctxReaders::ReadXAnim(Animation, AnimLayout, GameBlackOps7::BoneName);
}
