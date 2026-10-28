#include "stdafx.h"
#include "ByteView.h"

// Black Ops 6 / 7 terrain: the heightmap of vhmdata as a
// grid, a point (c, r) of a tile of W samples at centre - half + (c, r) * 2 half / (W - 1), its
// height offset + sample / 65535 * range; drawn where stterrain's finest node over it has a
// material.

#include "IwTerrain.h"

#include "CoDAssets.h"
#include "MapLive.h"

#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <vector>

namespace
{
    // vhmdata: centre x y 0x8 (z 0x10), half extents x y 0x14, grid size u32 0x20 (and 0x24),
    // cells per tile u32 0x28, height offset float 0x2c, range float 0x30 (z = offset + sample /
    // 65535 * range: the bounds stterrain keeps, 263.04 +- 1174.2 on mp_saw_sm_island, are the
    // samples' 51 and 65535). Its tiles: a quadtree, level L 2^L x 2^L tiles (row-major, rows along
    // y), the tiles of all levels in one list (level 0 first). A tile is (cells + 4)^2 samples, its
    // own points from sample 1 (one more before, two after: the neighbours' first ones, which the
    // next tile repeats: mp_t10 sp_t10_intro's level 1, 4 samples equal across each seam), in the
    // whole grid's quantization; or the whole grid at once (Black Ops 6 multiplayer / 7's tile 0).
    constexpr uint64_t VhmCentre = 0x8, VhmHalf = 0x14, VhmGrid = 0x20, VhmCells = 0x28, VhmOffset = 0x2c, VhmRange = 0x30;
    // a buffer (0x28): {u32 size, u32 flags (bit 31: in the zone)} or the package key at 0, its
    // 16-byte views at 8 (size u32 0, width u16 4, height u16 6, format u8 0xb: 0x0c R16_UNORM), the
    // bytes at 0x10 when in the zone, the view count u32 0x20 >> 9
    constexpr uint64_t BufViews = 8, BufData = 0x10, BufViewCount = 0x20;

    template <typename T> T Get(const IwTerrain::Source& Src, uint64_t Address)
    {
        T V{};
        if (const uint8_t* P = Src.Locate(Address, sizeof(T))) std::memcpy(&V, P, sizeof(T));
        return V;
    }

    uint32_t StepFor(uint32_t Size, uint32_t MaxQuads)
    {
        uint32_t Step = 1;
        while ((uint64_t)((Size - 1) / Step) * ((Size - 1) / Step) > MaxQuads) Step *= 2;
        return Step;
    }

    // a buffer's R16 samples, in the zone or the packages
    bool BufferTile(const IwTerrain::Source& Src, uint64_t Buffer, IwTerrain::Tile& T, std::map<std::string, size_t>& Counters)
    {
        if (Src.Locate(Buffer, 0x28) == nullptr) return false;
        const uint64_t View = Get<uint64_t>(Src, Buffer + BufViews);
        const uint16_t W = Get<uint16_t>(Src, View + 4), H = Get<uint16_t>(Src, View + 6);
        if ((Get<uint32_t>(Src, Buffer + BufViewCount) >> 9) == 0 || W < 2 || W != H || Get<uint8_t>(Src, View + 0xb) != 0x0c) return false;
        const uint64_t Bytes = (uint64_t)W * H * 2;
        T.W = W, T.H = H;
        T.Samples.resize((size_t)W * H);
        if ((Get<uint32_t>(Src, Buffer + 4) >> 31) != 0)
        {
            const uint8_t* P = Src.Locate(Get<uint64_t>(Src, Buffer + BufData), Bytes);
            if (P == nullptr) return false;
            std::memcpy(T.Samples.data(), P, Bytes);
            return true;
        }
        if (!Src.Streamed && CoDAssets::GamePackageCache == nullptr) return false;
        uint32_t Got = 0;
        const uint64_t Key = Get<uint64_t>(Src, Buffer);
        const auto Data = Src.Streamed ? Src.Streamed(Key, Got) : CoDAssets::GamePackageCache->ExtractPackageObject(Key, Got);
        Counters["terrain_tiles_streamed"]++;
        if (Data == nullptr || Got < Bytes) return false;
        std::memcpy(T.Samples.data(), Data.get(), Bytes);
        return true;
    }

