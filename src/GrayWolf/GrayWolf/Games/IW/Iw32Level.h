#pragma once

// The level of a 32-bit Infinity Ward zone (CoD4, World at War, Call of Duty Online): what these
// titles share (Games/IW/Iw32Level.cpp reads it) and where each keeps it (MapLayout, every offset
// one the title's zonekit tables read).

#include "ZonekitZone.h"

#include <map>
#include <string>

class Iw32LevelZone : public ZonekitZone
{
public:
    using ZonekitZone::ZonekitZone;

protected:
    struct MapLayout
    {
        // GfxWorld: the index buffer (u16), the surface count, the sky image (-1: in the skies
        // below), the sky model's name (-1: none), the vertex buffer (44-byte GfxWorldVertex)
        int64_t IndexCount, Indices, SurfaceCount, SkyImage, SkyModel, VertexCount, Vertices;
        // its brush models (model 0 is the level): their count, array, size, and where a
        // model's surface count and first surface are, each BrushField bytes
        int64_t BrushCount, Brushes, BrushSize, BrushSurfaceCount, BrushFirstSurface;
        uint32_t BrushField;
        // GfxWorldDpvsStatic: the static model count, the surfaces (size, material) and the
        // static model draw records (size; a GfxPackedPlacement at SModelPlacement, the model at
        // SModelModel)
        int64_t SModelCount, Surfaces, SurfaceSize, SurfaceMaterial, SModels, SModelSize;
        // clipMap_t: two dynamic entity lists (u16 counts, pointers; DynEntityDef records:
        // a quaternion at +4, the origin at +0x14, the model at +0x20)
        int64_t DynCounts, DynLists, DynSize;
        // ComWorld's primary lights: size, and where the colour is (dir, origin, radius,
        // cosHalfFovOuter follow it at +12, +24, +36, +40)
        int64_t LightSize, LightColor;
        int64_t SModelPlacement = 4, SModelModel = 0x38;   // CoD4 and World at War's draw records
        // the sky image in GfxWorld's skies (CODOL, MW2's GfxSky): the sky array, its image (-1: none)
        int64_t Skies = -1, SkyImageInSky = 0;
        // GfxWorld's layer block (vertexLayerDataSize, vld.data; -1: not read): the layered
        // materials' later layers' UVs (Maps::NamedLayerUvs)
        int64_t LayerDataSize = -1, LayerData = -1;
    };
    virtual MapLayout Map() const = 0;
    // clipMap_t's brushes (MapCollision.h); none by default
    virtual Maps::ClipLayout ClipBrushes() const { return {}; }
    // where MapEnts keeps its MapTriggers (Maps::ReadTriggers), -1: it has none
    virtual int64_t TriggersAt() const { return -1; }
    // the live material reader
    virtual XMaterial_t Material(uint64_t Address) const = 0;

    bool ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models) override;
    // the live material reader names its technique set: a shadow caster's is not drawn
    bool MaterialDrawn(const XMaterial_t& M) const override { return M.TechsetName.find("shadowcaster") == std::string::npos; }
};
