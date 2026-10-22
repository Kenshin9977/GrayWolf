#pragma once

// What the Infinity Ward engine's 64-bit zones (Modern Warfare 2 to WWII) share on the disk
// template: the LOD surfaces the game links at load time, the streamed-image table some titles keep
// in the .ff header, and the level's records they have in common.

#include "ZonekitZone.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace IwWeaponDefs
{
    struct Layout;
}

class IWZone : public ZonekitZone
{
public:
    using ZonekitZone::ZonekitZone;

    // An XModel's LOD records: a LOD holds its xmodelsurfs asset at +LodModelSurfs, and at +LodSurfs
    // the surface array the game copies from it at load time (lod.surfs = modelSurfs->surfs), which
    // the live reader follows and the zone leaves empty; XModelSurfs keeps that array at
    // +ModelSurfsSurfs.
    struct LodLayout
    {
        const char* Owner = "xmodel";   // the type holding the LOD records
        int64_t Lods = 0, LodSize = 0, LodCount = 0, LodSurfs = 0;
        int64_t MaxLods = 6, LodModelSurfs = 8, ModelSurfsSurfs = 8;
    };

    // Streamed images (Ghosts, Advanced Warfare, MWR): the .ff header, before the compressed zone,
    // holds a u32 count at 24, then that many 24-byte entries {pak word, start, end} into
    // imagefile<index>.pak, four per streamed image (base level with its whole chain, then
    // MipLevels[0..2]) in the order the zone loads its streamed images. An empty range means the
    // level has no data.
    struct PakEntry
    {
        uint64_t Pak = 0, Start = 0, End = 0;
    };

    // ComWorld's primary lights (com_map: count u32 at 0xc, array at 0x10), typed by a byte (the
    // first by default: 1 sun, 2 spot, 3 omni); the rest of a record where the title keeps it. Def: its
    // definition's name pointer, for the map debug file (-1: none).
    struct LightLayout
    {
        int64_t Size = 0, Color = 0, Dir = 0, Origin = 0, Radius = 0, CosHalfFov = 0, Def = -1;
        int64_t Count = 0xc, Array = 0x10;   // ComWorld's count (u32) and array
        int64_t TypeAt = 0;                  // the type byte (Modern Warfare 2019: 1, after the spawn flags)
    };

    // A dynamic entity's pose (a quaternion, the origin) and its model in its record
    struct DynPoseLayout
    {
        int64_t Quat = 4, Origin = 20, Model = 32;
    };

    // World vertices kept as separate streams (WWII) instead of 44-byte records: a pointer per
    // stream, the positions 3 floats, the colours 4 bytes, the uvs 2 floats, the packed normals a u32
    struct VertexStreams
    {
        int64_t Positions = 0;   // 0: the records at MapLayout::Vertices
        int64_t Colors = 0, Uvs = 0, Normals = 0;
    };

    // The level of a Ghosts, Advanced Warfare or MWR map zone, where the title keeps it. GfxWorld:
    // name 8, surface count 0x1c, skies at 0x28 (their count at 0x20, a sky's image at 0x10), the
    // 44-byte vertices and u16 indices (counts, arrays), the brush models (count, array; a model's
    // size, first surface and surface count), GfxWorldDpvsStatic's static model count, 0x28-byte
    // surfaces (first vertex 4, material 0x18; triangle count, first index) and static models
    // (record size, model at 0x38). clipMap_t: two dynamic entity lists. A later title names its
    // types otherwise, moves some of these (the defaults are the Ghosts to MWR places), keeps its
    // vertices in a transient zone asset (WWII, Infinite Warfare) or as Streams (WWII).
    struct MapLayout
    {
        const char* World = "gfx_map";     // GfxWorld's type
        const char* Com = "com_map";       // ComWorld's
        const char* VertexAsset = nullptr;   // the type holding VertexCount and Vertices (null: GfxWorld)
        int64_t VertexCount = 0, Vertices = 0, IndexCount = 0, Indices = 0;
        VertexStreams Streams;
        int64_t BrushCount = 0, Brushes = 0, BrushSize = 0, BrushFirstSurface = 0, BrushSurfaceCount = 0;
        int64_t SModelCount = 0, Surfaces = 0, SModels = 0, SModelSize = 0, SModelModel = 0x38;
        int64_t SurfaceSize = 0x28, SurfaceFirstVertex = 4, SurfaceMaterial = 0x18;
        int64_t SurfaceTris = 0, SurfaceBaseIndex = 0;
        const char* Clip = nullptr;
        int64_t DynCounts = 0x188, DynLists = 0x190;   // clipMap_t's two u16 counts and two pointers
        int64_t DynSize = 0, DynDestroyed = -1;
        DynPoseLayout DynPose;
        LightLayout Lights;
        int64_t ImageNameAt = 0;   // GfxImage's name, for the sky
        // clipMap_t's brushes (MapCollision.h; none when BrushSize is 0) in the asset of type Clip
        // (col_map_sp when unset), and the MapTriggers in MapEnts (-1: not read) with its records' shapes
        Maps::ClipLayout ClipBrushes;
        int64_t Triggers = -1;
        Maps::TriggerLayout TriggerShapes;
        // the entity string's keys by number (Ghosts on: their script strings' token ids)
        std::span<const std::pair<uint32_t, const char*>> EntityKeys;
    };
    // Advanced Warfare's and MWR's clipMap_t brushes (the same shapes in both)
    static Maps::ClipLayout AwClipBrushes();

protected:
    // ReadMap for a title of MapLayout's family, its materials read by the live reader
    bool ReadLevel(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models, const MapLayout& M,
                   const std::function<XMaterial_t(uint64_t)>& Material);
    // clipMap_t's two dynamic entity lists (u16 counts at Counts, pointers at Lists): records of
    // Size bytes, a quaternion at 4, the origin at 20, the model at 32 unless Pose says otherwise
    // (the destroyed one at Destroyed, -1: none), placed as dynent_<list>_<k>
    void AddDynamicEntities(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models, const char* Clip, int64_t Counts,
                            int64_t Lists, int64_t Size, int64_t Destroyed = -1, const DynPoseLayout& Pose = {});
    void AddPrimaryLights(Maps::MapScene& Scene, const LightLayout& L, const char* ComType = "com_map");

