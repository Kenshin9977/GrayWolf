#include "stdafx.h"

#include "CoDIWITranslator.h"

#include "CoDIWIDecoder.h"

#include "Image.h"
#include "MemoryReader.h"
#include "CoDAssetType.h"

#include <algorithm>
#include <cstring>

struct IWIHeader
{
    uint8_t Magic[3];
    uint8_t Version;
};

struct IWIInfo
{
    uint8_t ImageFormat;
    uint8_t ImageFlags;
    uint16_t ImageWidth;
    uint16_t ImageHeight;
};

struct IWIBigMips
{
    int32_t MipOffset1;
    int32_t MipOffset2;
    int32_t MipOffset3;
    int32_t MipOffset4;
    int32_t MipOffset5;
    int32_t MipOffset6;
    int32_t MipOffset7;
    int32_t MipOffset8;
};

struct IWISmallMips
{
    int32_t MipOffset1;
    int32_t MipOffset2;
    int32_t MipOffset3;
    int32_t MipOffset4;
};

enum class IWIFileFlags : uint8_t
{
    IMG_FLAG_CUBEMAP = 0x4
};

static_assert(sizeof(IWIHeader) == 0x4, "Invalid IWIHeader Size (Expected 0x4)");
static_assert(sizeof(IWIInfo) == 0x6, "Invalid IWIInfo Size (Expected 0x6)");
static_assert(sizeof(IWISmallMips) == 0x10, "Invalid IWISmallMips Size (Expected 0x10)");
static_assert(sizeof(IWIBigMips) == 0x20, "Invalid IWIBigMips Size (Expected 0x20)");

