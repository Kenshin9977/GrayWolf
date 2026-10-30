#pragma once

// The model preview's image based light: the procedural studio sky (ModelPreviewGPU.cpp) or an
// environment baked from an HDR panorama (bake-environment, below), turned about the up axis and
// scaled; the split-sum BRDF table the pixel shader's specular reads (with GGX's multiple
// scattering made up for, Fdez-Aguera 2019).
//
// A baked environment is two DDS cubes beside the exe, environments\<id>_radiance.dds (GGX
// prefiltered: mip m is roughness m / (mips - 1)) and <id>_irradiance.dds (the cosine-weighted
// light over pi), BC6H. Their sources and licenses: environments\NOTICE.txt.

#include <d3d11.h>
#include <directxtk/Effects.h>   // PBREffect (in an inline namespace: not declared ahead)
#include <wrl/client.h>

#include <memory>
#include <string>
#include <vector>

struct PreviewLighting;

// The id the procedural sky goes by (the default)
inline constexpr const char* kStudioEnvironment = "studio";

struct PreviewEnvironmentInfo
{
    std::string Id, Name;   // Id: the files' prefix ("studio": the procedural sky); Name: shown
};
// The procedural sky first, then each environment found beside the exe (read once)
const std::vector<PreviewEnvironmentInfo>& PreviewEnvironments();

// HLSL the preview's pixel shader is compiled after: the environment's constants (b3), the BRDF
// table (t24) and the functions that light a surface with them (EnvDir, EnvBrdf, EnvEnergy,
// EnvImageLight).
const char* PreviewEnvironmentHlsl();

// One renderer's environment: the cubes bound (PBREffect binds them at t4, t5), the BRDF table and
// the constants. Only the device is kept.
class PreviewEnvironment
{
public:
    // The procedural sky's cubes, the fallback when an environment cannot be read
    PreviewEnvironment(ID3D11Device* Dev, ID3D11ShaderResourceView* StudioRadiance, int StudioMips,
                       ID3D11ShaderResourceView* StudioIrradiance);
    ~PreviewEnvironment();
    PreviewEnvironment(const PreviewEnvironment&) = delete;
    PreviewEnvironment& operator=(const PreviewEnvironment&) = delete;
    bool Ok() const;

    // L's environment loaded when it changed, set on Effect (before its Apply), the constants and
    // the table bound to the pixel stage
    void Apply(ID3D11DeviceContext* Ctx, DirectX::PBREffect& Effect, const PreviewLighting& L);
    // The radiance cube Apply set last and its mips (the backdrop samples it)
    ID3D11ShaderResourceView* Radiance() const;
    int RadianceMips() const;

private:
    struct Impl;
    std::unique_ptr<Impl> P;
};

// Bakes HdrPath (an equirectangular .hdr, Z up) into OutDir\<Id>_radiance.dds and
// <Id>_irradiance.dds, Size the radiance cube's face, its light scaled so its mean matches the
// procedural sky's. Report: what was written, the scale, the time.
bool BakePreviewEnvironment(const std::string& HdrPath, const std::string& OutDir, const std::string& Id, int Size,
                            std::string& Report);