    // Writes every LOD's surface pointer as that fixup does; Elsewhere gives the surfaces of a LOD
    // whose xmodelsurfs (its zone position) holds none here, 0 for none. Returns the LODs left
    // without surfaces.
    size_t LinkLodSurfaces(const LodLayout& L, const std::function<uint64_t(int64_t ModelSurfs)>& Elsewhere = nullptr);

    std::unordered_map<int64_t, std::array<PakEntry, 4>> Streams;   // image position -> entries
    // Streams for the images whose byte at StreamedAt is set, in walk order
    void ReadStreams(int64_t StreamedAt);
    // What a pak word means to the game's order (its own whole word unless the title packs more)
    virtual uint64_t PakOf(const PakEntry& E) const { return E.Pak; }
    // Streams' images in pak order: the game lays its streamed data out to be read front to back
    std::vector<int64_t> StreamOrder() const;
    // An image's pak entries mixed into its content key
    void MixStreams(int64_t Pos, ExportWriters::ContentKey& Key) const;
    // The sounds of the sound banks (.sabl, .sabs) at these paths, through SABSupport, each written
    // under its folder in the bank (sounds/cp/dlc4/p6/i/) in the saved sound format; a sound both
    // banks hold once, from the one holding more of it. Patches: the zone's patch_ banks (Modern
    // Warfare 2019), loaded with its own: an entry of theirs replaces the bank entry of the same key
    // and adds the keys the banks lack
    void ExportSoundBanks(const std::vector<std::string>& Banks, const DiskGame::Context& C, DiskGame::Totals& T,
                          const std::vector<std::string>& Patches = {});
    // The file ExportSoundBanks writes a bank's sound Name (cp\dlc4\x.pn.75.36000.french) to,
    // under the sound kind's folder: cp/dlc4/x<Extension>, each part made safe
    static std::string SoundBankFile(const std::string& Name, const std::string& Extension);
    // Defines' images: Streams' and Also's, those named in place (at their position + NameAt)
    void DefineStreamed(std::vector<DiskGame::AssetId>& Out, int64_t NameAt, const std::vector<int64_t>& Also = {}) const;
    // The rawfile kind as Ghosts to MW2019 keep it (IwRawData.cpp): rawfile {name, u32 compressed
    // size 8, size 0xc, buffer 0x10} (zlib when compressed), LuaType {name, i32 size 8, buffer 0x10}
    // named .lua/.luac
    void ExportsIwRawfiles(const char* LuaType = "luafile");
    // The data kind's string tables as Ghosts to WWII keep them: stringtable {name, columns 8, rows
    // 0xc, 16-byte cells {string, hash} at 0x10}
    void ExportsIwStringTables();
    // The weapons kind (the weapon and attachment assets as struct dumps, named by the string at
    // their 0, the layout's fields named) and the weaponstats kind (IwWeaponDefs) of the games
    // keeping the classic WeaponDef, Ghosts to WWII
    void ExportsIwWeapons(const IwWeaponDefs::Layout& L);
};