    // Modern Warfare III's tile blob: {u64, u32 parts 8, u32}, a {pointer, u32 size, u32} per part,
    // a pointer, then the parts (from 0x28 for one); a lone part of (cells + 4)^2 samples is the tile (level 0, in the
    // zone); the streamed ones (4 parts each, sizes under the samples': compressed) are not read
    bool BlobTile(const IwTerrain::Source& Src, uint64_t Blob, IwTerrain::Tile& T)
    {
        if (Src.Locate(Blob, 0x20) == nullptr || Get<uint32_t>(Src, Blob + 8) != 1) return false;
        const uint32_t Size = Get<uint32_t>(Src, Blob + 0x18);
        uint32_t W = 1;
        while ((uint64_t)W * W * 2 < Size) W++;
        if ((uint64_t)W * W * 2 != Size) return false;
        const uint8_t* P = Src.Locate(Blob + 0x28, Size);
        if (P == nullptr) return false;
        T.W = T.H = W;
        T.Samples.resize((size_t)W * W);
        std::memcpy(T.Samples.data(), P, Size);
        return true;
    }

    struct Grid
    {
        std::vector<uint16_t> Samples;
        uint32_t N = 0;   // samples a side
    };

    // The finest grid the budget asks for (Side cells a side at least, else the finest there is):
    // the whole-grid tile, or a level's tiles put together; a level missing a tile gives way to a
    // coarser one
    bool BuildGrid(const IwTerrain::Source& Src, uint64_t Vhm, const IwTerrain::Layout& L, uint32_t Side, Grid& G,
                   std::map<std::string, size_t>& Counters)
    {
        uint32_t Levels = 0, Count = 0;
        uint64_t Entries = 0;
        if (L.Tiles == IwTerrain::TileKind::Buffers)
        {
            // a tile set (0x38) at 0x38 (count u32 0x34): levels u8 5, tile count u32 0x10, tiles 0x18
            if (Get<uint32_t>(Src, Vhm + 0x34) == 0) return false;
            const uint64_t Set = Get<uint64_t>(Src, Vhm + 0x38);
            if (Src.Locate(Set, 0x38) == nullptr) return false;
            Levels = Get<uint8_t>(Src, Set + 5), Count = Get<uint32_t>(Src, Set + 0x10), Entries = Get<uint64_t>(Src, Set + 0x18);
        }
        else if (L.Tiles == IwTerrain::TileKind::Images)
            Levels = Get<uint8_t>(Src, Vhm + (uint64_t)L.VhmLevels), Count = Get<uint32_t>(Src, Vhm + (uint64_t)L.VhmTileCount),
            Entries = Get<uint64_t>(Src, Vhm + (uint64_t)L.VhmTiles);
        else
        {
            // a record (0x18) per level (count u32 VhmLevels, records VhmTiles): tiles a side u32 4,
            // its tiles 0x10 (0x10 each: the tile's blob at 0)
            Levels = Get<uint32_t>(Src, Vhm + (uint64_t)L.VhmLevels), Entries = Get<uint64_t>(Src, Vhm + (uint64_t)L.VhmTiles);
            Count = Src.Locate(Entries, (uint64_t)Levels * 0x18) != nullptr ? Levels : 0;
        }
        const uint32_t Cells = Get<uint32_t>(Src, Vhm + VhmCells), N = Get<uint32_t>(Src, Vhm + VhmGrid);
        const uint64_t Stride = L.Tiles == IwTerrain::TileKind::Levels ? 0x18 : 8;
        if (Count == 0 || Levels == 0 || Levels > 12 || Cells == 0 || Src.Locate(Entries, (uint64_t)Count * Stride) == nullptr) return false;
        // level Lv's tile Index (row-major)
        const auto Fetch = [&](uint32_t Lv, uint32_t Index, IwTerrain::Tile& T) {
            bool Ok = false;
            if (L.Tiles == IwTerrain::TileKind::Levels)
            {
                const uint64_t Rec = Entries + (uint64_t)Lv * 0x18;
                const uint32_t PerSide = Get<uint32_t>(Src, Rec + 4);
                if (Lv >= Levels || Index >= (uint64_t)PerSide * PerSide) return false;
                Ok = BlobTile(Src, Get<uint64_t>(Src, Get<uint64_t>(Src, Rec + 0x10) + (uint64_t)Index * 0x10), T);
            }
            else
            {
                const uint32_t Global = ((1u << (2 * Lv)) - 1) / 3 + Index;
                if (Global >= Count) return false;
                const uint64_t Entry = Get<uint64_t>(Src, Entries + (uint64_t)Global * 8);
                Ok = L.Tiles == IwTerrain::TileKind::Buffers ? BufferTile(Src, Entry, T, Counters) : Src.ImageTile && Src.ImageTile(Entry, T);
            }
            return Ok && T.W == T.H && T.Samples.size() >= (size_t)T.W * T.H;
        };

        // the candidates, coarse to fine: the whole-grid tile 0 (when it is one), each level
        IwTerrain::Tile Zero;
        const bool Whole = Fetch(0, 0, Zero) && Zero.W != Cells + 4;
        struct Candidate { int Level; uint32_t Res; };
        std::vector<Candidate> All;
        if (Whole) All.push_back({ -1, Zero.W - 1 });
        // the whole-grid tile when it is fine enough (the levels' tiles may be the same buffer)
        if (Whole && Zero.W - 1 >= Side) All.resize(1);
        else
            for (uint32_t Lv = 0; Lv < Levels; Lv++)
                if ((uint64_t)Cells << Lv <= N - 1 && (!Whole || ((uint64_t)Cells << Lv) > Zero.W - 1)) All.push_back({ (int)Lv, Cells << Lv });
        std::stable_sort(All.begin(), All.end(), [](const Candidate& A, const Candidate& B) { return A.Res < B.Res; });
        size_t First = 0;
        while (First + 1 < All.size() && All[First].Res < Side) First++;
        for (size_t K = First + 1; K-- > 0;)
        {
            const Candidate& C = All[K];
            if (C.Level < 0)
            {
                G.N = Zero.W;
                G.Samples = std::move(Zero.Samples);
                Counters["terrain_level"] = 0;
                return true;
            }
            const uint32_t Tiles = 1u << C.Level;
            G.N = C.Res + 1;
            G.Samples.assign((size_t)G.N * G.N, 0);
            bool Ok = true;
            size_t SeamSame = 0, SeamAll = 0;
            for (uint32_t R = 0; R < Tiles && Ok; R++)
                for (uint32_t Q = 0; Q < Tiles && Ok; Q++)
                {
                    IwTerrain::Tile T;
                    if (!Fetch((uint32_t)C.Level, R * Tiles + Q, T) || T.W != Cells + 4)
                    {
                        Ok = false;
                        break;
                    }
                    // the seam with the tile on the left: its last own column is this one's first
                    if (Q > 0)
                        for (uint32_t I = 0; I <= Cells; I++, SeamAll++)
                            SeamSame += G.Samples[(size_t)(R * Cells + I) * G.N + Q * Cells] == T.Samples[(size_t)(I + 1) * T.W + 1];
                    for (uint32_t I = 0; I <= Cells; I++)
                        std::memcpy(&G.Samples[(size_t)(R * Cells + I) * G.N + Q * Cells], &T.Samples[(size_t)(I + 1) * T.W + 1], (size_t)(Cells + 1) * 2);
                }
            if (Ok)
            {
                Counters["terrain_level"] = (size_t)C.Level;
                if (SeamAll) Counters["terrain_seams_equal_permille"] = SeamSame * 1000 / SeamAll;
                // the proof the tiles are put together right: the whole-grid tile's samples are
                // the level's at the same points (equal, or a few units off when the whole is a
                // filtered copy)
                if (Whole && (G.N - 1) % (Zero.W - 1) == 0)
                {
                    const uint32_t Ratio = (G.N - 1) / (Zero.W - 1);
                    size_t Same = 0, All2 = 0;
                    for (uint32_t R = 0; R < Zero.W; R++)
                        for (uint32_t Q = 0; Q < Zero.W; Q++, All2++)
                            Same += std::abs((int)G.Samples[(size_t)R * Ratio * G.N + Q * Ratio] - (int)Zero.Samples[(size_t)R * Zero.W + Q]) <= 2;
                    Counters["terrain_level_matches_whole_permille"] = Same * 1000 / std::max<size_t>(1, All2);
                }
                return true;
            }
            Counters["terrain_levels_incomplete"]++;
        }
        return false;
    }
}

