#include "stdafx.h"

#include "Games/IW3/LoadDefImage.h"

#include "CoDAssetType.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace LoadDefImage
{
    bool D3dFormat(uint32_t Format, uint32_t W, uint32_t H, ImageFormat& Fmt, uint64_t& Level, Convert& How)
    {
        const uint64_t Blocks = (uint64_t)std::max(1u, (W + 3) / 4) * std::max(1u, (H + 3) / 4);
        How = Convert::None;
        switch (Format)
        {
        case 0x31545844: Fmt = ImageFormat::DDS_BC1_UNORM; Level = Blocks * 8; return true;    // DXT1
        case 0x33545844: Fmt = ImageFormat::DDS_BC2_UNORM; Level = Blocks * 16; return true;   // DXT3
        case 0x35545844: Fmt = ImageFormat::DDS_BC3_UNORM; Level = Blocks * 16; return true;   // DXT5
        case 0x31495441: Fmt = ImageFormat::DDS_BC4_UNORM; Level = Blocks * 8; return true;    // ATI1
        case 0x32495441: Fmt = ImageFormat::DDS_BC5_UNORM; Level = Blocks * 16; return true;   // ATI2 (3Dc)
        // A8R8G8B8, X8R8G8B8: B, G, R, A in memory. MW2's $identitynormalmap holds 255, 128, 128
        // (its blue first) and $pixelcostcolorcode is the same ramp CoD4's swapped one reads
        case 21: case 22: Fmt = ImageFormat::DDS_Standard_A8R8G8B8; Level = (uint64_t)W * H * 4; How = Convert::SwapRB; return true;
        case 28: Fmt = ImageFormat::DDS_Standard_A8_UNORM; Level = (uint64_t)W * H; return true;   // A8
        case 50: Fmt = ImageFormat::DDS_Standard_A8R8G8B8; Level = (uint64_t)W * H; How = Convert::Luminance; return true;   // L8
        default: return false;
        }
    }

    std::unique_ptr<XImageDDS> Dds(uint32_t W, uint32_t H, ImageFormat Fmt, const uint8_t* Pixels, uint64_t Level, Convert How, uint64_t Faces,
                                   uint64_t Stride)
    {
        const uint64_t Out = How == Convert::Luminance ? Level * 4 : Level;   // a written level's bytes
        auto Result = std::make_unique<XImageDDS>();
        auto Buffer = new int8_t[Image::GetMaximumDDSHeaderSize() + Out * Faces];
        uint32_t Header = 0;
        Image::WriteDDSHeaderToStream(Buffer, W, H, 1, Fmt, Header, Faces == 6);
        auto* To = reinterpret_cast<uint8_t*>(Buffer + Header);
        for (uint64_t F = 0; F < Faces; F++)
        {
            const uint8_t* From = Pixels + F * Stride;
            uint8_t* Face = To + F * Out;
            if (How == Convert::Luminance)
                for (uint64_t P = 0; P < Level; P++) Face[4 * P] = Face[4 * P + 1] = Face[4 * P + 2] = From[P], Face[4 * P + 3] = 0xFF;
            else
                std::memcpy(Face, From, (size_t)Level);
        }
        if (How == Convert::SwapRB)
            for (uint64_t P = 0; P + 3 < Out * Faces; P += 4) std::swap(To[P], To[P + 2]);   // BGRA -> RGBA
        Result->DataBuffer = Buffer;
        Result->DataSize = (uint32_t)(Header + Out * Faces);
        return Result;
    }
}
