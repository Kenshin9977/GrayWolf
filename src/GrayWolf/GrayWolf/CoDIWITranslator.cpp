#include "stdafx.h"

#include "CoDIWITranslator.h"

#include "Image.h"
#include "MemoryReader.h"
#include "CoDAssetType.h"

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

            auto ImageDataFormat = ImageFormat::DDS_BC1_SRGB;
            switch (Info.ImageFormat)
            {
            case 0x1:
                ImageDataFormat = ImageFormat::DDS_Standard_A8R8G8B8;
                break;
            case 0x2:
                ImageDataFormat = ImageFormat::DDS_Standard_R8G8B8;
                break;
            case 0x3:
                ImageDataFormat = ImageFormat::DDS_Standard_D16_UNORM;
                break;
            case 0x4:
            case 0x5:
                ImageDataFormat = ImageFormat::DDS_Standard_A8_UNORM;
                break;
            case 0x0E:
                ImageDataFormat = ImageFormat::DDS_BC5_UNORM;
                break;
            case 0x0D:
                ImageDataFormat = ImageFormat::DDS_BC3_UNORM;
                break;
            case 0x0B:
                ImageDataFormat = ImageFormat::DDS_BC1_UNORM;
                break;
            case 0x0C:
                ImageDataFormat = ImageFormat::DDS_BC2_UNORM;
                break;
            default:
#ifdef _DEBUG
                printf("CoDIWITranslator(): Unknown format: %d\n", Info.ImageFormat);
#endif
                break;
            }

            bool isCubemap = false;

            if ((Info.ImageFlags & (uint8_t)IWIFileFlags::IMG_FLAG_CUBEMAP) == (uint8_t)IWIFileFlags::IMG_FLAG_CUBEMAP)
                isCubemap = true;

            auto Result = std::make_unique<XImageDDS>();

            // 24-bit bitmaps (B, G, R per pixel: Call of Duty Online's, Black Ops' sky cubes such
            // as mp_firingrange's day_ft, a face after another): widened to RGBA, which the DDS
            // writers and converters know (a 24-bit DDS has no DXGI format; written as 32-bit,
            // its pixels fell short and the PNG was not written)
            if (Info.ImageFormat == 0x2)
            {
                const size_t PixelCount = (size_t)Info.ImageWidth * Info.ImageHeight * (isCubemap ? 6 : 1);
                if ((size_t)SizeToDump < PixelCount * 3)
                    return nullptr;
                auto Buffer = new int8_t[Image::GetMaximumDDSHeaderSize() + PixelCount * 4];
                uint32_t Header32 = 0;
                Image::WriteDDSHeaderToStream(Buffer, Info.ImageWidth, Info.ImageHeight, 1, ImageFormat::DDS_Standard_A8R8G8B8, Header32, isCubemap);
                const auto* Src = reinterpret_cast<const uint8_t*>(IWIBuffer.get()) + OffsetToDump;
                auto* Dst = reinterpret_cast<uint8_t*>(Buffer + Header32);
                for (size_t P = 0; P < PixelCount; P++)
                {
                    Dst[4 * P + 0] = Src[3 * P + 2];
                    Dst[4 * P + 1] = Src[3 * P + 1];
                    Dst[4 * P + 2] = Src[3 * P + 0];
                    Dst[4 * P + 3] = 0xFF;
                }
                Result->DataBuffer = Buffer;
                Result->DataSize = (uint32_t)(Header32 + PixelCount * 4);
                return Result;
            }

            auto ImageBuffer = new int8_t[Image::GetMaximumDDSHeaderSize() + SizeToDump];

            uint32_t ResultSize = 0;
            Image::WriteDDSHeaderToStream(ImageBuffer, Info.ImageWidth, Info.ImageHeight, 1, ImageDataFormat, ResultSize, isCubemap);

            Reader.Read(SizeToDump, ImageBuffer + ResultSize);

            // TODO: use DXGI_FORMAT directly instead of swapping channels here - fine for now
            // given how limited this case is.
            if (Iw4Layout && Info.ImageFormat == 0x1)
            {
                // Convert from BGRA to RGBA
                auto Pixels = ImageBuffer + ResultSize;
                auto PixelCount = Info.ImageWidth * Info.ImageHeight;

                for (size_t Pixel = 0; Pixel < PixelCount; Pixel++)
                {
                    // 32Bbp, verify we're not outside the bounds
                    auto PixelOffset = Pixel * 4;

                    if (PixelOffset + 4 > (size_t)SizeToDump)
                    {
                        break;
                    }

                    auto NewR = Pixels[PixelOffset + 2];
                    auto NewG = Pixels[PixelOffset + 1];
                    auto NewB = Pixels[PixelOffset + 0];
                    auto NewA = Pixels[PixelOffset + 3];

                    Pixels[PixelOffset + 0] = NewR;
                    Pixels[PixelOffset + 1] = NewG;
                    Pixels[PixelOffset + 2] = NewB;
                    Pixels[PixelOffset + 3] = NewA;
                }
            }

            Result->DataBuffer = ImageBuffer;
            Result->DataSize = (uint32_t)(ResultSize + SizeToDump);

            return Result;
        }
    }

    return nullptr;
}