namespace
{
    // the quadtree's nodes before its finest level (Side a side)
    uint64_t Total(uint32_t Side)
    {
        uint64_t T = 0;
        for (uint64_t L = 1; L < Side; L *= 2) T += L * L;
        return T;
    }

    // A buffer's bytes: in the zone (its size u32 0, bit 31 of u32 4) or its package object
    std::vector<uint8_t> BufferBytes(const IwTerrain::Source& Src, uint64_t Buffer, std::map<std::string, size_t>& Counters)
    {
        std::vector<uint8_t> Out;
        if (Src.Locate(Buffer, 0x28) == nullptr) return Out;
        if ((Get<uint32_t>(Src, Buffer + 4) >> 31) != 0)
        {
            const uint32_t Size = Get<uint32_t>(Src, Buffer);
            if (const uint8_t* P = Src.Locate(Get<uint64_t>(Src, Buffer + BufData), Size)) Out.assign(P, P + Size);
            return Out;
        }
        if (!Src.Streamed && CoDAssets::GamePackageCache == nullptr) return Out;
        uint32_t Got = 0;
        const uint64_t Key = Get<uint64_t>(Src, Buffer);
        const auto Data = Src.Streamed ? Src.Streamed(Key, Got) : CoDAssets::GamePackageCache->ExtractPackageObject(Key, Got);
        Counters["terrain_index_buffers_streamed"]++;
        if (Data != nullptr) Out.assign(Data.get(), Data.get() + Got);
        return Out;
    }

