#pragma once

// The pixels a zone carries for an image (its GfxImageLoadDef: the code images, $white and the
// like, the lightmaps) as a DDS of the largest level: CoD4, World at War, Black Ops, Black Ops II,
// CODOL and the 64-bit Modern Warfare 2 and 3 keep them so.

#include "Image.h"

#include <cstdint>
#include <memory>

struct XImageDDS;

namespace LoadDefImage
{
    // What the stored pixels need to become the DDS format's
    enum class Convert
    {
        None,
        SwapRB,      // B, G, R, A (D3D9's A8R8G8B8 and X8R8G8B8, DXGI's B8G8R8A8) to R, G, B, A
        Luminance,   // one byte a pixel, L (D3D9's L8) to L, L, L, 255: an A8 file would make it black
    };

    // A D3DFORMAT these zones keep (DXT1, DXT3, DXT5, ATI1, ATI2, A8R8G8B8, X8R8G8B8, A8, L8): its DDS
    // format, the bytes of a stored W x H level and what they need; false for any other
    bool D3dFormat(uint32_t Format, uint32_t W, uint32_t H, ImageFormat& Fmt, uint64_t& Level, Convert& How);

    // A W x H DDS in Fmt of Faces stored levels of Level bytes each, face F's from Pixels + F * Stride
    // (six faces: a cube map), converted as How says
    std::unique_ptr<XImageDDS> Dds(uint32_t W, uint32_t H, ImageFormat Fmt, const uint8_t* Pixels, uint64_t Level, Convert How = Convert::None,
                                   uint64_t Faces = 1, uint64_t Stride = 0);
}
