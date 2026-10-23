#include "stdafx.h"

#include "Image.h"

// A far faster PNG encoder than the one WIC ships
#include "fpng.h"

#include "Strings.h"
#include "VectorMath.h"

// DirectXTex public API only (a vcpkg install ships no private DDS.h): the DDS header is hand-written below.
#include "DirectXTex.h"

#include <cstring>

#include <wincodec.h>

namespace
{
    // Minimal DDS container structs (the documented on-disk layout).
#pragma pack(push, 1)
    struct GxDdsPixelFormat { uint32_t size, flags, fourCC, RGBBitCount, RBitMask, GBitMask, BBitMask, ABitMask; };
    struct GxDdsHeader
    {
        uint32_t size, flags, height, width, pitchOrLinearSize, depth, mipMapCount, reserved1[11];
        GxDdsPixelFormat ddspf;
        uint32_t caps, caps2, caps3, caps4, reserved2;
    };
    struct GxDdsHeaderDxt10 { uint32_t dxgiFormat, resourceDimension, miscFlag, arraySize, miscFlags2; };
#pragma pack(pop)
    constexpr uint32_t GX_DDS_MAGIC = 0x20534444u;      // "DDS "
    constexpr uint32_t GX_DDS_FOURCC_DX10 = 0x30315844u; // "DX10"
    constexpr uint32_t GX_DDS_MAX_HEADER = sizeof(uint32_t) + sizeof(GxDdsHeader) + sizeof(GxDdsHeaderDxt10);
}

bool Image::ConvertImageMemory(int8_t* ImageBuffer, uint64_t ImageSize, ImageFormat InFormat, const std::string& OutputFile, ImageFormat OutFormat, ImagePatch Patch, uint32_t MinSide)
{
    auto Image = std::make_unique<DirectX::ScratchImage>();
    DirectX::TexMetadata ImageMetadata;

    HRESULT Result = -1;

    switch (InFormat)
    {
    case ImageFormat::Standard_BMP:
    case ImageFormat::Standard_GIF:
    case ImageFormat::Standard_JPEG:
    case ImageFormat::Standard_PNG:
    case ImageFormat::Standard_TIFF:
        Result = DirectX::LoadFromWICMemory(reinterpret_cast<const uint8_t*>(ImageBuffer), (size_t)ImageSize,DirectX::WIC_FLAGS::WIC_FLAGS_NONE, &ImageMetadata, *Image);
        break;
    case ImageFormat::Standard_TGA:
        Result = DirectX::LoadFromTGAMemory(reinterpret_cast<const uint8_t*>(ImageBuffer), (size_t)ImageSize,&ImageMetadata, *Image);
        break;
    case ImageFormat::DDS_WithHeader:
        Result = DirectX::LoadFromDDSMemory(reinterpret_cast<const uint8_t*>(ImageBuffer), (size_t)ImageSize,DirectX::DDS_FLAGS::DDS_FLAGS_NONE, &ImageMetadata, *Image);
        break;
    default:
        return false;
    }

    if (FAILED(Result)) { return false; }

    auto OutputFormat = (OutFormat == ImageFormat::NoConversion) ? InFormat : OutFormat;

    return ConvertToFormat(Image, ImageMetadata, OutputFile, OutputFormat, Patch, MinSide);
}

