#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>

#include "MaterialDescription.h"
#include "PreviewTextures.h"

namespace DirectX { class ScratchImage; struct TexMetadata; struct Image; }

// A decoded RGBA8 texture (diffuse map), ready to upload to the GPU.
struct PreviewTexture
{
    int Width = 0, Height = 0;
    std::vector<uint8_t> Rgba; // Width*Height*4, straight RGBA
};

// A material as the GPU preview draws it: its description (paths resolved) and its maps, made
// ready off the UI thread. Alpha is resolved (never Auto).
struct PreviewMaterial
{
    Materials::Description Desc;
    PreviewImage Albedo, Normal, Roughness, Metalness, Specular, Occlusion, Emissive;
    PreviewImage Tint, TintOffset, ColorMask, LayerAlbedo[2], LayerNormal[2], LayerRoughness[2], LayerMask;
    PreviewImage Transmission, AlphaMap;
    Materials::AlphaMode Alpha = Materials::AlphaMode::Opaque;
};

// One skeleton bone in BIND POSE, in GLOBAL (model-space, not parent-relative) transform - the same
// convention the .semodel/.XMODEL_EXPORT files themselves store bones in.
struct PreviewBone
{
    std::string Name;
    int32_t Parent = -1;
    float Pos[3] = { 0, 0, 0 };
    float Rot[4] = { 0, 0, 0, 1 }; // quaternion x,y,z,w
};

// Up to this many bone influences are kept per vertex for skinning; a source format's own per-vertex
// weight count is truncated (keeping the largest weights) and renormalized to sum to 1 if it exceeds
// this. 4 is the conventional cap for real-time skinning and comfortably covers CoD's own exports.
constexpr int PREVIEW_MAX_BONE_INFLUENCES = 4;

// One or more .semodel parts, parsed + bone-aligned + merged, with per-vertex attributes and each
// material's decoded diffuse texture. Shared by the software (ModelPreview) and GPU (ModelPreviewGPU)
// renderers so both consume identical geometry.
struct PreviewGeometry
{
    std::vector<float> Pos;      // 3 per vertex (x,y,z) - bind pose
    std::vector<float> Nrm;      // 3 per vertex - bind pose
    std::vector<float> UV;       // 2 per vertex
    std::vector<int32_t> VtxMat; // material index per vertex (-1 = none)
    std::vector<uint32_t> Idx;   // 3 per triangle
    std::vector<PreviewTexture> Materials; // indexed by material id
    // Model files' materials, parallel to Materials (whose Rgba is then left empty); sources
    // without them (.glb, live) keep a plain diffuse in Materials.
    std::vector<PreviewMaterial> MaterialMaps;

    std::vector<PreviewBone> Bones; // bind-pose skeleton (empty if the source had none, e.g. a map)
    // Flattened PREVIEW_MAX_BONE_INFLUENCES-per-vertex skin weights, parallel to Bones above. An unused
    // slot is BoneIdx=-1, Weight=0. Empty when the source model has no skinning data (e.g. rigid single-
    // bone parts, or a map) - VtxBoneIdx.empty() is the "not skinned" check.
    std::vector<int32_t> VtxBoneIdx;
    std::vector<float> VtxBoneWeight;
};

// Parses + assembles the given .semodel parts into one PreviewGeometry (see RenderModelsToPNG for the
// assembly rules). Returns false if none could be parsed, or once Cancelled (checked between
// parts and textures) returns true.
bool LoadPreviewGeometry(const std::vector<std::string>& SemodelPaths, PreviewGeometry& Out,
                         const std::function<bool()>& Cancelled = {});

// The same, as one skeleton an anim plays on (AnimRigs.h): a later part's bones join the first's
// (a bone of the same name is that bone, the others are appended, the part's root under the bone it
// is seated on: a weapon's j_gun on the hands' tag_weapon) and its vertices keep their weights.
bool LoadRigGeometry(const std::vector<std::string>& Paths, PreviewGeometry& Out,
                     const std::function<bool()>& Cancelled = {});

