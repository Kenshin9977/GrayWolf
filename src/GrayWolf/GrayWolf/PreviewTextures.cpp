#include "stdafx.h"

#include "PreviewTextures.h"
#include "ModelPreview.h"      // ScratchImageToRGBA8
#include "ExportNaming.h"      // Lower
#include "Strings.h"

#include "DirectXTex.h"
#include <directxtk/DDSTextureLoader.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <filesystem>
#include <fstream>


using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{
    constexpr size_t MaxSize = 4096;  // larger images are scaled down first: a preview never needs more

    bool ReadAll(const std::string& Path, std::vector<uint8_t>& Out)
    {
        std::ifstream In(Strings::ToUnicodeString(Path), std::ios::binary | std::ios::ate);
        if (!In) return false;
        const std::streamsize Size = In.tellg();
        if (Size <= 0) return false;
        In.seekg(0);
        Out.resize((size_t)Size);
        return (bool)In.read(reinterpret_cast<char*>(Out.data()), Size);
    }

    // The alpha masks something: at least 2% of the (sampled) texels are clear and 80% are either
    // clear or opaque. A smooth alpha (IW games keep other data there) is not a mask.
    bool AlphaUsedIn(const Image& Img)
    {
        if (Img.format != DXGI_FORMAT_R8G8B8A8_UNORM && Img.format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) return false;
        const size_t Step = std::max<size_t>(1, (Img.width * Img.height) / 65536);
        size_t Seen = 0, Clear = 0, Solid = 0;
        for (size_t y = 0; y < Img.height; y++)
            for (size_t x = (y * 7) % Step; x < Img.width; x += Step)
            {
                const uint8_t A = Img.pixels[y * Img.rowPitch + x * 4 + 3];
                Seen++;
                Clear += A < 26;
                Solid += A > 229;
            }
        return Seen && Clear * 50 >= Seen && (Clear + Solid) * 5 >= Seen * 4;
    }

    // A signed two-channel map (Ghosts' BC5_SNORM normals) as R8G8_UNORM, -1..1 kept as 0..1.
    bool SignedToUnsigned(ScratchImage& Img)
    {
        ScratchImage Snorm, Unorm;
        const TexMetadata& M = Img.GetMetadata();
        if (M.format == DXGI_FORMAT_R8G8_SNORM) Snorm = std::move(Img);
        else if (FAILED(Decompress(Img.GetImages(), Img.GetImageCount(), M, DXGI_FORMAT_R8G8_SNORM, Snorm))) return false;
        TexMetadata Um = Snorm.GetMetadata();
        Um.format = DXGI_FORMAT_R8G8_UNORM;
        if (FAILED(Unorm.Initialize(Um))) return false;
        const uint8_t* In = Snorm.GetPixels();
        uint8_t* Out = Unorm.GetPixels();
        for (size_t i = 0; i < Unorm.GetPixelsSize() && i < Snorm.GetPixelsSize(); i++)
            Out[i] = (uint8_t)std::clamp((int)std::lround((std::max((int)(int8_t)In[i], -127) + 127) * 255.0 / 254.0), 0, 255);
        Img = std::move(Unorm);
        return true;
    }

    // Decompresses or converts to a plain 8-bit format, keeping the data's own colour space:
    // BC4 to R8, BC5 to R8G8, the rest to RGBA8 (sRGB data stays sRGB-encoded, tagged UNORM).
    bool ToPlain(ScratchImage& Img)
    {
        const TexMetadata& M = Img.GetMetadata();
        DXGI_FORMAT Want = DXGI_FORMAT_R8G8B8A8_UNORM;
        switch (M.format)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8G8_UNORM: return true;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return Img.OverrideFormat(DXGI_FORMAT_R8G8B8A8_UNORM);
        case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_TYPELESS: Want = DXGI_FORMAT_R8_UNORM; break;
        case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_TYPELESS: Want = DXGI_FORMAT_R8G8_UNORM; break;
        case DXGI_FORMAT_BC5_SNORM: case DXGI_FORMAT_R8G8_SNORM: return SignedToUnsigned(Img);
        default: if (IsSRGB(M.format)) Want = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; break;
        }
        ScratchImage Work;
        HRESULT Hr = IsCompressed(M.format)
            ? Decompress(Img.GetImages(), Img.GetImageCount(), M, Want, Work)
            : Convert(Img.GetImages(), Img.GetImageCount(), M, Want, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT, Work);
        if (FAILED(Hr)) return false;
        if (Want == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) Work.OverrideFormat(DXGI_FORMAT_R8G8B8A8_UNORM);
        Img = std::move(Work);
        return true;
    }

    // The first level of Img, capped to Cap, with a full box-filtered mip chain.
    bool WithMips(ScratchImage& Img, PreviewTexUse Use, size_t Cap = MaxSize)
    {
        const Image* Top = Img.GetImage(0, 0, 0);
        if (!Top) return false;
        // halved until within Cap (DirectXTex's box filter only halves: 2048 to 512 is two steps)
        ScratchImage Scaled[2];
        for (int Step = 0; std::max(Top->width, Top->height) > Cap; Step ^= 1)
        {
            const size_t W = std::max<size_t>(1, Top->width / 2), H = std::max<size_t>(1, Top->height / 2);
            const bool Exact = W * 2 == Top->width && H * 2 == Top->height;
            if (FAILED(Resize(*Top, W, H, (Exact ? TEX_FILTER_BOX : TEX_FILTER_LINEAR) | TEX_FILTER_FORCE_NON_WIC, Scaled[Step]))) return false;
            Top = Scaled[Step].GetImage(0, 0, 0);
        }
        TEX_FILTER_FLAGS F = TEX_FILTER_BOX | TEX_FILTER_FORCE_NON_WIC;
        if (Use == PreviewTexUse::Color) F |= TEX_FILTER_SRGB;
        ScratchImage Mips;
        if (Top->width == 1 && Top->height == 1)
        {
            if (FAILED(Mips.InitializeFromImage(*Top))) return false;
        }
        else if (FAILED(GenerateMipMaps(*Top, F, 0, Mips))) return false;
        Img = std::move(Mips);
        return true;
    }

    bool Load(const std::vector<uint8_t>& Bytes, const std::string& Ext, ScratchImage& Img)
    {
        HRESULT Hr = E_FAIL;
        if (Ext == ".dds") Hr = LoadFromDDSMemory(Bytes.data(), Bytes.size(), DDS_FLAGS_NONE, nullptr, Img);
        else if (Ext == ".tga") Hr = LoadFromTGAMemory(Bytes.data(), Bytes.size(), nullptr, Img);
        else Hr = LoadFromWICMemory(Bytes.data(), Bytes.size(), WIC_FLAGS_IGNORE_SRGB, nullptr, Img);
        return SUCCEEDED(Hr);
    }

    // The mips of a block-compressed 2D DDS no larger than Cap, read alone (the larger levels are
    // skipped on disk, not read) and written back as a DDS of their own. False when the file is
    // not such a DDS, has no mip within Cap, or none above it (then it is read whole).
    bool ReadDdsMipsWithin(const std::string& Path, size_t Cap, std::vector<uint8_t>& Out, TexMetadata& Meta)
    {
        std::ifstream In(Strings::ToUnicodeString(Path), std::ios::binary | std::ios::ate);
        if (!In) return false;
        const size_t FileSize = (size_t)(std::streamoff)In.tellg();
        uint8_t Head[148] = {};
        if (FileSize < 128) return false;
        const size_t HeadRead = std::min(sizeof(Head), FileSize);
        In.seekg(0);
        if (!In.read(reinterpret_cast<char*>(Head), (std::streamsize)HeadRead)) return false;
        if (FAILED(GetMetadataFromDDSMemory(Head, HeadRead, DDS_FLAGS_NONE, Meta))) return false;
        if (Meta.dimension != TEX_DIMENSION_TEXTURE2D || Meta.arraySize != 1 || Meta.depth != 1 || Meta.IsCubemap() ||
            Meta.mipLevels < 2 || !IsCompressed(Meta.format) || Meta.format == DXGI_FORMAT_BC5_SNORM ||
            std::max(Meta.width, Meta.height) <= Cap) return false;
        const size_t HeadSize = std::memcmp(Head + 84, "DX10", 4) == 0 ? 148 : 128;
        size_t Skip = 0, First = 0;
        for (; First < Meta.mipLevels && std::max(Meta.width >> First, Meta.height >> First) > Cap; First++)
        {
            size_t Row = 0, Slice = 0;
            if (FAILED(ComputePitch(Meta.format, std::max<size_t>(1, Meta.width >> First), std::max<size_t>(1, Meta.height >> First), Row, Slice)))
                return false;
            Skip += Slice;
        }
        if (First >= Meta.mipLevels) return false;
        if (HeadSize + Skip >= FileSize) return false;
        TexMetadata Small = Meta;
        Small.width = std::max<size_t>(1, Meta.width >> First);
        Small.height = std::max<size_t>(1, Meta.height >> First);
        Small.mipLevels = Meta.mipLevels - First;
        size_t Rest = 0;
        for (size_t L = 0; L < Small.mipLevels; L++)
        {
            size_t Row = 0, Slice = 0;
            if (FAILED(ComputePitch(Small.format, std::max<size_t>(1, Small.width >> L), std::max<size_t>(1, Small.height >> L), Row, Slice)))
                return false;
            Rest += Slice;
        }
        if (HeadSize + Skip + Rest > FileSize) return false;
        size_t NewHead = 0;
        if (FAILED(EncodeDDSHeader(Small, DDS_FLAGS_NONE, nullptr, 0, NewHead))) return false;
        Out.resize(NewHead + Rest);
        if (FAILED(EncodeDDSHeader(Small, DDS_FLAGS_NONE, Out.data(), NewHead, NewHead))) return false;
        In.seekg((std::streamoff)(HeadSize + Skip));
        if (!In.read(reinterpret_cast<char*>(Out.data() + NewHead), (std::streamsize)Rest)) return false;
        Meta = Small;
        return true;
    }

    // Blocks decode alone: every Step-th block row and column of a block-compressed image, as a
    // smaller image of the same format.
    bool SampleBlocks(const Image& Top, size_t Step, ScratchImage& Out)
    {
        const size_t Bw = (Top.width + 3) / 4, Bh = (Top.height + 3) / 4, BlockBytes = BitsPerPixel(Top.format) * 2;
        const size_t Sw = (Bw + Step - 1) / Step, Sh = (Bh + Step - 1) / Step;
        if (FAILED(Out.Initialize2D(Top.format, Sw * 4, Sh * 4, 1, 1))) return false;
        const Image* To = Out.GetImage(0, 0, 0);
        for (size_t y = 0; y < Sh; y++)
            for (size_t x = 0; x < Sw; x++)
                std::memcpy(To->pixels + y * To->rowPitch + x * BlockBytes,
                            Top.pixels + (y * Step) * Top.rowPitch + (x * Step) * BlockBytes, BlockBytes);
        return true;
    }
}

