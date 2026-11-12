#include "stdafx.h"

#include "CoDRawImageTranslator.h"

#include "Image.h"
#include "CoDAssetType.h"

#include <cstring>
#include <utility>

std::unique_ptr<XImageDDS> CoDRawImageTranslator::TranslateBC(const std::unique_ptr<uint8_t[]>& BCBuffer, uint32_t BCBufferSize, uint32_t Width, uint32_t Height, uint8_t ImageFormat, uint8_t MipLevels, bool isCubemap, uint32_t Depth)
{
    auto ImageDataFormat = ImageFormat::DDS_BC1_UNORM;   // (the parameter hides the type: a declaration names it through ::)
    bool SwapRB = false;
    switch (ImageFormat)
    {
    case 2: ImageDataFormat = ImageFormat::DDS_Standard_R32G32B32A32_FLOAT; break;
    case 10: ImageDataFormat = ImageFormat::DDS_Standard_R16G16B16A16_FLOAT; break;
    case 24: ImageDataFormat = ImageFormat::DDS_Standard_R10G10B10A2_UNORM; break;
    case 28: ImageDataFormat = ImageFormat::DDS_Standard_A8R8G8B8; break;
    case 29: ImageDataFormat = ImageFormat::DDS_Standard_A8R8G8B8_SRGB; break;
    case 34: ImageDataFormat = ImageFormat::DDS_Standard_R16G16_FLOAT; break;
    case 56: ImageDataFormat = ImageFormat::DDS_Standard_R16_UNORM; break;   // BO3's sstMinMax_* (written as BC1 before)
    case 61: ImageDataFormat = ImageFormat::DDS_Standard_R8_UNORM; break;
    case 62: ImageDataFormat = ImageFormat::DDS_Standard_R8_UINT; break;
    case 65: ImageDataFormat = ImageFormat::DDS_Standard_A8_UNORM; break;
    // B8G8R8A8: its blue first, written as R8G8B8A8
    case 87: ImageDataFormat = ImageFormat::DDS_Standard_A8R8G8B8, SwapRB = true; break;
    case 71: ImageDataFormat = ImageFormat::DDS_BC1_UNORM; break;
    case 72: ImageDataFormat = ImageFormat::DDS_BC1_SRGB; break;
    case 74: ImageDataFormat = ImageFormat::DDS_BC2_UNORM; break;
    case 75: ImageDataFormat = ImageFormat::DDS_BC2_SRGB; break;
    case 77: ImageDataFormat = ImageFormat::DDS_BC3_UNORM; break;
    case 78: ImageDataFormat = ImageFormat::DDS_BC3_SRGB; break;
    case 80: ImageDataFormat = ImageFormat::DDS_BC4_UNORM; break;
    case 81: ImageDataFormat = ImageFormat::DDS_BC4_SNORM; break;
    case 83: ImageDataFormat = ImageFormat::DDS_BC5_UNORM; break;
    case 84: ImageDataFormat = ImageFormat::DDS_BC5_SNORM; break;
    case 95: ImageDataFormat = ImageFormat::DDS_BC6_UF16; break;
    case 96: ImageDataFormat = ImageFormat::DDS_BC6_SF16; break;
    case 98: ImageDataFormat = ImageFormat::DDS_BC7_UNORM; break;
    case 99: ImageDataFormat = ImageFormat::DDS_BC7_SRGB; break;
    // Any other is not written: these bytes under a guessed format (BC1 sRGB, before) made a
    // plausible-looking garbage file
    default: return nullptr;
    }

    auto Result = std::make_unique<XImageDDS>();
    auto ImageBuffer = new int8_t[Image::GetMaximumDDSHeaderSize() + BCBufferSize];

    uint32_t ResultSize = 0;
    Image::WriteDDSHeaderToStream(ImageBuffer, Width, Height, MipLevels, ImageDataFormat, ResultSize, isCubemap, Depth);

    std::memcpy(ImageBuffer + ResultSize, BCBuffer.get(), BCBufferSize);
    if (SwapRB)
        for (uint32_t P = 0; P + 3 < BCBufferSize; P += 4) std::swap(ImageBuffer[ResultSize + P], ImageBuffer[ResultSize + P + 2]);

    Result->DataBuffer = ImageBuffer;
    Result->DataSize = (uint32_t)(ResultSize + BCBufferSize);

    return Result;
}
