#pragma once

// Modern Warfare 4's asset readers (Call of Duty HQ's cod26, cod.exe PE timestamp 0x6add5b92), over
// its zones served by the disk template (Games/MW4/DiskGame.cpp). Read from its files only: no
// running-game reader.
//
// Its loaders are Black Ops 7's with a few changes (the mw4 tables of Wolff's profiles/mw4 against
// bo7's, field by field): XModel (0xd8), its skeleton (0x98), LODs (0x48), XModelSurfs (0x48),
// XSurface (0xf0), Material (0x78) and GfxImage (0x40) read the same fields at the same offsets, so
// those go through Black Ops 7's readers (Games/BO7/GameBlackOps7.h). What differs is read here: the
// sound's streamed test (bits 3-4 of its flags, not 4-5) and the anim header (0x90: four bytes more
// before the bone counts, a list of eight-byte pointers at 0x80).

#include <cstdint>
#include <memory>
#include <string>

#include "CoDXModelTranslator.h"

class GameMW4
{
public:
    // sndasset (0x40, loader 0x1d24110): its frames in the zone at 8 when (flags u8 0x30 & 0x18) is 0
    // (Black Ops 7: & 0x30 under 0x10), else the .xsub object whose key is at 8; seek table size u32
    // 0x20, frames' size u32 0x24, frame count u32 0x28, rate u16 0x32
    static std::unique_ptr<XSound> ReadXSound(const CoDSound_t* Sound);
    static constexpr int64_t SoundKey = 8, SoundFrames = 0x28, SoundFlags = 0x30, SoundRate = 0x32;

    // xanim (0x90, loader 0x1d8ae40): Black Ops 7's packed keys in 1 to 4 .xsub parts
    // (Games/IW/IwctxReaders, XAnimLayout: its reader decodes the keys), its header fields four bytes later
    static std::unique_ptr<XAnim_t> ReadXAnim(const CoDAnim_t* Animation);
};