    // stterrain's partial nodes (its geometry struct at Geo, 0x1a0): index buffers (0x28 each,
    // count u32 0x14, at 8: u16 indices over a node's 33 x 33 points) and the nodes' lists (u64
    // each, count u32 0x2c, at 0x20: the node in bits 0-21, its first index (bits 46-63) into
    // its buffer; the buffers' lists one after the other, each from 0: a list ends where the next
    // starts). Only the finest level's are kept (the coarser ones draw the distance).
    void ReadPartial(const IwTerrain::Source& Src, uint64_t Geo, uint64_t First, uint32_t Side, std::map<uint32_t, std::vector<uint16_t>>& Out,
                     std::map<std::string, size_t>& Counters)
    {
        const uint32_t Buffers = Get<uint32_t>(Src, Geo + 0x14), Count = Get<uint32_t>(Src, Geo + 0x2c);
        const uint64_t BufferArray = Get<uint64_t>(Src, Geo + 8), Lists = Get<uint64_t>(Src, Geo + 0x20);
        if (Buffers == 0 || Count == 0 || Src.Locate(BufferArray, (uint64_t)Buffers * 0x28) == nullptr || Src.Locate(Lists, (uint64_t)Count * 8) == nullptr) return;
        struct Item
        {
            uint32_t Node, Start;
        };
        std::vector<std::vector<Item>> Groups(1);
        for (uint32_t K = 0; K < Count; K++)
        {
            const uint64_t V = Get<uint64_t>(Src, Lists + (uint64_t)K * 8);
            const Item I{ (uint32_t)(V & 0x3FFFFF), (uint32_t)(V >> 46) };
            if (!Groups.back().empty() && I.Start <= Groups.back().back().Start) Groups.emplace_back();
            Groups.back().push_back(I);
        }
        if (Groups.size() > Buffers)
        {
            Counters["terrain_partial_unread"]++;
            return;
        }
        const auto Finest = [&](uint32_t Node) { return Node >= First && Node - First < (uint64_t)Side * Side; };
        for (size_t G = 0; G < Groups.size(); G++)
        {
            if (std::none_of(Groups[G].begin(), Groups[G].end(), [&](const Item& I) { return Finest(I.Node); })) continue;
            const std::vector<uint8_t> Bytes = BufferBytes(Src, BufferArray + G * 0x28, Counters);
            const size_t Indices = Bytes.size() / 2;
            if (Indices == 0)
            {
                Counters["terrain_partial_buffers_unread"]++;
                continue;
            }
            for (size_t J = 0; J < Groups[G].size(); J++)
            {
                const Item& I = Groups[G][J];
                if (!Finest(I.Node)) continue;
                const size_t End = std::min(Indices, J + 1 < Groups[G].size() ? (size_t)Groups[G][J + 1].Start : Indices);
                if (I.Start >= End) continue;
                auto& List = Out[(uint32_t)(I.Node - First)];
                List.resize(End - I.Start);
                std::memcpy(List.data(), Bytes.data() + (size_t)I.Start * 2, List.size() * 2);
                Counters["terrain_partial_nodes"]++;
            }
        }
    }
}

namespace
{
    // The material layers: the geometry struct's layer
    // records (count u32 0xdc, its material at 0, world to uv scales x 0x10 and y 0x24: 1 / 128 on
    // mp_saw_sm_island), the vtmdata's index map (its tile set count u32 0x3c, sets 0x40 of 0x20:
    // tiles 0x18 of 0x10, each a buffer pointer; tile 0's second view an R8 map of the layer of each
    // texel, (cells + 4)^2 as the heightmap's tiles). Each layer used gets a mask (255 where it is)
    // written as a PNG beside the map, its colour the layer material's (slot 71: the terrain
    // materials' BC1 sRGB colour, 72 BC7, 74/75 BC4 not read; 82 a common material's colour).
    // an 8-bit grey PNG in memory (stb_image_write, compiled in Core's GLTFExport.cpp)
    bool GrayPng(ByteView Pixels, uint32_t W, uint32_t H, std::vector<uint8_t>& Out)
    {
        Out.clear();
        const auto Append = [](void* Context, void* Data, int Size) {
            auto* V = static_cast<std::vector<uint8_t>*>(Context);
            V->insert(V->end(), static_cast<uint8_t*>(Data), static_cast<uint8_t*>(Data) + Size);
        };
        return stbi_write_png_to_func(Append, &Out, (int)W, (int)H, 1, Pixels.data(), (int)W) != 0 && !Out.empty();
    }

