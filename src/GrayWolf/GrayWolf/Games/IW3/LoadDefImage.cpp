#include "stdafx.h"

#include "Games/IW3/LoadDefImage.h"

#include "CoDAssetType.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace LoadDefImage
{
    bool D3dFormat(uint32_t Format, uint32_t W, uint32_t H, ImageFormat& Fmt, uint64_t& Level, bool& Swap)
    {
        const uint64_t Blocks = (uint64_t)std::max(1u, (W + 3) / 4) * std::max(1u, (H + 3) / 4);
        Swap = false;
        switch (Format)
        {
        case 0x31545844: Fmt = ImageFormat::DDS_BC1_UNORM; Level = Blocks * 8; return true;    // DXT1
        case 0x33545844: Fmt = ImageFormat::DDS_BC2_UNORM; Level = Blocks * 16; return true;   // DXT3
        case 0x35545844: Fmt = ImageFormat::DDS_BC3_UNORM; Level = Blocks * 16; return true;   // DXT5
        case 21: case 22: Fmt = ImageFormat::DDS_Standard_A8R8G8B8; Level = (uint64_t)W * H * 4; Swap = true; return true;
        case 50: Fmt = ImageFormat::DDS_Standard_A8_UNORM; Level = (uint64_t)W * H; return true;   // L8
        default: return false;
        }
    }

    std::unique_ptr<XImageDDS> Dds(uint32_t W, uint32_t H, ImageFormat Fmt, const uint8_t* Pixels, uint64_t Level, bool Swap, uint64_t Faces,
                                   uint64_t Stride)
    {
        auto Result = std::make_unique<XImageDDS>();
        auto Buffer = new int8_t[Image::GetMaximumDDSHeaderSize() + Level * Faces];
        uint32_t Header = 0;
        Image::WriteDDSHeaderToStream(Buffer, W, H, 1, Fmt, Header, Faces == 6);
        for (uint64_t F = 0; F < Faces; F++) std::memcpy(Buffer + Header + F * Level, Pixels + F * Stride, (size_t)Level);
        if (Swap)
            for (uint64_t P = 0; P + 3 < Level * Faces; P += 4) std::swap(Buffer[Header + P], Buffer[Header + P + 2]);   // BGRA -> RGBA
        Result->DataBuffer = Buffer;
        Result->DataSize = (uint32_t)(Header + Level * Faces);
        return Result;
    }
}
