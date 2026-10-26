#pragma once

#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <memory>
#include <string>
#include <vector>

struct PreviewGeometry;
namespace DirectX { class ScratchImage; }

// What one material draws with: its maps (null when absent) and the constants the pixel shader
// reads to know which channel means what (MaterialDescription.h).
struct PreviewGpuMaterial
{
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> Albedo, Normal, Roughness, Metalness, Specular, Occlusion, Emissive;
    // the tint gradient and its offset map; two layers' colour, detail normal, roughness, and the mask
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> Tint, TintOffset, ColorMask, LayerAlbedo[2], LayerNormal[2], LayerRoughness[2], LayerMask;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> Transmission, AlphaMap;
    struct Constants
    {
        float RoughMask[4] = { 1, 0, 0, 0 };  // the roughness value: dot(sample, mask)
        float MetalMask[4] = { 1, 0, 0, 0 };
        float OcclusionMask[4] = { 1, 0, 0, 0 };
        float RoughInvert = 0;                  // 1: the map is gloss
        float RoughValue = 0.6f, MetalValue = 0, Cutoff = 0.5f;
        uint32_t Flags = 0;                     // PreviewMaterialFlags
        uint32_t NormalEncoding = 0;            // Materials::NormalEncoding
        float Pad[2] = {};
        float TintColor[4] = { 1, 1, 1, 0.5f };  // rgb: colour times the gradient's channel; w: its u
        float TintParams[4] = {};               // x: offset scale, y: 1 = the gradient's rgb
        float TintChannel[4] = { 1, 0, 0, 0 }, OffsetChannel[4] = { 1, 0, 0, 0 }, MaskChannel[4] = { 1, 0, 0, 0 };
        float Layer[2][4] = { { 1, 1, 0, 1 }, { 1, 1, 0, 1 } };      // uv scale, rotation, normal scale
        float LayerTint[2][4] = { { 1, 1, 1, -1 }, { 1, 1, 1, -1 } }; // tint, roughness (< 0: none)
        float MaskScale[4] = { 1, 1, 0, 0 };
        float ColorTint[4] = { 1, 1, 1, 0 }, ColorMaskChannel[4] = { 1, 0, 0, 0 };
        float Specular[4] = { 1, 0, 0, 0 };      // x: the specular map's scale, y: 1 = pre-PBR (colour kept)
        float TransmissionColor[4] = { 1, 1, 1, 0 };   // rgb: linear colour, w: 1 = the map is a thickness
        float TransmissionMask[4] = { 1, 0, 0, 0 };
        float AlphaParams[4] = { 0, 1, 1, 1 };   // the alpha map's low, high, uv scale
        float AlphaMask[4] = { 1, 0, 0, 0 };
        float EmissiveColor[4] = { 1, 1, 1, 0 };   // rgb: the emission's colour (linear)
    } C;
    int Blend = 0;                              // 0 opaque, 1 cutout, 2 blend
};

enum PreviewMaterialFlags : uint32_t
{
    PreviewHasAlbedo = 1, PreviewHasNormal = 2, PreviewHasRoughness = 4, PreviewHasMetalness = 8,
    PreviewHasSpecular = 16, PreviewHasOcclusion = 32, PreviewHasEmissive = 64, PreviewCutout = 128,
    PreviewNormalFlipY = 256, PreviewHasTint = 512, PreviewHasTintOffset = 1024, PreviewHasLayers = 2048,
    PreviewHasLayerMask = 4096, PreviewHasColorTint = 8192, PreviewHasTransmission = 16384,
    PreviewBlended = 32768,     // alpha blended: the alpha is written out
    PreviewHasAlphaMap = 65536,
};

// A PreviewGeometry on the GPU: one interleaved vertex buffer (pos3, nrm3, uv2), the triangles
// grouped by material (one DrawIndexed per Range), a PreviewGpuMaterial per material.
struct PreviewGpuMesh
{
    Microsoft::WRL::ComPtr<ID3D11Buffer> VB, IB;
    // Layer > 0: triangles lying on a surface of an earlier material (a decal, a blend layer over the
    // base, CoD's way): drawn pulled toward the camera, one step per layer, so they do not z-fight.
    struct Range { int Mat; UINT Start, Count; int Layer = 0; };
    static constexpr int MaxLayer = 3;
    std::vector<Range> Ranges;
    std::vector<PreviewGpuMaterial> Mats;

    // Framing for an orbit camera, and the counts a viewer shows.
    DirectX::XMVECTOR Center{};
    float Radius = 1.0f, FrameDist = 3.0f;
    UINT VertexCount = 0, TriCount = 0, TexCount = 0;
    bool Valid = false;
};
// Only the device is used (it is free threaded): a loader thread may build the mesh.
// MaxTextureSize > 0 uploads each map from its first mip that fits (the asset grid's thumbnails).
PreviewGpuMesh UploadPreviewMesh(ID3D11Device* Dev, const PreviewGeometry& Geo, int MaxTextureSize = 0);
// Pos/Nrm (3 floats per vertex) and UV (2) as the vertex buffer's pos3, nrm3, uv2 records.
std::vector<float> InterleavePreviewVertices(const std::vector<float>& Pos, const std::vector<float>& Nrm,
                                             const std::vector<float>& UV);