    void AddLayers(Maps::MapScene& Scene, const IwTerrain::Source& Src, uint64_t Geo, const IwTerrain::Layout& L, Maps::Material& Mat,
                   std::map<std::string, size_t>& Counters)
    {
        const uint32_t Count = Get<uint32_t>(Src, Geo + 0xdc);
        const uint64_t Records = Get<uint64_t>(Src, Geo + (uint64_t)L.LayerList);
        if (Count == 0 || Count > 256 || Src.Locate(Records, (uint64_t)Count * (uint64_t)L.LayerSize) == nullptr) return;
        // the index map: tile 0 of the first set
        if (Get<uint32_t>(Src, Src.Vtm + 0x3c) == 0) return;
        const uint64_t Set = Get<uint64_t>(Src, Src.Vtm + 0x40);
        const uint64_t Buffer = Get<uint64_t>(Src, Get<uint64_t>(Src, Set + 0x18));
        if (Src.Locate(Buffer, 0x28) == nullptr || (Get<uint32_t>(Src, Buffer + 0x20) >> 9) < 2) return;
        const uint64_t View = Get<uint64_t>(Src, Buffer + 8) + 0x10;
        const uint32_t Size = Get<uint32_t>(Src, View), Offset = Get<uint32_t>(Src, View + 0xc);
        const uint16_t W = Get<uint16_t>(Src, View + 4), H = Get<uint16_t>(Src, View + 6);
        std::vector<uint8_t> Index;
        if ((Get<uint32_t>(Src, Buffer + 4) >> 31) != 0)
        {
            if (const uint8_t* P = Src.Locate(Get<uint64_t>(Src, Buffer + 0x10) + Offset, (uint64_t)W * H)) Index.assign(P, P + (size_t)W * H);
        }
        if (Index.empty() || W != H || W < 6 || Size < (uint64_t)W * H)
        {
            Counters["terrain_layers_unread"]++;
            return;
        }
        // the map's own texels (one before, two after)
        const uint32_t N = W - 3;
        std::vector<float> Uv;
        for (uint32_t K = 0; K < Count; K++)
        {
            std::vector<uint8_t> Mask((size_t)N * N, 0);
            size_t Used = 0;
            for (uint32_t R = 0; R < N; R++)
                for (uint32_t C = 0; C < N; C++)
                    if (Index[(size_t)(R + 1) * W + C + 1] == K) Mask[(size_t)R * N + C] = 255, Used++;
            const uint64_t Rec = Records + (uint64_t)K * (uint64_t)L.LayerSize;
            Uv.push_back(0), Uv.push_back(0);
            if (Used == 0) continue;
            const std::string Material = Src.LayerMaterial(Get<uint64_t>(Src, Rec));
            const auto It = Scene.Materials.find(Material);
            if (Material.empty() || It == Scene.Materials.end())
            {
                Counters["terrain_layer_materials_unread"]++;
                continue;
            }
            // the layer's world-to-uv scales over the grid's span (the grid's uv is 0..1)
            Uv[2 * K] = std::abs(Get<float>(Src, Rec + 0x10)) * 2.0f * Get<float>(Src, Src.Vtm + 0x14);
            Uv[2 * K + 1] = std::abs(Get<float>(Src, Rec + 0x24)) * 2.0f * Get<float>(Src, Src.Vtm + 0x18);
            if (FILE* Dbg = Maps::DebugFile())
                for (const Maps::Texture& T : It->second.Textures) std::fprintf(Dbg, "[terrain layer] %u %s %u %s\n", K, Material.c_str(), T.Semantic, T.Image.c_str());
            for (const Maps::Texture& T : It->second.Textures)
                if (T.Semantic == 71 || T.Semantic == 82) Mat.Textures.push_back({ IwTerrain::LayerSemantic + K * 4, T.Image });
            const std::string Name = std::format("{}_layer{}", Mat.Name, K);
            std::vector<uint8_t> Png;
            if (!GrayPng(Mask, N, N, Png)) continue;
            Scene.Images[Name].Png = std::move(Png);
            Mat.Textures.push_back({ IwTerrain::LayerSemantic + K * 4 + 2, Name });
            Counters["terrain_layers"]++;
        }
        Mat.Constants.assign(1, std::vector<uint8_t>(Uv.size() * 4));
        std::memcpy(Mat.Constants[0].data(), Uv.data(), Uv.size() * 4);
    }
}