bool PreparePreviewImage(const std::string& Path, PreviewTexUse Use, float Cutoff, PreviewImage& Out, bool* OutAlphaUsed,
                         int MaxTexture)
{
    Out = PreviewImage{};
    if (OutAlphaUsed) *OutAlphaUsed = false;
    const size_t Cap = MaxTexture > 0 ? std::min(MaxSize, (size_t)MaxTexture) : MaxSize;

    // Capped: a mipped DDS's levels within the cap are read alone, uploaded as they are.
    if (MaxTexture > 0 && Cutoff < 0 && !OutAlphaUsed)
    {
        std::vector<uint8_t> Small;
        TexMetadata Meta{};
        if (ReadDdsMipsWithin(Path, Cap, Small, Meta))
        {
            Out.Width = (int)Meta.width; Out.Height = (int)Meta.height;
            Out.Dds = std::make_shared<const std::vector<uint8_t>>(std::move(Small));
            return true;
        }
    }

    std::vector<uint8_t> Bytes;
    if (!ReadAll(Path, Bytes)) return false;
    const std::string Ext = ExportNaming::Lower(std::filesystem::path(Path).extension().string());

    // A DDS with its whole mip chain goes to the GPU as it is; its alpha is judged on a small level.
    TexMetadata Meta{};
    if (Ext == ".dds" && SUCCEEDED(GetMetadataFromDDSMemory(Bytes.data(), Bytes.size(), DDS_FLAGS_NONE, Meta)) &&
        Meta.mipLevels > 1 && Meta.dimension == TEX_DIMENSION_TEXTURE2D && Meta.arraySize == 1 &&
        Meta.format != DXGI_FORMAT_BC5_SNORM && Meta.format != DXGI_FORMAT_R8G8_SNORM &&
        std::max(Meta.width, Meta.height) <= MaxSize && Cutoff < 0)
    {
        if (OutAlphaUsed && HasAlpha(Meta.format))
        {
            ScratchImage Img, Small;
            if (SUCCEEDED(LoadFromDDSMemory(Bytes.data(), Bytes.size(), DDS_FLAGS_NONE, nullptr, Img)))
            {
                size_t Level = 0;
                while (Level + 1 < Meta.mipLevels && Img.GetImage(Level, 0, 0)->width > 256) Level++;
                if (SUCCEEDED(Small.InitializeFromImage(*Img.GetImage(Level, 0, 0))) && ToPlain(Small))
                    *OutAlphaUsed = AlphaUsedIn(*Small.GetImage(0, 0, 0));
            }
        }
        Out.Width = (int)Meta.width; Out.Height = (int)Meta.height;
        Out.Dds = std::make_shared<const std::vector<uint8_t>>(std::move(Bytes));
        return true;
    }

    auto Img = std::make_shared<ScratchImage>();
    if (!Load(Bytes, Ext, *Img)) return false;
    // Capped, a block-compressed image four times the cap or more is decoded from every few of its
    // blocks (at twice the cap, halved below), not whole: a 4K BC7 map takes 100+ ms to decode.
    if (const Image* Top = Img->GetImage(0, 0, 0);
        MaxTexture > 0 && !OutAlphaUsed && Top && IsCompressed(Top->format) && std::max(Top->width, Top->height) >= Cap * 4)
    {
        ScratchImage Sample;
        if (!SampleBlocks(*Top, std::max(Top->width, Top->height) / (Cap * 2), Sample)) return false;
        *Img = std::move(Sample);
    }
    if (!ToPlain(*Img)) return false;
    if (OutAlphaUsed) *OutAlphaUsed = AlphaUsedIn(*Img->GetImage(0, 0, 0));
    if (!WithMips(*Img, Use, Cap)) return false;
    Out.Width = (int)Img->GetMetadata().width; Out.Height = (int)Img->GetMetadata().height;
    Out.Image = std::move(Img);
    if (Cutoff >= 0) PreserveAlphaCoverage(Out, Cutoff);
    return true;
}

