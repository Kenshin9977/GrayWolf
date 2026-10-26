#pragma once

// The level of a 32-bit Treyarch zone (Black Ops T5, Black Ops II T6): what the two titles share
// and where each keeps it. GfxWorld holds its name and baseName, the surface count, the sky (a
// model the world names, else an image), GfxWorldDraw (vertices, u16 indices) and the brush
// models (model 0 is the level) over GfxWorldDpvsStatic's surfaces (srfTriangles_t, then the
// material) and static model draw instances (a GfxPackedPlacement, then the model). ComWorld's
// primary lights: type u8 0 (1 sun, 2 spot, else omni), colour 8, direction 0x14, origin 0x20,
// radius 0x2c, cosHalfFovOuter 0x30. A title gives its places (Level32Layout).

#include "ZonekitZone.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

class TreyarchLevelZone : public ZonekitZone
{
public:
    using ZonekitZone::ZonekitZone;

    // How a title packs a world vertex
    enum class VertexFormat
    {
        // Black Ops: 44-byte GfxWorldVertex, xyz 0, colour 16, uv 20 (floats), normal 36 (bytes
        // (b - 127) x (scale + 192) / 32385, the scale in the fourth byte)
        Records44,
        // Black Ops II: a packed block (vd0) of vertexDataSize0 / vertexCount bytes each, xyz 0,
        // colour 16, uv 20 (two halves), normal 28 (10:10:10, the game's "third based" decode);
        // a surface's first vertex is a byte offset into the block
        Packed
    };

    struct Level32Layout
    {
        int64_t BaseName = 4, SurfaceCount = 0x10;
        int64_t SkyImage = -1, SkyModel = -1;   // -1: the title has none
        // GfxWorldDraw, from GfxWorld
        int64_t VertexCount = 0, Vertices = 0, VertexBytes = -1, IndexCount = 0, Indices = 0;
        VertexFormat Format = VertexFormat::Records44;
        // brush models: count, array, record size, its surface count and first surface
        int64_t BrushCount = 0, Brushes = 0, BrushSize = 0, BrushSurfaceCount = 0, BrushFirstSurface = 0;
        // GfxWorldDpvsStatic, from GfxWorld: the static model count, the surfaces and the draw
        // instances (record size, the placement and the model in a record)
        int64_t SModelCount = 0, Surfaces = 0, SModels = 0, SModelSize = 0, SModelPlacement = 0, SModelModel = 0;
        // GfxSurface: size, srfTriangles_t's first vertex, triangle count (u16), first index; the material
        int64_t SurfaceSize = 0x50, SurfaceFirstVertex = 0, SurfaceTris = 0, SurfaceBaseIndex = 0, SurfaceMaterial = 0;
        // ComWorld: the primary light count and array, a light's size
        int64_t LightCount = 8, Lights = 0xc, LightSize = 0;
        // clipMap_t (col_map_mp, col_map_sp): its brushes (MapCollision.h), none when BrushSize is 0
        Maps::ClipLayout Clip;
    };

    // Where a title keeps a weapon's models and xanims: WeaponVariantDef (zone type "weapon"): its
    // WeaponDef at Def, the xanim names (Anims, AnimCount pointers); WeaponDef: gunXModel (GunCount
    // model pointers) at Guns, handXModel at Hands; WeaponAttachmentUnique ("attachmentunique",
    // Black Ops II): its xanim names at UniqueAnims (-1: the title has none)
    struct WeaponLayout
    {
        int64_t Def = 8, Anims = 0, AnimCount = 0, Guns = 4, GunCount = 16, Hands = 8, UniqueAnims = -1, UniqueAnimCount = 0;
    };

protected:
    // ReadMap for a title of the family, its materials read by the live reader
    bool ReadLevel(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models, const Level32Layout& L,
                   const std::function<XMaterial_t(uint64_t)>& Material);
    // The anim owners' weapon documents (GscAnimOwners::WeaponDocument) of the weapons and
    // attachment uniques this zone defines
    void WeaponDocuments(std::vector<std::pair<std::string, std::string>>& Out, const WeaponLayout& L) const;
};
