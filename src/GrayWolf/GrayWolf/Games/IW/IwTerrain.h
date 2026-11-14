#pragma once

// Black Ops 6 and Black Ops 7's terrain ("superterrain") as world geometry. Its heights are the
// vhmdata asset's heightmap; stterrain is what draws it (a quadtree of nodes over the same grid,
// their virtual-texture tiles, index lists, decal trees) and says which nodes are drawn and the
// terrain's colour map. Layouts measured on the zones' bytes.

#include "MapScene.h"
#include "MaterialDescription.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace IwTerrain
{
    // How a game's vhmdata holds its tiles: a tile set of buffers (0x38 at 0x38; Black Ops 6
    // multiplayer, Black Ops 7), image assets listed in the vhmdata (Black Ops 6 campaign), a
    // record per level (Modern Warfare III)
    enum class TileKind { Buffers, Images, Levels };

    // the fields this reader uses, per game: stterrain's, vhmdata's
    struct Layout
    {
        int64_t NodeCount = 0x15c, NodeRecords = 0x160;   // u32, u16 per node: its material record
        int64_t RecordCount = 0, Records = 0;             // u32, 32-byte material bitmasks (none set: not drawn)
        int64_t ColorCount = 0, Colors = 0;               // u32, 0x30-byte records: the colour map at 8 (0: none)
        TileKind Tiles = TileKind::Buffers;
        int64_t Geometry = -1;                            // stterrain's geometry struct (0x1a0: the partial nodes), -1: none
        int64_t LayerList = 0, LayerSize = 0;             // in it: the layers (count u32 0xdc, at LayerList, LayerSize each)
        int64_t VhmLevels = 0, VhmTileCount = 0, VhmTiles = 0;   // Images: u8, u32, the list (8 bytes a tile); Levels: u32, -, the records
        int64_t LayerCount = 0xdc;                        // in the geometry struct: the layers' u32 count
    };
    constexpr Layout BlackOps7{ 0x15c, 0x160, 0x1f4, 0x1f8, 0x1dc, 0x1e0, TileKind::Buffers, 0x18, 0xe8, 0x238 };
    constexpr Layout BlackOps6{ 0x15c, 0x160, 0x204, 0x208, 0x1e0, 0x1e8, TileKind::Buffers, 0x10, 0xe0, 0x210 };
    // the campaign's stterrain (0x2f0): node records 0x158 (count 0x154), material records 0x1f8
    // (0x1f4); its colour records (0x1e0, count 0x1d8, 0x18 each) hold the nodes' virtual-texture
    // tiles and a 36 x 36 map, no whole-terrain colour map (none read); its vhmdata (0x60):
    // levels u8 0x35, tile count u32 0x40, tiles 0x48 (image references; per tile min / max u16
    // 0x50 / 0x58)
    constexpr Layout BlackOps6Campaign{ 0x154, 0x158, 0x1f4, 0x1f8, 0, 0, TileKind::Images, 0x10, 0, 0, 0x35, 0x40, 0x48 };

    // Modern Warfare III's vhmdata (0x68): its levels' records (count u32 0x4c, 0x50); its
    // stterrain is not read (every node drawn, no colour map)
    constexpr Layout ModernWarfare3{ 0, 0, 0, 0, 0, 0, TileKind::Levels, -1, 0, 0, 0x4c, 0, 0x50 };
    // Modern Warfare 4's stterrain (0x3f8): Black Ops 7's fields moved: the geometry struct (0x1d8)
    // at 0x20, its layers (0x248 each, u32 count 0x110) at 0x120, the node records 0x1a0 (count
    // 0x19c), material records 0x260 (0x25c), colour records 0x220 (0x21c, 0x40 each, the colour
    // map at 8); vhmdata (0x50): Black Ops 7's tile set (0x50 records at 0x38, count 0x34)
    constexpr Layout ModernWarfare4{ 0x19c, 0x1a0, 0x25c, 0x260, 0x21c, 0x220, TileKind::Buffers, 0x20, 0x120, 0x248, 0, 0, 0, 0x110 };
    // its campaign's stterrain (0x3f0, sp26-cod.exe 0x21fcef0): the multiplayer's, every field 8
    // bytes on (the geometry struct at 0x18, unchanged within; vhmdata unchanged)
    constexpr Layout ModernWarfare4Campaign{ 0x194, 0x198, 0x254, 0x258, 0x214, 0x218, TileKind::Buffers, 0x18, 0x120, 0x248, 0, 0, 0, 0x110 };

    // a tile's samples (R16, row-major)
    struct Tile
    {
        std::vector<uint16_t> Samples;
        uint32_t W = 0, H = 0;
    };

    struct Source
    {
        // N bytes at a served address, null when they are not all there
        std::function<const uint8_t*(uint64_t Address, uint64_t N)> Locate;
        // the name the image kind gives the image a reference field holds ("" when none)
        std::function<std::string(uint64_t Field)> ImageName;
        // a streamed buffer by its package key (null when the packages do not hold it); unset: the
        // session's package cache
        std::function<std::unique_ptr<uint8_t[]>(uint64_t Key, uint32_t& Size)> Streamed;
        // TileKind::Images: the samples of the image a tile entry holds (its served address)
        std::function<bool(uint64_t Entry, Tile& Out)> ImageTile;
        // the material layers: the vtmdata asset (its index map: a layer per texel), and each
        // layer's material added to the scene from its reference field (its name, "" when unread)
        uint64_t Vtm = 0;
        std::function<std::string(uint64_t Field)> LayerMaterial;
    };

    // The grid's resolution: every sample of the finest grid the tiles give unless its quads exceed
    // MaxDrawnQuads (then every 2nd, 4th... sample), in meshes of at most MaxMeshQuads (Cold War's
    // Terrain.h budget per mesh)
    struct Options
    {
        uint32_t MaxDrawnQuads = 1u << 20;
        uint32_t MaxMeshQuads = 1u << 18;
    };

    // The terrain material's layers (Read's textures: semantic LayerSemantic + layer * 4 + 0 colour,
    // 1 "nog" (gloss r), 2 the layer's mask; constant block 0 the layers' uv scales, two floats
    // each) as the common format's layers, over the baked colour map: false when it has none
    constexpr uint32_t LayerSemantic = 0x7E770000;
    bool DescribeLayers(const Materials::Source& M, Materials::Description& D);

    // Adds the terrain of the vhmdata asset at Vhm (drawn where the stterrain asset at St says,
    // when St is not 0) to Scene.Objects as "terrain" (material "terrain_<map>" drawing its colour
    // map); Counters says what was read (terrain_*). False when there is no heightmap.
    bool Read(Maps::MapScene& Scene, const Source& Src, uint64_t Vhm, uint64_t St, const Layout& L, const Options& O,
              std::map<std::string, size_t>& Counters);
}