bool ColorMapAlphaUsed(const std::vector<uint8_t>& Bytes, const std::string& Ext, bool& Used)
{
    Used = false;
    ScratchImage Img;
    if (Bytes.empty() || !Load(Bytes, ExportNaming::Lower(Ext), Img)) return false;
    const TexMetadata& M = Img.GetMetadata();
    if (!HasAlpha(M.format)) return true;
    const Image* Top = Img.GetImage(0, 0, 0);
    if (!Top) return false;
    ScratchImage Sample;
    if (IsCompressed(M.format) && (Top->width > 256 || Top->height > 256))
    {
        if (!SampleBlocks(*Top, std::max<size_t>(1, std::max((Top->width + 3) / 4, (Top->height + 3) / 4) / 64), Sample)) return false;
    }
    else if (FAILED(Sample.InitializeFromImage(*Top)))
        return false;
    if (!ToPlain(Sample)) return false;
    Used = AlphaUsedIn(*Sample.GetImage(0, 0, 0));
    return true;
}

bool ColorMapAlphaUsed(const std::string& Path, bool& Used)
{
    std::vector<uint8_t> Bytes;
    Used = false;
    return ReadAll(Path, Bytes) && ColorMapAlphaUsed(Bytes, std::filesystem::path(Path).extension().string(), Used);
}

