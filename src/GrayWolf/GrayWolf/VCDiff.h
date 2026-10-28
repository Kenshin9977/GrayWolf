#pragma once

#include "ByteView.h"
#include "ZoneMemory.h"

#include <cstdint>
#include <string>
#include <vector>

// A VCDIFF (RFC 3284) decoder: the "BDelta" format BO4's FastFileCompression::BDeltaZLib
// blocks contain after their zlib layer is removed. Ported from open-vcdiff (Apache 2.0,
// https://github.com/google/open-vcdiff): src/codetable.cc's default code table, and the
// window/instruction decode algorithm in src/vcdecoder.cc + src/addrcache.cc + src/headerparser.cc.
namespace VCDiff
{
    // Source may be empty if the delta never references it (VCD_TARGET-only or ADD-only streams).
    bool Decode(ByteView Source, const uint8_t* Delta, size_t DeltaSize,
        ZoneBytes& Target, std::string& Error);
}
