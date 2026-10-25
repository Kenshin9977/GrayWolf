#pragma once

// Draws the effects preview's particles (FxPreviewSim.h) with Direct3D 11: camera-facing and
// oriented quads, tails and lines stretched along their velocity, clouds as puffs of small quads,
// trails as ribbons, lights as a glow, the atlas frame of each, blended additive, alpha or
// premultiplied, sorted back to front over a ground grid. Textures are the exported images
// (FxPreview::TextureIndex), a soft dot where none is found.

#include "FxPreviewSim.h"
#include "ModelPreviewGPU.h"   // PreviewCamera

#include <d3d11.h>
#include <wrl/client.h>

#include <memory>
#include <string>

namespace DirectX { class ScratchImage; }

namespace FxPreview
{
    // Where the effect is drawn: 4x MSAA colour and depth, resolved into Output, sRGB-encoded
    // RGBA8 (what ImGui::Image shows, OutputSRV, and the PNG capture reads).
    struct Target
    {
        int W = 0, H = 0;
        UINT Samples = 1;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> Color, Output;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> RTV;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> DSV;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> OutputSRV;
        bool Resize(ID3D11Device* Dev, int NewW, int NewH);
    };

    struct DrawOptions
    {
        bool Grid = true;
        float Background[4] = { 0.086f, 0.086f, 0.098f, 1.0f };   // linear-ish display colour
        float GridCenter[3] = {};
        float GridStep = 0;                                          // 0: from the camera's distance
        bool ShowLights = true;
    };

    class Renderer
    {
    public:
        explicit Renderer(ID3D11Device* Dev);
        ~Renderer();
        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;
        bool Ok() const;
        // where the materials' images are found (shared: a grid of cells may share one)
        void SetTextures(std::shared_ptr<const TextureIndex> Index);
        // images start at their first mip within MaxSize (0: whole; a grid's small cells)
        void SetTextureLimit(int MaxSize);
        // Reads the images and models F draws, found through Index, so Render finds them ready.
        // Any thread (the device makes them): a loader thread prepares an effect before it shows.
        void Prepare(const Frame& F, const TextureIndex& Index);
        // Forgets which image each material drew with (kept by its address): call it once the
        // effects drawn so far are freed, before others are loaded where they were.
        void ForgetMaterials();
        // Draws F as Cam sees it into T. Counts what was drawn.
        void Render(ID3D11DeviceContext* Ctx, const Frame& F, const PreviewCamera& Cam, Target& T, const DrawOptions& O);
        size_t QuadsDrawn() const;
        size_t TexturesMissing() const;   // materials drawn with the soft dot

    private:
        struct Impl;
        std::unique_ptr<Impl> P;
    };

    // T's Output as RGBA8 (straight alpha), for SavePreviewPng
    bool CaptureTarget(ID3D11Device* Dev, ID3D11DeviceContext* Ctx, const Target& T, DirectX::ScratchImage& Out);

    // An effect ready to be shown: its library (the effects it starts), simulation and framing.
    struct Scene
    {
        std::shared_ptr<Library> Lib;
        std::shared_ptr<const Effect> Fx;
        std::unique_ptr<Simulator> Sim;
        std::shared_ptr<const TextureIndex> Textures;
        float Center[3] = {}, Radius = 32;   // what the camera frames: the particles over a run
        double DurationMsec = 1000;
        // Loads the effect at JsonPath, the images of its game folders (GameFolders) and the extra
        // folders given; frames it. False with Error when the file is not an effect.
        bool Load(const std::string& JsonPath, const std::vector<std::string>& ExtraFolders, std::string& Error,
                  const SimOptions& Options = {});
        // frames the effect again (after the options changed)
        void Frame();
    };

    // The orbit camera around a scene (Yaw, Pitch in radians; DistScale 1 frames it whole)
    PreviewCamera OrbitCamera(const Scene& S, float Yaw, float Pitch, float DistScale, const float Pan[3] = nullptr);

    // Renders a scene at Msec into T, W x H, from the default orbit: what a cell of an asset
    // grid shows of an effect. The renderer must be made on the context's device.
    bool RenderAt(ID3D11DeviceContext* Ctx, Renderer& R, Scene& S, double Msec, Target& T, int W, int H,
                  const DrawOptions& O = {});
}
