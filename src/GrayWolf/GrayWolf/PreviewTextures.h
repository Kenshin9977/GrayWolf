#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace DirectX { class ScratchImage; }

// A texture made ready for the preview's GPU off the UI thread: a DDS that already carries its
// whole mip chain stays as its file bytes (uploaded as is, block compressed); anything else is
// decoded once and given a mip chain (sRGB-correct for colour, coverage-preserving for cutouts).
struct PreviewImage
{
    std::shared_ptr<const std::vector<uint8_t>> Dds; // uploaded with DirectXTK's DDSTextureLoader
    std::shared_ptr<DirectX::ScratchImage> Image;    // decoded, with mips
    int Width = 0, Height = 0;
    bool Empty() const { return !Dds && !Image; }
};

enum class PreviewTexUse
{
    Color,   // sRGB colour (albedo, specular colour, emission)
    Data,    // linear values (gloss, occlusion, metalness)
    Normal,  // linear, two or three channels
};

// Reads and prepares Path. Cutoff >= 0 makes the alpha mips keep the top level's coverage at that
// alpha. OutAlphaUsed (colour maps) tells whether the alpha channel masks anything. False when the
// file is missing or does not decode. MaxTexture > 0 keeps the image within that size, and reads
// only the levels within it of a mipped block-compressed DDS (a map's hundreds of textures).
bool PreparePreviewImage(const std::string& Path, PreviewTexUse Use, float Cutoff, PreviewImage& Out,
                         bool* OutAlphaUsed = nullptr, int MaxTexture = 0);

// Whether a colour map's alpha masks anything (what Auto alpha means, see OutAlphaUsed above),
// judged on at most 256x256 texels: a block-compressed image on a grid of its blocks, never
// decoded whole. Bytes are the file's (Ext ".dds", ".png", ".tga"...). False when it does not decode.
bool ColorMapAlphaUsed(const std::vector<uint8_t>& Bytes, const std::string& Ext, bool& Used);
bool ColorMapAlphaUsed(const std::string& Path, bool& Used);

// Applies a coverage cutoff to an already prepared colour map (its alpha decided the cutout).
void PreserveAlphaCoverage(PreviewImage& Img, float Cutoff);

// A view of a prepared image. Device only (free threaded): a loader thread may call it. MaxSize > 0
// starts the texture at its first mip no larger than that (the whole image when none is).
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> CreatePreviewSRV(ID3D11Device* Dev, const PreviewImage& Img, bool Srgb,
                                                                   int MaxSize = 0);

// A straight RGBA8 image (the .glb previews) with mips, for CreatePreviewSRV.
PreviewImage PreviewImageFromRGBA(int W, int H, const std::vector<uint8_t>& Rgba);

// The top level as straight RGBA8, for the software rasterizer.
bool PreviewImageToRGBA8(const PreviewImage& Img, int& W, int& H, std::vector<uint8_t>& Rgba);