std::unique_ptr<XImageDDS> CoDIWITranslator::TranslateIWI(const std::unique_ptr<uint8_t[]>& IWIBuffer, uint32_t IWIBufferSize)
{
    auto Reader = MemoryReader((int8_t*)IWIBuffer.get(), IWIBufferSize, true);

    auto Header = Reader.Read<IWIHeader>();

    // version 9 (Call of Duty Online) is laid out as 8
    const bool Iw4Layout = Header.Version == 0x8 || Header.Version == 0x9;
    if (Iw4Layout) { Reader.SetPosition(8); }

    auto Info = Reader.Read<IWIInfo>();
    // the third dimension after width and height: a volume's slices (Black Ops' 32x32x32 colour
    // LUTs), 1 for any other
    const uint32_t Depth = std::max<uint32_t>(1, Reader.Read<uint16_t>());

    // Verify magic (IWi)
    if (Header.Magic[0] == 0x49 && Header.Magic[1] == 0x57 && Header.Magic[2] == 0x69)
    {
        int32_t OffsetToDump = -1, SizeToDump = -1;

        if (Header.Version == 0x1B || Header.Version == 0x0D)
        {
            // Big mips
            if (Header.Version == 0x0D) { Reader.SetPosition(0x10); }
            if (Header.Version == 0x1B) { Reader.SetPosition(0x20); }

            auto BigMips = Reader.Read<IWIBigMips>();

            if (BigMips.MipOffset1 == BigMips.MipOffset2 || BigMips.MipOffset1 == BigMips.MipOffset8)
            {
                OffsetToDump = (int32_t)Reader.GetPosition();
            }
            else
            {
                OffsetToDump = (BigMips.MipOffset2);
            }

            SizeToDump = (int32_t)(Reader.GetLength() - OffsetToDump);
        }
        else if (Header.Version == 0x6 || Iw4Layout)
        {
            // Small mips
            if (Header.Version == 0x6) { Reader.SetPosition(0xC); }
            if (Iw4Layout) { Reader.SetPosition(0x10); }

            auto SmallMips = Reader.Read<IWISmallMips>();

            if (SmallMips.MipOffset1 == SmallMips.MipOffset2 || SmallMips.MipOffset1 == SmallMips.MipOffset4)
            {
                OffsetToDump = (int32_t)Reader.GetPosition();
            }
            else
            {
                OffsetToDump = (SmallMips.MipOffset2);
            }

            SizeToDump = (int32_t)(Reader.GetLength() - OffsetToDump);
        }

        if (OffsetToDump > -1 && SizeToDump > -1)
        {
            Reader.SetPosition(OffsetToDump);

            // A cube map: the flags byte's 0x4, but version 8/9's flags are the u32 after the
            // version and its cube is 1 << 16 (the byte IWIInfo calls flags is unused there)
            bool isCubemap = (Info.ImageFlags & (uint8_t)IWIFileFlags::IMG_FLAG_CUBEMAP) != 0;
            if (Iw4Layout)
            {
                uint32_t Flags = 0;
                std::memcpy(&Flags, IWIBuffer.get() + 4, 4);
                isCubemap = (Flags & 0x30000) == 0x10000;
            }
            const size_t PixelCount = (size_t)Info.ImageWidth * Info.ImageHeight * Depth * (isCubemap ? 6 : 1);
            const auto* Src = reinterpret_cast<const uint8_t*>(IWIBuffer.get()) + OffsetToDump;

            // The uncompressed formats as 8-bit RGBA: 1 RGBA, B, G, R, A per pixel (D3D9's A8R8G8B8)
            // in every version but Black Ops II's (27: R, G, B, A); 2 RGB, B, G, R (Call of Duty
            // Online's, Black Ops' sky cubes such as mp_firingrange's day_ft; a 24-bit DDS has no
            // DXGI format); 3 luminance and alpha, L, A; 4 luminance. Measured on the IWDs: the red
            // waypoint_x_red (WaW, version 6) and the Spanish flag pc_flag_spain (Black Ops, 13)
            // have their red third. Written as one level (each face of a cube).
            const auto Expand = [&](size_t Bpp, auto&& Pixel) -> std::unique_ptr<XImageDDS> {
                if ((size_t)SizeToDump < PixelCount * Bpp) return nullptr;
                auto Result = std::make_unique<XImageDDS>();
                auto Buffer = new int8_t[Image::GetMaximumDDSHeaderSize() + PixelCount * 4];
                uint32_t Header32 = 0;
                Image::WriteDDSHeaderToStream(Buffer, Info.ImageWidth, Info.ImageHeight, 1, ImageFormat::DDS_Standard_A8R8G8B8, Header32, isCubemap, Depth);
                auto* Dst = reinterpret_cast<uint8_t*>(Buffer + Header32);
                for (size_t P = 0; P < PixelCount; P++) Pixel(Src + P * Bpp, Dst + P * 4);
                Result->DataBuffer = Buffer;
                Result->DataSize = (uint32_t)(Header32 + PixelCount * 4);
                return Result;
            };
            switch (Info.ImageFormat)
            {
            case 0x1:
                if (Header.Version == 0x1B)
                    return Expand(4, [](const uint8_t* S, uint8_t* D) { std::memcpy(D, S, 4); });
                return Expand(4, [](const uint8_t* S, uint8_t* D) { D[0] = S[2], D[1] = S[1], D[2] = S[0], D[3] = S[3]; });
            case 0x2: return Expand(3, [](const uint8_t* S, uint8_t* D) { D[0] = S[2], D[1] = S[1], D[2] = S[0], D[3] = 0xFF; });
            case 0x3: return Expand(2, [](const uint8_t* S, uint8_t* D) { D[0] = D[1] = D[2] = S[0], D[3] = S[1]; });
            case 0x4: return Expand(1, [](const uint8_t* S, uint8_t* D) { D[0] = D[1] = D[2] = S[0], D[3] = 0xFF; });
            default: break;
            }

            // Wavelet-compressed (6 RGBA, 7 RGB, 8 luminance and alpha, 9 luminance, 10 alpha):
            // decoded by CoDIWIDecoder (versions 6, 8, 9, 13)
            if (Info.ImageFormat >= 0x6 && Info.ImageFormat <= 0xA)
            {
                CoDIWIDecoder::Rgba Decoded;
                std::string Error;
                if (!CoDIWIDecoder::LargestRgba(reinterpret_cast<const uint8_t*>(IWIBuffer.get()), IWIBufferSize, Decoded, Error)) return nullptr;
                auto Result = std::make_unique<XImageDDS>();
                auto Buffer = new int8_t[Image::GetMaximumDDSHeaderSize() + Decoded.Pixels.size()];
                uint32_t Header32 = 0;
                Image::WriteDDSHeaderToStream(Buffer, Decoded.Width, Decoded.Height, 1, ImageFormat::DDS_Standard_A8R8G8B8, Header32, Decoded.Cube);
                std::memcpy(Buffer + Header32, Decoded.Pixels.data(), Decoded.Pixels.size());
                Result->DataBuffer = Buffer;
                Result->DataSize = (uint32_t)(Header32 + Decoded.Pixels.size());
                return Result;
            }

            ImageFormat ImageDataFormat;
            switch (Info.ImageFormat)
            {
            case 0x5: ImageDataFormat = ImageFormat::DDS_Standard_A8_UNORM; break;
            case 0x0B: ImageDataFormat = ImageFormat::DDS_BC1_UNORM; break;
            case 0x0C: ImageDataFormat = ImageFormat::DDS_BC2_UNORM; break;
            case 0x0D: ImageDataFormat = ImageFormat::DDS_BC3_UNORM; break;
            case 0x0E: ImageDataFormat = ImageFormat::DDS_BC5_UNORM; break;
            // any other (Modern Warfare 3's console-only 0x17, Black Ops' RGB565...) is not
            // written: a guessed format makes a plausible-looking garbage file
            default: return nullptr;
            }

            auto Result = std::make_unique<XImageDDS>();
            auto ImageBuffer = new int8_t[Image::GetMaximumDDSHeaderSize() + SizeToDump];

            uint32_t ResultSize = 0;
            Image::WriteDDSHeaderToStream(ImageBuffer, Info.ImageWidth, Info.ImageHeight, 1, ImageDataFormat, ResultSize, isCubemap, Depth);

            Reader.Read(SizeToDump, ImageBuffer + ResultSize);

            Result->DataBuffer = ImageBuffer;
            Result->DataSize = (uint32_t)(ResultSize + SizeToDump);

            return Result;
        }
    }

    return nullptr;
}