// Loads a map .glb (our own map-export writer's output, or any glTF 2.0 binary) into a PreviewGeometry
// via tinygltf, so map previews reuse the exact same GPU upload + viewport code as .semodel/.XMODEL_EXPORT
// models. Every node places its mesh with its transform (a map's models are instanced), except the
// map writer's CoD-to-glTF root, so the geometry stays Z-up; skinning/animation are ignored - a static
// preview only. Textures are kept at 512 texels at most. Skips sky-named meshes: a skybox
// hijacks camera auto-framing. Returns false if nothing could be read.
bool LoadPreviewGeometryGlb(const std::string& GlbPath, PreviewGeometry& Out);

// Diagnostic: renders a map .glb straight to a PNG via the headless software rasterizer, no D3D
// device/window needed - a fast way to sanity-check a map load (framing, textures) without driving
// the interactive GUI. Requires WraithX::InitializeAPI first (WIC, for the PNG encode).
bool RenderGlbToPNG(const std::string& GlbPath, const std::string& OutPngPath, int Size = 512);

// Diagnostic: renders an already-assembled PreviewGeometry (e.g. a skinned/animated pose - swap Pos/
// Nrm for the output of SkinPreviewGeometry before calling this) straight to a PNG via the headless
// software rasterizer. Requires WraithX::InitializeAPI first (WIC, for the PNG encode).
bool RenderPreviewGeometryToPNG(const PreviewGeometry& Geo, const std::string& OutPngPath, int Size = 512);

// Writes an RGBA8 image (straight alpha) as a PNG through WIC, creating its folder: every preview
// PNG goes through here. Requires WraithX::InitializeAPI first.
bool SavePreviewPng(const DirectX::Image& Img, const std::string& OutPngPath);

// Decompresses (BC1-BC7) or converts an already-loaded DirectXTex image to row-tight RGBA8,
// shared by the model-texture loader below and the asset browser's image preview.
// False on decode failure.
bool ScratchImageToRGBA8(DirectX::ScratchImage& Img, const DirectX::TexMetadata& Meta,
                          int& OutW, int& OutH, std::vector<uint8_t>& OutRgba);

// Loads a texture file into RGBA8: PNG/JPEG via WIC, DDS (BC1-BC7 and uncompressed) via DirectXTex.
// False (OutW=OutH=0) on a missing file or a decode failure.
bool DecodeImageFileToRGBA8(const std::string& Path, int& OutW, int& OutH, std::vector<uint8_t>& OutRgba);

// Reads just the bone/mesh/material counts from a SEModel file's fixed-offset header (its first 26
// bytes) - shared by the asset browser's folder scan and AssetIndex so every reader of this header
// agrees byte-for-byte on the layout. False if the file is too short or the magic differs.
bool ReadSEModelHeaderCounts(const std::string& Path, uint32_t& Bones, uint32_t& Meshes, uint32_t& Materials);

// Diagnostic: a text summary of a .semodel (vertex bbox + skeleton bones with parent/pos/rotation),
// used to debug multi-part seating. Empty string if it cannot be parsed.
std::string DumpModelInfo(const std::string& SemodelPath);

// Renders a .semodel to a shaded PNG thumbnail using a headless software rasterizer (no GPU,
// window, or D3D device needed) - the model-preview counterpart to RenderImageToPNG. A 3/4 view,
// z-buffered, with Lambert shading, on a transparent background so it composites into contact
// sheets. Returns false if the model cannot be parsed or nothing could be drawn.
// Requires WraithX::InitializeAPI first (WIC, for the PNG encode).
bool RenderModelToPNG(const std::string& SemodelPath, const std::string& OutPngPath, int Size = 256);

// Renders several .semodel parts (e.g. a character's body + head + gear) into one shared, framed
// scene - the assembled preview. Parts are merged in their common model space; each keeps its own
// textures. Returns false if none could be parsed.
bool RenderModelsToPNG(const std::vector<std::string>& SemodelPaths, const std::string& OutPngPath, int Size = 256);

// Renders Frames views of the model(s) evenly spaced around the up axis and tiles them into one
// contact-sheet PNG. Returns false if nothing could be drawn.
bool RenderModelTurntable(const std::vector<std::string>& SemodelPaths, const std::string& OutPngPath,
                          int Size = 256, int Frames = 8);