bool Image::ConvertToFormat(std::unique_ptr<DirectX::ScratchImage>& Image, DirectX::TexMetadata ImageMetadata, const std::string& OutputFile, ImageFormat OutFormat, ImagePatch Patch, uint32_t MinSide)
{
    // Stage 1: Check if the image is planar, if so, convert to a single plane
    if (DirectX::IsPlanar(ImageMetadata.format))
    {
        auto FirstImage = Image->GetImage(0, 0, 0);
        auto ImageCount = Image->GetImageCount();
        auto TemporaryImage = std::make_unique<DirectX::ScratchImage>();

        auto Result = DirectX::ConvertToSinglePlane(FirstImage, ImageCount, ImageMetadata, *TemporaryImage);

        if (!FAILED(Result))
        {
            auto& TextureInfo = TemporaryImage->GetMetadata();
            ImageMetadata.format = TextureInfo.format;
            Image.reset(TemporaryImage.release());
        }
        else
        {
            return false;
        }
    }

    // Stage 2: Decompress the texture if necessary, or ensure it's in the proper format. An sRGB source
    // keeps its encoded values: it goes to the sRGB RGBA8 (no curve applied on the way) and its bytes are
    // then read as plain RGBA8, which the file writers store as they are. To plain RGBA8, DirectXTex
    // would decode the curve and write linear values into a PNG or TGA read as sRGB: far too dark.
    const bool Srgb = DirectX::IsSRGB(ImageMetadata.format);
    const DXGI_FORMAT Rgba8 = Srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    if (DirectX::IsCompressed(ImageMetadata.format))
    {
        auto FirstImage = Image->GetImage(0, 0, 0);
        auto ImageCount = Image->GetImageCount();
        auto TemporaryImage = std::make_unique<DirectX::ScratchImage>();

        DXGI_FORMAT ResultFormat = Rgba8;
        auto Result = DirectX::Decompress(FirstImage, ImageCount, ImageMetadata, ResultFormat, *TemporaryImage);

        if (!FAILED(Result))
        {
            auto& TextureInfo = TemporaryImage->GetMetadata();
            ImageMetadata.format = TextureInfo.format;
            Image.reset(TemporaryImage.release());
        }
        else
        {
            return false;
        }
    }
    else if (ImageMetadata.format != Rgba8)
    {
        auto TemporaryImage = std::make_unique<DirectX::ScratchImage>();

        DXGI_FORMAT ResultFormat = Rgba8;
        // FORCE_NON_WIC: WIC needs COM on the calling thread and its process-wide factory crashed the
        // export's conversion pool.
        auto Result = DirectX::Convert(Image->GetImages(), Image->GetImageCount(), Image->GetMetadata(), ResultFormat, DirectX::TEX_FILTER_FLAGS::TEX_FILTER_FORCE_NON_WIC, DirectX::TEX_THRESHOLD_DEFAULT, *TemporaryImage);

        if (!FAILED(Result))
        {
            auto& TextureInfo = TemporaryImage->GetMetadata();
            ImageMetadata.format = TextureInfo.format;
            Image.reset(TemporaryImage.release());
        }
        else
        {
            return false;
        }
    }
    if (Srgb && Image->OverrideFormat(DXGI_FORMAT_R8G8B8A8_UNORM)) ImageMetadata.format = DXGI_FORMAT_R8G8B8A8_UNORM;

    // Stage 3: Patching, apply an image patch if need be
    if (Patch != ImagePatch::NoPatch)
    {
        switch (Patch)
        {
        case ImagePatch::Normal_Bumpmap: PatchNormalFromBumpmap(Image); break;
        case ImagePatch::Normal_Expand: PatchNormalFromCompressed(Image); break;
        case ImagePatch::Normal_COD_NOG: PatchNormalCODFromNOG(Image); break;
        case ImagePatch::Color_StripAlpha: PatchAlphaChannel(Image); break;
        }
    }

    // Stage 3b: a tiny image (an engine built-in, 1x1) repeated up to MinSide, the first mip only
    if (MinSide > 0 && (ImageMetadata.width < MinSide || ImageMetadata.height < MinSide))
    {
        auto TemporaryImage = std::make_unique<DirectX::ScratchImage>();
        const size_t W = std::max<size_t>(ImageMetadata.width, MinSide), H = std::max<size_t>(ImageMetadata.height, MinSide);
        const auto Result = DirectX::Resize(*Image->GetImage(0, 0, 0), W, H,
                                            DirectX::TEX_FILTER_FLAGS::TEX_FILTER_POINT | DirectX::TEX_FILTER_FLAGS::TEX_FILTER_FORCE_NON_WIC, *TemporaryImage);
        if (FAILED(Result)) return false;
        ImageMetadata = TemporaryImage->GetMetadata();
        Image.reset(TemporaryImage.release());
    }

    // This format only applies for DDS saving
    DXGI_FORMAT ResultFormat = DXGI_FORMAT::DXGI_FORMAT_BC1_UNORM;

    // Stage 4: Recompression, if the output format requires it, DDS image formats only
    if (OutFormat > ImageFormat::DDS_WithHeader)
    {
        switch (OutFormat)
        {
        case ImageFormat::DDS_BC1_TYPELESS: ResultFormat = DXGI_FORMAT::DXGI_FORMAT_BC1_TYPELESS; break;
        case ImageFormat::DDS_BC1_UNORM: ResultFormat = DXGI_FORMAT::DXGI_FORMAT_BC1_UNORM; break;
        case ImageFormat::DDS_BC1_SRGB: ResultFormat = DXGI_FORMAT::DXGI_FORMAT_BC1_UNORM_SRGB; break;

        case ImageFormat::DDS_BC2_TYPELESS: ResultFormat = DXGI_FORMAT::DXGI_FORMAT_BC2_TYPELESS; break;
        case ImageFormat::DDS_BC2_UNORM: ResultFormat = DXGI_FORMAT::DXGI_FORMAT_BC2_UNORM; break;
        case ImageFormat::DDS_BC2_SRGB: ResultFormat = DXGI_FORMAT::DXGI_FORMAT_BC2_UNORM_SRGB; break;

        case ImageFormat::DDS_BC3_TYPELESS: ResultFormat = DXGI_FORMAT::DXGI_FORMAT_BC3_TYPELESS; break;
        case ImageFormat::DDS_BC3_UNORM: ResultFormat = DXGI_FORMAT::DXGI_FORMAT_BC3_UNORM; break;
        case ImageFormat::DDS_BC3_SRGB: ResultFormat = DXGI_FORMAT::DXGI_FORMAT_BC3_UNORM_SRGB; break;

        case ImageFormat::DDS_BC4_TYPELESS: ResultFormat = DXGI_FORMAT::DXGI_FORMAT_BC4_TYPELESS; break;
        case ImageFormat::DDS_BC4_UNORM: ResultFormat = DXGI_FORMAT::DXGI_FORMAT_BC4_UNORM; break;
        case ImageFormat::DDS_BC4_SNORM: ResultFormat = DXGI_FORMAT::DXGI_FORMAT_BC4_SNORM; break;

        case ImageFormat::DDS_BC5_TYPELESS: ResultFormat = DXGI_FORMAT::DXGI_FORMAT_BC5_TYPELESS; break;
        case ImageFormat::DDS_BC5_UNORM: ResultFormat = DXGI_FORMAT::DXGI_FORMAT_BC5_UNORM; break;
        case ImageFormat::DDS_BC5_SNORM: ResultFormat = DXGI_FORMAT::DXGI_FORMAT_BC5_SNORM; break;

        case ImageFormat::DDS_BC6_TYPELESS: ResultFormat = DXGI_FORMAT::DXGI_FORMAT_BC6H_TYPELESS; break;
        case ImageFormat::DDS_BC6_UF16: ResultFormat = DXGI_FORMAT::DXGI_FORMAT_BC6H_UF16; break;
        case ImageFormat::DDS_BC6_SF16: ResultFormat = DXGI_FORMAT::DXGI_FORMAT_BC6H_SF16; break;

        case ImageFormat::DDS_BC7_TYPELESS: ResultFormat = DXGI_FORMAT::DXGI_FORMAT_BC7_TYPELESS; break;
        case ImageFormat::DDS_BC7_UNORM: ResultFormat = DXGI_FORMAT::DXGI_FORMAT_BC7_UNORM; break;
        case ImageFormat::DDS_BC7_SRGB: ResultFormat = DXGI_FORMAT::DXGI_FORMAT_BC7_UNORM_SRGB; break;

        case ImageFormat::DDS_Standard_R8G8B8A8: ResultFormat = DXGI_FORMAT::DXGI_FORMAT_R8G8B8A8_TYPELESS; break;
        case ImageFormat::DDS_Standard_R8G8B8A8_SRGB: ResultFormat = DXGI_FORMAT::DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; break;
        }

        if (DirectX::IsCompressed(ResultFormat))
        {
            auto FirstImage = Image->GetImage(0, 0, 0);
            auto ImageCount = Image->GetImageCount();
            auto TemporaryImage = std::make_unique<DirectX::ScratchImage>();

            // TODO: Replace 0 with proper flags
            auto Result = DirectX::Compress(
                FirstImage,
                ImageCount,
                ImageMetadata,
                ResultFormat,
                DirectX::TEX_COMPRESS_DEFAULT,
                DirectX::TEX_THRESHOLD_DEFAULT,
                *TemporaryImage);

            if (!FAILED(Result))
            {
                auto& TextureInfo = TemporaryImage->GetMetadata();
                ImageMetadata.format = TextureInfo.format;
                Image.reset(TemporaryImage.release());
            }
            else
            {
                return false;
            }
        }
    }

    // Stage 5: Saving to a file
    {
        auto FirstImage = Image->GetImage(0, 0, 0);
        auto ImageCount = 1;

        if (OutFormat > ImageFormat::DDS_WithHeader)
        {
            DirectX::SaveToDDSFile(FirstImage, ImageCount, ImageMetadata, DirectX::DDS_FLAGS::DDS_FLAGS_NONE, Strings::ToUnicodeString(OutputFile).c_str());
        }
        else
        {
            switch (OutFormat)
            {
            case ImageFormat::Standard_TGA:
                DirectX::SaveToTGAFile(FirstImage[0], Strings::ToUnicodeString(OutputFile).c_str());
                break;
            case ImageFormat::Standard_BMP:
                DirectX::SaveToWICFile(FirstImage, ImageCount, DirectX::WIC_FLAGS::WIC_FLAGS_NONE, DirectX::GetWICCodec(DirectX::WICCodecs::WIC_CODEC_BMP), Strings::ToUnicodeString(OutputFile).c_str(), nullptr, nullptr);
                break;
            case ImageFormat::Standard_GIF:
                DirectX::SaveToWICFile(FirstImage, ImageCount, DirectX::WIC_FLAGS::WIC_FLAGS_NONE, DirectX::GetWICCodec(DirectX::WICCodecs::WIC_CODEC_GIF), Strings::ToUnicodeString(OutputFile).c_str(), nullptr, nullptr);
                break;
            case ImageFormat::Standard_PNG:
            {
                // WIC's PNG encoder dominates export cost, so use fpng when the layout fits,
                // falling back to WIC otherwise. Static init below runs once, thread-safe.
                static const bool FpngReady = []
                {
                    fpng::fpng_init();
                    return true;
                }();
                (void)FpngReady;

                bool Encoded = false;

                if (FirstImage[0].format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                    FirstImage[0].format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
                {
                    // fpng wants tightly packed rows, DirectXTex may have padded them
                    const size_t TightPitch = (size_t)FirstImage[0].width * 4;

                    if (FirstImage[0].rowPitch == TightPitch)
                    {
                        Encoded = fpng::fpng_encode_image_to_file(
                            OutputFile.c_str(),
                            FirstImage[0].pixels,
                            (uint32_t)FirstImage[0].width,
                            (uint32_t)FirstImage[0].height,
                            4);
                    }
                    else
                    {
                        std::vector<uint8_t> Packed(TightPitch * FirstImage[0].height);

                        for (size_t Row = 0; Row < FirstImage[0].height; Row++)
                        {
                            std::memcpy(
                                Packed.data() + (Row * TightPitch),
                                FirstImage[0].pixels + (Row * FirstImage[0].rowPitch),
                                TightPitch);
                        }

                        Encoded = fpng::fpng_encode_image_to_file(
                            OutputFile.c_str(),
                            Packed.data(),
                            (uint32_t)FirstImage[0].width,
                            (uint32_t)FirstImage[0].height,
                            4);
                    }
                }

                if (!Encoded)
                {
                    DirectX::SaveToWICFile(FirstImage, ImageCount, DirectX::WIC_FLAGS::WIC_FLAGS_NONE, DirectX::GetWICCodec(DirectX::WICCodecs::WIC_CODEC_PNG), Strings::ToUnicodeString(OutputFile).c_str(), nullptr, nullptr);
                }
            }
                break;
            case ImageFormat::Standard_JPEG:
                DirectX::SaveToWICFile(FirstImage, ImageCount, DirectX::WIC_FLAGS::WIC_FLAGS_NONE, DirectX::GetWICCodec(DirectX::WICCodecs::WIC_CODEC_JPEG), Strings::ToUnicodeString(OutputFile).c_str(), nullptr, [&](IPropertyBag2* props)
                {
                    PROPBAG2 options = {};
                    VARIANT varValues = {};
                    options.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
                    varValues.vt = VT_R4;
                    varValues.fltVal = 1.f;
                    (void)props->Write(1, &options, &varValues);
                });
                break;
            case ImageFormat::Standard_TIFF:
                DirectX::SaveToWICFile(FirstImage, ImageCount, DirectX::WIC_FLAGS::WIC_FLAGS_NONE, DirectX::GetWICCodec(DirectX::WICCodecs::WIC_CODEC_TIFF), Strings::ToUnicodeString(OutputFile).c_str(), nullptr, [&](IPropertyBag2* props)
                {
                    PROPBAG2 options = {};
                    VARIANT varValues = {};
                    options.pstrName = const_cast<LPOLESTR>(L"TiffCompressionMethod");
                    varValues.vt = VT_UI1;
                    varValues.bVal = WICTiffCompressionNone;
                    (void)props->Write(1, &options, &varValues);
                });
                break;
            default:
                return false;
            }
        }
    }

    return true;
}

namespace
{
    // Replaces Image by Pixel applied to each of its pixels (kept when the transform fails)
    template <typename FPixel>
    void TransformPixels(std::unique_ptr<DirectX::ScratchImage>& Image, FPixel Pixel)
    {
        auto TemporaryImage = std::make_unique<DirectX::ScratchImage>();
        auto Result = DirectX::TransformImage(Image->GetImages(), Image->GetImageCount(), Image->GetMetadata(), [&](DirectX::XMVECTOR* outPixels, const DirectX::XMVECTOR* inPixels, size_t width, size_t y)
        {
            UNREFERENCED_PARAMETER(y);
            for (size_t j = 0; j < width; j++)
            {
                outPixels[j] = Pixel(inPixels[j]);
            }
        }, *TemporaryImage);
        if (!FAILED(Result))
        {
            Image.reset(TemporaryImage.release());
        }
    }

    // A normal's Z as a channel value, from its X and Y channels
    float NormalZ(float RedVal, float GreenVal)
    {
        float nx = 2 * RedVal - 1;
        float ny = 2 * GreenVal - 1;
        float nz = 0.0f;

        if (1 - nx * nx - ny * ny > 0) nz = std::sqrtf(1 - nx * nx - ny * ny);

        return VectorMath::Clamp<float>(((nz + 1) / 2.0f), 0, 1.0);
    }
}

void Image::PatchNormalFromBumpmap(std::unique_ptr<DirectX::ScratchImage>& Image)
{
    // G and A as the normal's X and Y, Z as 1.0: closer than most reconstructions
    TransformPixels(Image, [](DirectX::XMVECTOR scanline)
    {
        float GreenVal = DirectX::XMVectorGetY(scanline);
        float AlphaVal = DirectX::XMVectorGetW(scanline);

        scanline = DirectX::XMVectorSetX(scanline, AlphaVal);
        scanline = DirectX::XMVectorSetY(scanline, GreenVal);
        scanline = DirectX::XMVectorSetZ(scanline, 1.0);
        scanline = DirectX::XMVectorSetW(scanline, 1.0);
        return scanline;
    });
}

void Image::PatchNormalFromCompressed(std::unique_ptr<DirectX::ScratchImage>& Image)
{
    TransformPixels(Image, [](DirectX::XMVECTOR scanline)
    {
        return DirectX::XMVectorSetZ(scanline, NormalZ(DirectX::XMVectorGetX(scanline), DirectX::XMVectorGetY(scanline)));
    });
}

void Image::PatchNormalCODFromNOG(std::unique_ptr<DirectX::ScratchImage>& Image)
{
    // A COD NOG normal map (G: X, A: Y) as a traditional one (XYZ), alpha 1.0
    TransformPixels(Image, [](DirectX::XMVECTOR scanline)
    {
        float RedVal = DirectX::XMVectorGetY(scanline);
        float GreenVal = DirectX::XMVectorGetW(scanline);

        scanline = DirectX::XMVectorSetX(scanline, RedVal);
        scanline = DirectX::XMVectorSetY(scanline, GreenVal);
        scanline = DirectX::XMVectorSetZ(scanline, NormalZ(RedVal, GreenVal));
        scanline = DirectX::XMVectorSetW(scanline, 1.0);
        return scanline;
    });
}

void Image::PatchAlphaChannel(std::unique_ptr<DirectX::ScratchImage>& Image)
{
    TransformPixels(Image, [](DirectX::XMVECTOR scanline) { return DirectX::XMVectorSetW(scanline, 1.0); });
}

void Image::WriteDDSHeaderToStream(int8_t* Buffer, uint32_t Width, uint32_t Height, uint32_t MipLevels, ImageFormat Format, uint32_t& ResultSize, bool isCubemap)
{
    ResultSize = 0;

    auto HeaderBuffer = BuildDDSHeader(Width, Height, MipLevels, Format, ResultSize, isCubemap);

    if (HeaderBuffer != nullptr && ResultSize > 0)
    {
        std::memcpy(Buffer, HeaderBuffer.get(), ResultSize);
    }
}

std::unique_ptr<int8_t[]> Image::BuildDDSHeader(uint32_t Width, uint32_t Height, uint32_t MipLevels, ImageFormat Format, uint32_t& ResultSize, bool isCubemap)
{
    uint32_t MaximumSize = GX_DDS_MAX_HEADER;
    auto HeaderBuffer = std::make_unique<int8_t[]>(MaximumSize);

    DirectX::TexMetadata Metadata;
    Metadata.width = Width;
    Metadata.height = Height;
    Metadata.depth = 1;
    Metadata.arraySize = (isCubemap) ? 6 : 1;
    Metadata.mipLevels = MipLevels;
    Metadata.miscFlags = (isCubemap) ? DirectX::TEX_MISC_FLAG::TEX_MISC_TEXTURECUBE: 0;
    Metadata.miscFlags2 = 0;
    Metadata.dimension = DirectX::TEX_DIMENSION::TEX_DIMENSION_TEXTURE2D;

    bool UseAlphaValue = true;

    switch (Format)
    {
    case ImageFormat::DDS_BC1_UNORM:
    case ImageFormat::DDS_BC1_TYPELESS:
        Metadata.format = DXGI_FORMAT::DXGI_FORMAT_BC1_UNORM;
        break;
    case ImageFormat::DDS_BC1_SRGB: Metadata.format = DXGI_FORMAT::DXGI_FORMAT_BC1_UNORM_SRGB; break;

    case ImageFormat::DDS_BC2_UNORM:
    case ImageFormat::DDS_BC2_TYPELESS:
        Metadata.format = DXGI_FORMAT::DXGI_FORMAT_BC2_UNORM;
        break;
    case ImageFormat::DDS_BC2_SRGB: Metadata.format = DXGI_FORMAT::DXGI_FORMAT_BC2_UNORM_SRGB; break;

    case ImageFormat::DDS_BC3_UNORM:
    case ImageFormat::DDS_BC3_TYPELESS:
        Metadata.format = DXGI_FORMAT::DXGI_FORMAT_BC3_UNORM;
        break;
    case ImageFormat::DDS_BC3_SRGB: Metadata.format = DXGI_FORMAT::DXGI_FORMAT_BC3_UNORM_SRGB; break;

    case ImageFormat::DDS_BC4_UNORM:
    case ImageFormat::DDS_BC4_TYPELESS:
        Metadata.format = DXGI_FORMAT::DXGI_FORMAT_BC4_UNORM;
        break;
    case ImageFormat::DDS_BC4_SNORM: Metadata.format = DXGI_FORMAT::DXGI_FORMAT_BC4_SNORM; break;

    case ImageFormat::DDS_BC5_UNORM:
    case ImageFormat::DDS_BC5_TYPELESS:
        Metadata.format = DXGI_FORMAT::DXGI_FORMAT_BC5_UNORM;
        break;
    case ImageFormat::DDS_BC5_SNORM: Metadata.format = DXGI_FORMAT::DXGI_FORMAT_BC5_SNORM; break;

    case ImageFormat::DDS_BC6_UF16:
    case ImageFormat::DDS_BC6_TYPELESS:
        Metadata.format = DXGI_FORMAT::DXGI_FORMAT_BC6H_UF16;
        break;
    case ImageFormat::DDS_BC6_SF16: Metadata.format = DXGI_FORMAT::DXGI_FORMAT_BC6H_SF16; break;

    case ImageFormat::DDS_BC7_UNORM:
    case ImageFormat::DDS_BC7_TYPELESS:
        Metadata.format = DXGI_FORMAT::DXGI_FORMAT_BC7_UNORM;
        break;
    case ImageFormat::DDS_BC7_SRGB: Metadata.format = DXGI_FORMAT::DXGI_FORMAT_BC7_UNORM_SRGB; break;

    case ImageFormat::DDS_Standard_R8G8B8A8:
    case ImageFormat::DDS_Standard_A8R8G8B8:
        Metadata.format = DXGI_FORMAT::DXGI_FORMAT_R8G8B8A8_UNORM;
        break;

    case ImageFormat::DDS_Standard_R8G8B8:
        Metadata.format = DXGI_FORMAT::DXGI_FORMAT_R8G8B8A8_UNORM;
        UseAlphaValue = false;
        break;

    case ImageFormat::DDS_Standard_A8R8G8B8_SRGB:
    case ImageFormat::DDS_Standard_R8G8B8A8_SRGB:
        Metadata.format = DXGI_FORMAT::DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        break;

    case ImageFormat::DDS_Standard_R9G9B9E5: Metadata.format = DXGI_FORMAT::DXGI_FORMAT_R9G9B9E5_SHAREDEXP; break;
    case ImageFormat::DDS_Standard_R16G16B16A16_UNORM: Metadata.format = DXGI_FORMAT::DXGI_FORMAT_R16G16B16A16_UNORM; break;
    case ImageFormat::DDS_Standard_R16G16B16A16_FLOAT: Metadata.format = DXGI_FORMAT::DXGI_FORMAT_R16G16B16A16_FLOAT; break;
    case ImageFormat::DDS_Standard_R32G32B32A32_UNORM: Metadata.format = DXGI_FORMAT::DXGI_FORMAT_R32G32B32A32_UINT; break;
    case ImageFormat::DDS_Standard_R32G32B32A32_FLOAT: Metadata.format = DXGI_FORMAT::DXGI_FORMAT_R32G32B32A32_FLOAT; break;
    case ImageFormat::DDS_Standard_R10G10B10A2_UNORM: Metadata.format = DXGI_FORMAT::DXGI_FORMAT_R10G10B10A2_UNORM; break;
    case ImageFormat::DDS_Standard_D16_UNORM: Metadata.format = DXGI_FORMAT::DXGI_FORMAT_D16_UNORM; break;
    case ImageFormat::DDS_Standard_A8_UNORM: Metadata.format = DXGI_FORMAT::DXGI_FORMAT_A8_UNORM; break;
    case ImageFormat::DDS_Standard_R8_UNORM: Metadata.format = DXGI_FORMAT::DXGI_FORMAT_R8_UNORM; break;
    case ImageFormat::DDS_Standard_R8_UINT: Metadata.format = DXGI_FORMAT::DXGI_FORMAT_R8_UINT; break;

    default:
        Metadata.format = DXGI_FORMAT::DXGI_FORMAT_BC1_UNORM;
        break;
    }

    // A DX10-extended DDS header carries any DXGI format without per-format pixel-format masks.
    int8_t* P = HeaderBuffer.get();
    std::memset(P, 0, MaximumSize);
    *reinterpret_cast<uint32_t*>(P) = GX_DDS_MAGIC;

    auto* Hdr = reinterpret_cast<GxDdsHeader*>(P + sizeof(uint32_t));
    Hdr->size = sizeof(GxDdsHeader);                          // 124
    Hdr->flags = 0x1007u | (MipLevels > 1 ? 0x20000u : 0u);   // CAPS|HEIGHT|WIDTH|PIXELFORMAT [|MIPMAPCOUNT]
    Hdr->height = Height;
    Hdr->width = Width;
    Hdr->mipMapCount = MipLevels;
    Hdr->ddspf.size = sizeof(GxDdsPixelFormat);               // 32
    Hdr->ddspf.flags = 0x4u;                                  // DDPF_FOURCC
    Hdr->ddspf.fourCC = GX_DDS_FOURCC_DX10;
    Hdr->caps = 0x1000u | (MipLevels > 1 ? 0x400008u : 0u) | (isCubemap ? 0x8u : 0u); // TEXTURE [|MIPMAP|COMPLEX]
    Hdr->caps2 = isCubemap ? 0xFE00u : 0u;                    // DDS_CUBEMAP_ALLFACES

    auto* Dx10 = reinterpret_cast<GxDdsHeaderDxt10*>(P + sizeof(uint32_t) + sizeof(GxDdsHeader));
    Dx10->dxgiFormat = (uint32_t)Metadata.format;
    Dx10->resourceDimension = 3u;                             // TEXTURE2D
    Dx10->miscFlag = isCubemap ? 0x4u : 0u;                   // TEXTURECUBE
    Dx10->arraySize = 1u;
    Dx10->miscFlags2 = 0u;

    ResultSize = MaximumSize;
    return HeaderBuffer;
}

const uint32_t Image::GetMaximumDDSHeaderSize()
{
    return GX_DDS_MAX_HEADER;
}

void Image::SetupConversionThread()
{
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
}

void Image::DisableConversionThread()
{
    CoUninitialize();
}

void Image::InitializeImageAPI()
{
    // Just make sure we are initialized on the main thread, after COM
    bool iswic2 = true;
    (VOID)DirectX::GetWICFactory(iswic2);
}

void Image::ShutdownImageAPI()
{
    DirectX::SetWICFactory(nullptr);
}