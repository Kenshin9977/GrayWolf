#pragma once

// The model viewport's quality passes, kept apart from the renderer: ModelPreviewGPU.cpp calls them
// at four marked hooks ("PreviewPost hook") in PreviewRenderer::Render.
//  - Ambient occlusion: Intel's XeGTAO (ThirdParty/XeGTAO, MIT) over a 1x depth and normal prepass,
//    applied in the main pixel shader to the ambient and image based light only (and as specular
//    occlusion), never to the three direct lights.
//  - SMAA 1x (ThirdParty/SMAA, MIT) on the tone mapped image, optional, over or instead of MSAA.
//  - Still accumulation: while nothing changes (camera, mesh, lighting, size, quality), each frame is
//    drawn again with a sub-pixel jitter (and a new AO noise) and averaged into a float target, up to
//    StillSamples frames; then the view is not drawn at all until something changes. The captures
//    (render-model --gpu, the screenshot button) draw all of those frames at once.

#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <directxtk/CommonStates.h>   // DirectXTK's classes live in an inline namespace: no forward declaration
#include <directxtk/Effects.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

struct PreviewGpuMesh;
struct PreviewCamera;
struct PreviewTarget;

// The viewport's quality settings (graywolf.json: preview_ao, preview_ao_strength, preview_ao_radius,
// preview_msaa, preview_smaa, preview_still_samples). The defaults are the viewport's look.
struct PreviewQuality
{
    bool Ao = true;              // screen space ambient occlusion
    float AoStrength = 2.0f;     // the occlusion raised to this power (0: none, 1: XeGTAO's own; 2 reads in creases)
    float AoRadius = 0.1f;       // the occlusion's reach, as a fraction of the model's bounding radius
    int Msaa = 4;                // samples a pixel: 1, 2, 4 or 8 (fewer when the device has not as many)
    bool Smaa = false;           // SMAA 1x on the tone mapped image
    int StillSamples = 16;       // jittered frames averaged while the view holds still (1: none)

    // The renderer as it was before these passes: what a renderer nobody configured draws (the asset
    // grid's thumbnails, whose disk cache was made that way).
    static PreviewQuality Legacy()
    {
        PreviewQuality Q;
        Q.Ao = false;
        Q.StillSamples = 1;
        return Q;
    }
    bool operator==(const PreviewQuality&) const = default;
};

PreviewQuality LoadPreviewQuality();
void SavePreviewQuality(const PreviewQuality& Q);
// "key=value,..." over Q, the settings' names without "preview_" (ao=0, ao_strength=1.5, ao_radius=0.05,
// msaa=8, smaa=1, still_samples=32, accum=32 as a short name). False (and Error) on an unknown key or value.
bool ApplyPreviewQualitySpec(PreviewQuality& Q, const std::string& Spec, std::string* Error = nullptr);
// The settings as ImGui controls (inside a popup or a menu); saved when an edit is let go. True when Q changed.
bool DrawPreviewQualityControls(PreviewQuality& Q);

// One frame as Begin planned it.
struct PreviewPostFrame
{
    bool Skip = false;      // the still is done: Output already holds it, nothing to draw
    int Index = 0;          // the frame's place in the still (0: the plain, unjittered frame)
    int Of = 1;             // the frames the still averages
    float Jitter[2] = {};   // pixels
};

class PreviewPost
{
public:
    explicit PreviewPost(ID3D11Device* Dev);
    ~PreviewPost();
    PreviewPost(const PreviewPost&) = delete;
    PreviewPost& operator=(const PreviewPost&) = delete;

    // The main pixel shader's source is ShaderPrefix() + the renderer's own: the prefix declares what
    // the hooks in it call (PreviewScreenAO, PreviewSpecularOcclusion). CompilePrepass builds the
    // depth/normal prepass from the same source (its entry, ShaderSuffix(), reads the material's alpha).
    static const char* ShaderPrefix();
    bool CompilePrepass(const char* MainShaderSource);
    bool Ok() const;

    void SetQuality(const PreviewQuality& Q);
    const PreviewQuality& Quality() const;
    // Something the still's key cannot see changed (vertices rewritten in place, a texture swapped):
    // the next frame starts a new still.
    void Invalidate();

    // Hook 1, first thing in Render: the target's MSAA set to the quality's, the still's next frame
    // (or Skip). Lighting: the renderer's lighting settings as bytes (part of the still's key).
    PreviewPostFrame Begin(ID3D11DeviceContext* Ctx, const PreviewGpuMesh* Mesh, const PreviewCamera& Cam, PreviewTarget& T,
                           const float Background[4], const void* Lighting, size_t LightingBytes);
    // Hook 2: the projection moved by the frame's jitter.
    DirectX::XMMATRIX Jitter(const PreviewPostFrame& F, DirectX::FXMMATRIX Proj, const PreviewTarget& T) const;
    // Hook 3, after the effect's matrices and lights are set, before the main pass: the prepass and
    // XeGTAO, then the occlusion bound for the main pass (t40, b6). Leaves the pipeline to be set again.
    void Occlusion(ID3D11DeviceContext* Ctx, const PreviewPostFrame& F, const PreviewGpuMesh& Mesh, DirectX::FXMMATRIX View,
                   const PreviewCamera& Cam, PreviewTarget& T, DirectX::PBREffect& Effect, ID3D11InputLayout* Layout,
                   ID3D11Buffer* MaterialCB, DirectX::CommonStates& States);
    // Hook 4, after the tone map wrote T.Output: SMAA, then the frame added to the still.
    void End(ID3D11DeviceContext* Ctx, const PreviewPostFrame& F, PreviewTarget& T);

private:
    struct Impl;
    std::unique_ptr<Impl> P;
};