// The bounding box's center and the largest vertex distance from it (at least 1e-4).
void PreviewBoundingSphere(const PreviewGeometry& Geo, DirectX::XMVECTOR& Center, float& Radius);
// The view a turning model needs: the box of the triangles drawn, without the outer 1% of their
// area on each axis (a few small stray pieces far off made the rest tiny), and the distance at
// which that box stays whole at every yaw of a turn at Pitch. ByCount (a map) weighs each triangle
// alike instead of by its area: the play area, not the vista planes around it, a thousand times
// larger and few triangles.
void FramePreviewForTurn(const PreviewGeometry& Geo, float Pitch, PreviewGpuMesh& M, bool ByCount = false);

struct PreviewCamera
{
    DirectX::XMFLOAT3 Eye{}, Target{}, Up{ 0, 0, 1 };
    float Fov = 0.61f, Near = 0.1f, Far = 1000.0f;
};
// The orbit viewers' camera: around Mesh's center moved by Pan (world units), CoD's Z up.
PreviewCamera OrbitPreviewCamera(const PreviewGpuMesh& Mesh, float Yaw, float Pitch, float DistScale,
                                 DirectX::XMFLOAT3 Pan = {});
// Moves Pan so the model follows a drag of Dx, Dy pixels across a ViewH-pixel-high view of Cam.
void PanPreviewCamera(const PreviewCamera& Cam, float Dx, float Dy, float ViewH, DirectX::XMFLOAT3& Pan);

// Where the renderer draws: MSAA HDR colour and depth, resolved, then tone mapped into Output
// (RGBA8, sRGB encoded, straight alpha) - what ImGui shows, the window copies, or the PNG capture
// reads. Straight holds the resolved image divided by its alpha when the background is transparent.
struct PreviewTarget
{
    int W = 0, H = 0;
    UINT Samples = 1;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> Hdr, HdrResolved, Straight, Output;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> HdrRTV, StraightRTV, OutputRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> DSV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> HdrSRV, StraightSRV, OutputSRV;
    bool Resize(ID3D11Device* Dev, int NewW, int NewH);
};

// The viewer's lighting controls; the defaults are the renderer's own look.
struct PreviewLighting
{
    float Exposure = 0.0f;      // stops
    float Lights = 1.0f;        // the three lights' strength
    bool Specular = true;       // false: every surface a plain dielectric (F0 0.04, colour whole)
};

// The one model renderer every preview uses (offscreen PNG, the orbit window, the asset browser).
// DirectXTK's PBREffect supplies the vertex stage and the camera/light constants; the pixel shader
// is ours (see the .cpp for why), ToneMapPostProcess the display transform.
class PreviewRenderer
{
public:
    explicit PreviewRenderer(ID3D11Device* Dev);
    ~PreviewRenderer();
    PreviewRenderer(const PreviewRenderer&) = delete;
    PreviewRenderer& operator=(const PreviewRenderer&) = delete;
    bool Ok() const;
    void SetLighting(const PreviewLighting& L);

    // Draws Mesh (or only the background) into T. Background alpha 0 leaves it transparent.
    void Render(ID3D11DeviceContext* Ctx, const PreviewGpuMesh* Mesh, const PreviewCamera& Cam, PreviewTarget& T,
                const float Background[4]);

    // Mesh drawn as Cam sees it into a target of its own, W x H, on a transparent background, and
    // written to a PNG: the viewport's image without the viewport (W/H sets the framing's aspect).
    bool Screenshot(ID3D11Device* Dev, ID3D11DeviceContext* Ctx, const PreviewGpuMesh& Mesh, const PreviewCamera& Cam,
                    int W, int H, const std::string& OutPngPath);
    // The same image in memory: the caller writes it (SavePreviewPng), off the UI thread.
    bool Capture(ID3D11Device* Dev, ID3D11DeviceContext* Ctx, const PreviewGpuMesh& Mesh, const PreviewCamera& Cam,
                 int W, int H, DirectX::ScratchImage& Out);

private:
    struct Impl;
    std::unique_ptr<Impl> P;
};

// Writes T's Output as a PNG (straight alpha). Requires WraithX::InitializeAPI first (WIC).
bool SavePreviewTargetPng(ID3D11Device* Dev, ID3D11DeviceContext* Ctx, const PreviewTarget& T, const std::string& OutPngPath);

// Renders one or more .semodel parts to a PNG with the renderer above on an offscreen device
// (hardware, else WARP so it runs headless). False if the model can't be loaded or the render fails.
// Requires WraithX::InitializeAPI first (WIC, for the PNG encode).
bool RenderModelGPU(const std::vector<std::string>& SemodelPaths, const std::string& OutPngPath, int Size = 512);