void PreserveAlphaCoverage(PreviewImage& Img, float Cutoff)
{
    if (Img.Dds)
    {
        auto Decoded = std::make_shared<ScratchImage>();
        if (FAILED(LoadFromDDSMemory(Img.Dds->data(), Img.Dds->size(), DDS_FLAGS_NONE, nullptr, *Decoded)) || !ToPlain(*Decoded))
            return;
        Img.Image = std::move(Decoded);
        Img.Dds.reset();
    }
    if (!Img.Image) return;
    const TexMetadata M = Img.Image->GetMetadata();
    if (M.mipLevels < 2 || M.format != DXGI_FORMAT_R8G8B8A8_UNORM) return;
    auto Copy = std::make_shared<ScratchImage>();   // the image may be shared (a load's texture cache)
    if (FAILED(Copy->Initialize(M))) return;
    std::memcpy(Copy->GetPixels(), Img.Image->GetPixels(), std::min(Copy->GetPixelsSize(), Img.Image->GetPixelsSize()));
    Img.Image = std::move(Copy);

    // What DirectXTex's ScaleMipMapsAlphaForCoverage does, from one alpha histogram per level
    // instead of its search (about 1 s per 1024x1024 map): each level's alpha is scaled so as
    // many texels pass Cutoff as on the top level.
    auto Histogram = [](const Image& L, size_t (&H)[256]) {
        std::fill(std::begin(H), std::end(H), 0);
        for (size_t y = 0; y < L.height; y++)
            for (size_t x = 0; x < L.width; x++) H[L.pixels[y * L.rowPitch + x * 4 + 3]]++;
    };
    const int Ref = std::clamp((int)std::lround(Cutoff * 255.0f), 1, 255);
    size_t H[256];
    const Image* Top = Img.Image->GetImage(0, 0, 0);
    Histogram(*Top, H);
    size_t Passing = 0;
    for (int a = Ref; a < 256; a++) Passing += H[a];
    const double Coverage = (double)Passing / (double)(Top->width * Top->height);
    for (size_t Level = 1; Level < M.mipLevels; Level++)
    {
        const Image* L = Img.Image->GetImage(Level, 0, 0);
        Histogram(*L, H);
        const double Want = Coverage * (double)(L->width * L->height);
        size_t Count = 0;
        int T = 255;
        for (; T > 0; T--) { Count += H[T]; if ((double)Count >= Want) break; }
        if (T <= 0 || T == Ref) continue;
        const float Scale = (float)Ref / (float)T;
        for (size_t y = 0; y < L->height; y++)
            for (size_t x = 0; x < L->width; x++)
            {
                uint8_t& A = L->pixels[y * L->rowPitch + x * 4 + 3];
                A = (uint8_t)std::min(255, (int)std::lround(A * Scale));
            }
    }
}

