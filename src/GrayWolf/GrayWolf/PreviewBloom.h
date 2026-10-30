#pragma once

// The model preview's post-processing before its tone map: a soft HDR bloom (the bright parts
// spread over a pyramid of four blurred halvings, DirectXTK's BasicPostProcess blur and downscale,
// DualPostProcess merge). Its targets are kept for the last size drawn.

#include <d3d11.h>

#include <memory>

class PreviewBloom
{
public:
    explicit PreviewBloom(ID3D11Device* Dev);
    ~PreviewBloom();
    PreviewBloom(const PreviewBloom&) = delete;
    PreviewBloom& operator=(const PreviewBloom&) = delete;

    // Src (linear HDR, W x H) with its bloom added at Strength, in a target of the bloom's own: what
    // the tone map reads next. Src itself when Strength is 0 or the targets cannot be made. Exposure
    // (stops, the tone map's) places the threshold where the display starts to saturate. Leaves no
    // render target bound, the viewport W x H.
    ID3D11ShaderResourceView* Apply(ID3D11DeviceContext* Ctx, ID3D11ShaderResourceView* Src, int W, int H, float Strength,
                                    float Exposure);

private:
    struct Impl;
    std::unique_ptr<Impl> P;
};