namespace IwTerrain
{
    bool Read(Maps::MapScene& Scene, const Source& Src, uint64_t Vhm, uint64_t St, const Layout& L, const Options& O,
              std::map<std::string, size_t>& Counters)
    {
        if (Src.Locate(Vhm, 0x34) == nullptr) return false;
        Grid G;
        uint32_t Want = 1;
        while ((uint64_t)Want * 2 * Want * 2 <= O.MaxDrawnQuads) Want *= 2;
        if (!BuildGrid(Src, Vhm, L, Want, G, Counters) || G.N < 2)
        {
            if (Get<uint32_t>(Src, Vhm + VhmGrid) != 0) Counters["terrain_heightmap_unread"]++;
            return false;
        }
        const uint32_t W = G.N, H = G.N;
        const uint16_t* Samples = G.Samples.data();

        const float Cx = Get<float>(Src, Vhm + VhmCentre), Cy = Get<float>(Src, Vhm + VhmCentre + 4);
        const float Hx = Get<float>(Src, Vhm + VhmHalf), Hy = Get<float>(Src, Vhm + VhmHalf + 4);
        const float Offset = Get<float>(Src, Vhm + VhmOffset), Range = Get<float>(Src, Vhm + VhmRange);
        const float X0 = Cx - Hx, Y0 = Cy - Hy, Dx = 2 * Hx / (float)(W - 1), Dy = 2 * Hy / (float)(H - 1);
        const auto Sample = [&](uint32_t Row, uint32_t Col) { return Offset + (float)Samples[(size_t)Row * W + Col] / 65535.0f * Range; };

        // the finest nodes (the quadtree's last level, row-major, rows along y): a node is drawn
        // when its material record has a bit set (on mp_saw_sm_island 40 of the 1024 have none,
        // each with min = max: the ground the world geometry replaces)
        uint32_t Side = 0;
        std::vector<uint8_t> Drawn;
        if (St != 0 && Src.Locate(St, (uint64_t)L.Colors + 8) != nullptr)
        {
            const uint32_t Nodes = Get<uint32_t>(Src, St + (uint64_t)L.NodeCount), Records = Get<uint32_t>(Src, St + (uint64_t)L.RecordCount);
            uint64_t Total = 0, Level = 1;
            while (Total + Level * Level < Nodes) Total += Level * Level, Level *= 2;
            const uint64_t Index = Get<uint64_t>(Src, St + (uint64_t)L.NodeRecords), Recs = Get<uint64_t>(Src, St + (uint64_t)L.Records);
            if (Total + Level * Level == Nodes && Src.Locate(Index, (uint64_t)Nodes * 2) != nullptr && Src.Locate(Recs, (uint64_t)Records * 32) != nullptr)
            {
                Side = (uint32_t)Level;
                Drawn.assign((size_t)Side * Side, 0);
                for (uint32_t K = 0; K < Side * Side; K++)
                {
                    const uint16_t R = Get<uint16_t>(Src, Index + (Total + K) * 2);
                    const uint8_t* Bits = R < Records ? Src.Locate(Recs + (uint64_t)R * 32, 32) : nullptr;
                    Drawn[K] = Bits != nullptr && std::any_of(Bits, Bits + 32, [](uint8_t B) { return B != 0; });
                    Counters[Drawn[K] ? "terrain_nodes_drawn" : "terrain_nodes_hidden"]++;
                }
            }
            else
                Counters["terrain_nodes_unread"]++;
        }
        // the finest nodes the game draws in part: their own triangles over the node's 33 x 33 points
        std::map<uint32_t, std::vector<uint16_t>> Partial;
        if (Side != 0 && L.Geometry >= 0) ReadPartial(Src, St + (uint64_t)L.Geometry, Total(Side), Side, Partial, Counters);
        const auto NodeOf = [&](uint32_t Row, uint32_t Col) {
            const uint32_t R = std::min(Side - 1, (uint32_t)((uint64_t)Row * Side / (H - 1))), C = std::min(Side - 1, (uint32_t)((uint64_t)Col * Side / (W - 1)));
            return R * Side + C;
        };
        const auto QuadDrawn = [&](uint32_t Row, uint32_t Col) {
            if (Side == 0) return true;
            const uint32_t K = NodeOf(Row, Col);
            return Drawn[K] != 0 && !Partial.count(K);
        };

        // the grid every Step samples (the last row and column at the edge), each drawn quad as two
        // triangles, in meshes of at most MaxMeshQuads (ChunkQuads a side: "terrain_<row>_<col>",
        // one mesh "terrain" when it fits); normals from the heights around a point
        const uint32_t Step = StepFor(W, O.MaxDrawnQuads);
        std::vector<uint32_t> Coords;
        for (uint32_t K = 0; K < W - 1; K += Step) Coords.push_back(K);
        Coords.push_back(W - 1);
        const uint32_t M = (uint32_t)Coords.size();
        uint32_t ChunkQuads = 1;
        while ((uint64_t)ChunkQuads * 2 * ChunkQuads * 2 <= O.MaxMeshQuads) ChunkQuads *= 2;
        const uint32_t Chunks = (M - 1 + ChunkQuads - 1) / ChunkQuads;
        const std::string MaterialName = "terrain_" + Scene.Name;
        std::vector<Maps::Surface> Sfs((size_t)Chunks * Chunks);
        for (auto& One : Sfs) One.Material = MaterialName;
        std::vector<int64_t> Number((size_t)M * M, -1);
        const auto Id = [&](Maps::Surface& Sf, uint32_t I, uint32_t J) {
            int64_t& Slot = Number[(size_t)I * M + J];
            if (Slot >= 0) return (uint32_t)Slot;
            const uint32_t Row = Coords[I], Col = Coords[J];
            Maps::Vertex V;
            V.Pos[0] = X0 + (float)Col * Dx;
            V.Pos[1] = Y0 + (float)Row * Dy;
            V.Pos[2] = Sample(Row, Col);
            const uint32_t C0 = Col >= Step ? Col - Step : 0, C1 = std::min(W - 1u, Col + Step);
            const uint32_t R0 = Row >= Step ? Row - Step : 0, R1 = std::min(H - 1u, Row + Step);
            const float Gx = (Sample(Row, C1) - Sample(Row, C0)) / ((float)(C1 - C0) * Dx);
            const float Gy = (Sample(R1, Col) - Sample(R0, Col)) / ((float)(R1 - R0) * Dy);
            const float Len = std::sqrt(Gx * Gx + Gy * Gy + 1.0f);
            V.Normal[0] = -Gx / Len, V.Normal[1] = -Gy / Len, V.Normal[2] = 1.0f / Len;
            // the colour map covers the grid, its first row the grid's first (rows along y)
            V.Uv[0] = (float)Col / (float)(W - 1);
            V.Uv[1] = (float)Row / (float)(H - 1);
            Sf.Vertices.push_back(V);
            Slot = (int64_t)Sf.Vertices.size() - 1;
            return (uint32_t)Slot;
        };
        for (uint32_t Ci = 0; Ci < Chunks; Ci++)
            for (uint32_t Cj = 0; Cj < Chunks; Cj++)
            {
                Maps::Surface& Sf = Sfs[(size_t)Ci * Chunks + Cj];
                // a chunk numbers its own points (its edge points repeated by the next)
                const uint32_t I0 = Ci * ChunkQuads, I1 = std::min(M - 1, I0 + ChunkQuads), J0 = Cj * ChunkQuads, J1 = std::min(M - 1, J0 + ChunkQuads);
                for (uint32_t I = I0; I <= I1; I++)
                    for (uint32_t J = J0; J <= J1; J++) Number[(size_t)I * M + J] = -1;
                for (uint32_t I = I0; I < I1; I++)
                    for (uint32_t J = J0; J < J1; J++)
                    {
                        if (!QuadDrawn(Coords[I], Coords[J])) continue;
                        const uint32_t A = Id(Sf, I, J), B = Id(Sf, I, J + 1), C = Id(Sf, I + 1, J), D = Id(Sf, I + 1, J + 1);
                        // the game's front face is clockwise seen from above (Maps::Surface keeps the game's winding)
                        Sf.Indices.insert(Sf.Indices.end(), { A, D, B, A, C, D });
                    }
            }
        // the partial nodes, their points on the grid (bilinear when the grid is coarser than the
        // nodes' 32 cells), each in the chunk holding its first point
        const float Scale = Side != 0 ? (float)(W - 1) / (float)(Side * 32) : 1.0f;
        const auto Height = [&](float Row, float Col) {
            Row = std::clamp(Row, 0.0f, (float)(H - 1)), Col = std::clamp(Col, 0.0f, (float)(W - 1));
            const uint32_t R0 = std::min((uint32_t)Row, H - 2), C0 = std::min((uint32_t)Col, W - 2);
            const float Fr = Row - (float)R0, Fc = Col - (float)C0;
            return Sample(R0, C0) * (1 - Fr) * (1 - Fc) + Sample(R0, C0 + 1) * (1 - Fr) * Fc + Sample(R0 + 1, C0) * Fr * (1 - Fc) +
                   Sample(R0 + 1, C0 + 1) * Fr * Fc;
        };
        for (const auto& [K, List] : Partial)
        {
            if (!Drawn[K]) continue;
            const uint32_t NodeRow = K / Side, NodeCol = K % Side;
            const uint32_t Ci = std::min(Chunks - 1, (uint32_t)((float)(NodeRow * 32) * Scale / (float)Step) / ChunkQuads);
            const uint32_t Cj = std::min(Chunks - 1, (uint32_t)((float)(NodeCol * 32) * Scale / (float)Step) / ChunkQuads);
            Maps::Surface& Sf = Sfs[(size_t)Ci * Chunks + Cj];
            std::map<uint16_t, uint32_t> Local;
            const auto Point = [&](uint16_t Vtx) {
                if (const auto It = Local.find(Vtx); It != Local.end()) return It->second;
                const float Row = (float)(NodeRow * 32 + Vtx / 33) * Scale, Col = (float)(NodeCol * 32 + Vtx % 33) * Scale;
                Maps::Vertex V;
                V.Pos[0] = X0 + Col * Dx;
                V.Pos[1] = Y0 + Row * Dy;
                V.Pos[2] = Height(Row, Col);
                const float Gx = (Height(Row, Col + 1) - Height(Row, Col - 1)) / (2 * Dx), Gy = (Height(Row + 1, Col) - Height(Row - 1, Col)) / (2 * Dy);
                const float Len = std::sqrt(Gx * Gx + Gy * Gy + 1.0f);
                V.Normal[0] = -Gx / Len, V.Normal[1] = -Gy / Len, V.Normal[2] = 1.0f / Len;
                V.Uv[0] = Col / (float)(W - 1);
                V.Uv[1] = Row / (float)(H - 1);
                Sf.Vertices.push_back(V);
                return Local[Vtx] = (uint32_t)Sf.Vertices.size() - 1;
            };
            for (size_t T = 0; T + 2 < List.size(); T += 3)
            {
                if (List[T] >= 33 * 33 || List[T + 1] >= 33 * 33 || List[T + 2] >= 33 * 33) continue;
                // the game's winding (clockwise seen from above, as the grid's)
                const uint32_t A = Point(List[T]), B = Point(List[T + 1]), C = Point(List[T + 2]);
                Sf.Indices.insert(Sf.Indices.end(), { A, B, C });
                Counters["terrain_partial_triangles"]++;
            }
        }
        size_t Triangles = 0;
        for (const auto& One : Sfs) Triangles += One.Triangles();
        if (Triangles == 0) return false;
        Counters["terrain_triangles"] += Triangles;
        Counters["terrain_grid"] = W;
        Counters["terrain_step"] = Step;
        Counters["terrain_meshes"] = (size_t)std::count_if(Sfs.begin(), Sfs.end(), [](const Maps::Surface& One) { return !One.Indices.empty(); });

        Maps::Material& Mat = Scene.Materials[MaterialName];
        Mat.Name = MaterialName;
        if (Src.ImageName && St != 0 && L.Colors != 0 && Get<uint32_t>(Src, St + (uint64_t)L.ColorCount) != 0)
            if (const uint64_t Colors = Get<uint64_t>(Src, St + (uint64_t)L.Colors); Src.Locate(Colors + 8, 8) != nullptr)
                if (const std::string Name = Src.ImageName(Get<uint64_t>(Src, Colors + 8)); !Name.empty())
                {
                    Mat.ColorMap = Name;
                    Mat.Textures.push_back({ 0, Name });
                    Scene.Images[Name];
                }
        if (Side != 0 && L.Geometry >= 0 && L.LayerSize > 0 && Src.Vtm != 0 && Src.LayerMaterial)
            AddLayers(Scene, Src, St + (uint64_t)L.Geometry, L, Mat, Counters);
        for (uint32_t K = 0; K < Sfs.size(); K++)
        {
            if (Sfs[K].Indices.empty()) continue;
            Maps::Mesh Mesh;
            Mesh.Name = Chunks == 1 ? std::string("terrain") : std::format("terrain_{}_{}", K / Chunks, K % Chunks);
            Mesh.Surfaces.push_back(std::move(Sfs[K]));
            Scene.Objects.push_back(std::move(Mesh));
        }
        return true;
    }

