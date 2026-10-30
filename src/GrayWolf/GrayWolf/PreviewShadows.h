#pragma once

// The model preview's shadows: the key light's (light 0) soft shadow, one depth map fitted to the
// model's box, PCSS-like lookups (blocker search, then a PCF kernel as wide as the penumbra); and a
// ground at the model's lowest point that only darkens what is behind it (a shadow catcher: the key
// shadow plus a contact darkening from a depth map of the model seen from below).
// The pixel shader's side is PreviewShadowHlsl(), put before the preview's own: b4, t30, t31, s6, s7.

#include <d3d11.h>
#include <DirectXMath.h>

#include <memory>
#include <vector>

struct PreviewGpuMesh;
struct PreviewCamera;

// The viewport's shadow controls. The defaults draw none (the asset grid's cells); the viewport and
// render-model --gpu turn them on (LoadPreviewShadowSettings, the CLI's flags).
struct PreviewShadowSettings
{
    bool Shadows = false;      // the key light's shadow, on the model and on the ground
    bool Ground = false;       // the shadow catcher under the model
    float Softness = 0.5f;     // 0..1: the key light's apparent size (0 nearly hard, 1 a large soft box)

    static PreviewShadowSettings On() { PreviewShadowSettings S; S.Shadows = S.Ground = true; return S; }
    bool operator==(const PreviewShadowSettings&) const = default;
};

// graywolf.json's preview_shadows, preview_ground, preview_shadow_softness (on by default).
PreviewShadowSettings LoadPreviewShadowSettings();
void SavePreviewShadowSettings(const PreviewShadowSettings& S);
// The Viewport bar's controls; true when S changed in a way to save (a toggle, a slider let go).
bool DrawPreviewShadowControls(PreviewShadowSettings& S);

// The HLSL put before the preview's pixel shader: PreviewShadow() for a surface, the static KeyShadow.
const char* PreviewShadowHlsl();

// The bounds of a mesh's vertices (Pos: 3 floats each): the ground's height, the light's fit.
void PreviewMeshBox(const std::vector<float>& Pos, DirectX::XMFLOAT3& Min, DirectX::XMFLOAT3& Max);

// One renderer's shadow resources: the key light's map, the contact map, their shaders and states.
class PreviewShadows
{
public:
    explicit PreviewShadows(ID3D11Device* Dev);
    ~PreviewShadows();
    PreviewShadows(const PreviewShadows&) = delete;
    PreviewShadows& operator=(const PreviewShadows&) = delete;
    bool Ok() const;

    // Before the model's passes: the maps for Mesh lit along KeyDir (the way the key light travels),
    // and the constants. LightScale: the lights' strength. Leaves the output merger without targets.
    void RenderMaps(ID3D11DeviceContext* Ctx, const PreviewGpuMesh& Mesh, DirectX::FXMVECTOR KeyDir,
                    const PreviewShadowSettings& S, float LightScale);
    // Binds the maps and constants to the pixel shader (after the effect's Apply).
    void Bind(ID3D11DeviceContext* Ctx);
    // After the model's passes, into the bound target and depth: the ground (when on), then unbinds
    // the maps. Aspect: the target's width / height.
    void DrawGround(ID3D11DeviceContext* Ctx, const PreviewCamera& Cam, float Aspect);

private:
    struct Impl;
    std::unique_ptr<Impl> P;
};
