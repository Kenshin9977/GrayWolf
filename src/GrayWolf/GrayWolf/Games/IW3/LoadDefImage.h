#pragma once

// The pixels a 32-bit zone carries for an image (its GfxImageLoadDef: the code images, $white and
// the like, the lightmaps) as a DDS of the largest level: CoD4, World at War, Black Ops, Black Ops
// II and CODOL keep them so.

#include "Image.h"

#include <cstdint>
#include <memory>

struct XImageDDS;

namespace LoadDefImage
{
    // A D3DFORMAT these zones keep (DXT1, DXT3, DXT5, A8R8G8B8 and X8R8G8B8 with Swap set, L8): its
    // DDS format and the bytes of a W x H level; false for any other
    bool D3dFormat(uint32_t Format, uint32_t W, uint32_t H, ImageFormat& Fmt, uint64_t& Level, bool& Swap);

    // A W x H DDS in Fmt of Faces levels of Level bytes each, face F's from Pixels + F * Stride (six
    // faces: a cube map); Swap exchanges every pixel's first and third bytes (BGRA to RGBA)
    std::unique_ptr<XImageDDS> Dds(uint32_t W, uint32_t H, ImageFormat Fmt, const uint8_t* Pixels, uint64_t Level, bool Swap = false,
                                   uint64_t Faces = 1, uint64_t Stride = 0);
}