    bool DescribeLayers(const Materials::Source& M, Materials::Description& D)
    {
        if (M.BySemantic(LayerSemantic + 2) == nullptr && M.BySemantic(LayerSemantic + 4 + 2) == nullptr) return false;
        std::vector<float> Uv;
        if (!M.Constants.empty() && M.Constants[0].size() % 4 == 0)
        {
            Uv.resize(M.Constants[0].size() / 4);
            std::memcpy(Uv.data(), M.Constants[0].data(), M.Constants[0].size());
        }
        // the baked colour first (what a layer's mask leaves), then each layer over it
        Materials::Layer Base;
        Base.Albedo = D.Albedo;
        D.Layers.push_back(Base);
        for (uint32_t K = 0; K < 256; K++)
        {
            const auto* Color = M.BySemantic(LayerSemantic + K * 4);
            const auto* Nog = M.BySemantic(LayerSemantic + K * 4 + 1);
            const auto* Mask = M.BySemantic(LayerSemantic + K * 4 + 2);
            if (Mask == nullptr || (Color == nullptr && Nog == nullptr)) continue;
            Materials::Layer Ly;
            if (Color) Ly.Albedo = { Color->Path, 0, false };
            if (Nog) Ly.Roughness = { Nog->Path, 0, true };
            Ly.Mask = { Mask->Path, 0, false };
            if (2 * K + 1 < Uv.size()) Ly.UVScale[0] = Uv[2 * K], Ly.UVScale[1] = Uv[2 * K + 1];
            D.Layers.push_back(Ly);
        }
        if (D.Layers.size() < 2) D.Layers.clear();
        return !D.Layers.empty();
    }
}