ComPtr<ID3D11ShaderResourceView> CreatePreviewSRV(ID3D11Device* Dev, const PreviewImage& Img, bool Srgb, int MaxSize)
{
    ComPtr<ID3D11ShaderResourceView> Srv;
    if (Img.Dds)
    {
        // the loader skips the mips over MaxSize, and fails when none fits: then the whole image
        for (const size_t Max : { (size_t)std::max(0, MaxSize), (size_t)0 })
        {
            CreateDDSTextureFromMemoryEx(Dev, Img.Dds->data(), Img.Dds->size(), Max, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE,
                                         0, 0, Srgb ? DDS_LOADER_FORCE_SRGB : DDS_LOADER_DEFAULT, nullptr, Srv.GetAddressOf());
            if (Srv || Max == 0) break;
        }
    }
    else if (Img.Image)
    {
        TexMetadata Meta = Img.Image->GetMetadata();
        size_t First = 0;
        if (MaxSize > 0 && Meta.arraySize == 1 && Meta.depth == 1)
            while (First + 1 < Meta.mipLevels && std::max(Meta.width >> First, Meta.height >> First) > (size_t)MaxSize) First++;
        const Image* Images = Img.Image->GetImage(First, 0, 0);
        Meta.width = std::max<size_t>(1, Meta.width >> First);
        Meta.height = std::max<size_t>(1, Meta.height >> First);
        Meta.mipLevels -= First;
        CreateShaderResourceViewEx(Dev, Images, First ? Meta.mipLevels : Img.Image->GetImageCount(), Meta,
                                   D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0,
                                   Srgb ? CREATETEX_FORCE_SRGB : CREATETEX_DEFAULT, Srv.GetAddressOf());
    }
    return Srv;
}

PreviewImage PreviewImageFromRGBA(int W, int H, const std::vector<uint8_t>& Rgba)
{
    PreviewImage Out;
    if (W <= 0 || H <= 0 || Rgba.size() != (size_t)W * H * 4) return Out;
    Image Src{};
    Src.width = (size_t)W; Src.height = (size_t)H; Src.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    Src.rowPitch = (size_t)W * 4; Src.slicePitch = Src.rowPitch * H;
    Src.pixels = const_cast<uint8_t*>(Rgba.data());
    auto Img = std::make_shared<ScratchImage>();
    if (FAILED(Img->InitializeFromImage(Src)) || !WithMips(*Img, PreviewTexUse::Color)) return Out;
    Out.Width = W; Out.Height = H;
    Out.Image = std::move(Img);
    return Out;
}

bool PreviewImageToRGBA8(const PreviewImage& Img, int& W, int& H, std::vector<uint8_t>& Rgba)
{
    ScratchImage Top;
    if (Img.Dds)
    {
        ScratchImage All;
        if (FAILED(LoadFromDDSMemory(Img.Dds->data(), Img.Dds->size(), DDS_FLAGS_NONE, nullptr, All))) return false;
        if (FAILED(Top.InitializeFromImage(*All.GetImage(0, 0, 0)))) return false;
    }
    else if (!Img.Image || FAILED(Top.InitializeFromImage(*Img.Image->GetImage(0, 0, 0)))) return false;
    if (!ToPlain(Top)) return false;
    return ScratchImageToRGBA8(Top, Top.GetMetadata(), W, H, Rgba);
}
