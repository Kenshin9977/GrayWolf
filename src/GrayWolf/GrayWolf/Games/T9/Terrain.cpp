#include "stdafx.h"
#include "ByteView.h"

// Cold War's terraingfx: a surface's grid point (c, r)
// is at its origin + (c, r) * cell, its height the heightmap's sample (bilinear) on the grid of the
// surface owning the heightmap (the largest cells among those sharing it), offset - translation z
// + sample / 65535 * range; the visibility bitmap gives the cells drawn (8 x 4 per texel).

#include "Terrain.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace
{
    // terraingfx: surface count u32 at 8, surfaces at 0x10. A surface: translation 0x50, origin
    // 0xe8, cell 0xf0, height offset 0xf4 and range 0xf8, grid size u16 0x100; images: heightmap
    // 0x168 (R16_UNORM), visibility 0x170 (R32_UINT), colour map 0x198 (one texel per point)
    constexpr int64_t SurfaceCount = 8, Surfaces = 0x10, SurfaceSize = 0x1d8;
    constexpr int64_t Translation = 0x50, Origin = 0xe8, Cell = 0xf0, HeightOffset = 0xf4, HeightRange = 0xf8, GridSize = 0x100;
    constexpr int64_t HeightMap = 0x168, Visibility = 0x170, ColorMap = 0x198;

    template <typename T> T At(ByteView Buf, int64_t Pos)
    {
        T V{};
        if (Pos >= 0 && (uint64_t)Pos + sizeof(T) <= Buf.size()) std::memcpy(&V, Buf.data() + Pos, sizeof(T));
        return V;
    }

    struct Surface
    {
        int64_t Pos = -1, HeightImage = -1;
        float OriginX = 0, OriginY = 0, CellSize = 0, Base = 0, Range = 0;
        uint32_t Size = 0;
    };

    struct Grid
    {
        const Surface* S = nullptr;
        const Surface* Owner = nullptr;   // whose grid the heightmap's samples follow
        const std::vector<uint8_t>* Heights = nullptr;
        uint32_t HeightSize = 0;
        std::vector<uint8_t> Visible;   // the bitmap's texels; empty: all drawn

        float Height(float X, float Y) const
        {
            const float U = std::clamp((X - Owner->OriginX) / Owner->CellSize, 0.0f, (float)(HeightSize - 1));
            const float V = std::clamp((Y - Owner->OriginY) / Owner->CellSize, 0.0f, (float)(HeightSize - 1));
            const uint32_t U0 = (uint32_t)U, V0 = (uint32_t)V, U1 = std::min(U0 + 1, HeightSize - 1), V1 = std::min(V0 + 1, HeightSize - 1);
            const float Fu = U - (float)U0, Fv = V - (float)V0;
            const auto H = [&](uint32_t Col, uint32_t Row) { return (float)At<uint16_t>(*Heights, ((int64_t)Row * HeightSize + Col) * 2); };
            const float Sample = H(U0, V0) * (1 - Fu) * (1 - Fv) + H(U1, V0) * Fu * (1 - Fv) + H(U0, V1) * (1 - Fu) * Fv + H(U1, V1) * Fu * Fv;
            return S->Base + Sample / 65535.0f * S->Range;
        }
        bool Drawn(uint32_t Row, uint32_t Col) const
        {
            if (Visible.empty()) return true;
            const int64_t Texel = (int64_t)(Row / 4) * (S->Size / 8) + Col / 8;
            return (At<uint32_t>(Visible, Texel * 4) >> ((Row % 4) * 8 + Col % 8)) & 1;
        }
        // every cell of [Row, Row + Step) x [Col, Col + Step) drawn
        bool QuadDrawn(uint32_t Row, uint32_t Col, uint32_t Step) const
        {
            for (uint32_t R = Row; R < std::min(Row + Step, S->Size - 1); R++)
                for (uint32_t C = Col; C < std::min(Col + Step, S->Size - 1); C++)
                    if (!Drawn(R, C)) return false;
            return true;
        }
    };

    uint32_t StepFor(uint32_t Size, uint32_t MaxQuads)
    {
        uint32_t Step = 1;
        while ((uint64_t)((Size - 1) / Step) * ((Size - 1) / Step) > MaxQuads) Step *= 2;
        return Step;
    }

    // The grid's points every Step samples (the last row and column at the edge), each drawn quad
    // as two triangles Emit(a, b, c), counter-clockwise seen from above
    template <class FPoint, class FTri>
    void Walk(const Grid& G, uint32_t Step, FPoint Point, FTri Emit)
    {
        const uint32_t N = G.S->Size;
        std::vector<uint32_t> Coords;
        for (uint32_t K = 0; K < N - 1; K += Step) Coords.push_back(K);
        Coords.push_back(N - 1);
        const uint32_t M = (uint32_t)Coords.size();
        std::vector<int64_t> Number((size_t)M * M, -1);
        const auto Id = [&](uint32_t I, uint32_t J) {
            int64_t& Slot = Number[(size_t)I * M + J];
            if (Slot < 0) Slot = Point(Coords[I], Coords[J]);
            return (uint32_t)Slot;
        };
        for (uint32_t I = 0; I + 1 < M; I++)
            for (uint32_t J = 0; J + 1 < M; J++)
            {
                if (!G.QuadDrawn(Coords[I], Coords[J], Coords[J + 1] - Coords[J])) continue;
                const uint32_t A = Id(I, J), B = Id(I, J + 1), C = Id(I + 1, J), D = Id(I + 1, J + 1);
                Emit(A, B, D);
                Emit(A, D, C);
            }
    }
}

namespace ColdWar
{
    bool ReadTerrain(Maps::MapScene& Scene, const TerrainSource& Src, int64_t Terrain, const TerrainOptions& Options,
                     std::map<std::string, size_t>& Counters)
    {
        const auto& Img = *Src.Image;
        const uint32_t Count = At<uint32_t>(Img, Terrain + SurfaceCount);
        const int64_t Array = Src.Off(At<uint64_t>(Img, Terrain + Surfaces));
        if (Count == 0 || Array < 0) return false;

        std::vector<Surface> List(Count);
        for (uint32_t K = 0; K < Count; K++)
        {
            Surface& S = List[K];
            S.Pos = Array + (int64_t)K * SurfaceSize;
            S.OriginX = At<float>(Img, S.Pos + Origin);
            S.OriginY = At<float>(Img, S.Pos + Origin + 4);
            S.CellSize = At<float>(Img, S.Pos + Cell);
            S.Base = At<float>(Img, S.Pos + HeightOffset) - At<float>(Img, S.Pos + Translation + 8);
            S.Range = At<float>(Img, S.Pos + HeightRange);
            S.Size = At<uint16_t>(Img, S.Pos + GridSize);
            S.HeightImage = Src.Off(At<uint64_t>(Img, S.Pos + HeightMap));
        }
        std::map<int64_t, std::vector<uint8_t>> Heights;   // by image
        std::map<int64_t, uint32_t> HeightSizes;
        bool Any = false;
        for (uint32_t K = 0; K < Count; K++)
        {
            const Surface& S = List[K];
            Grid G;
            G.S = &S;
            G.Owner = &S;
            for (const Surface& O : List)
                if (O.HeightImage == S.HeightImage && O.CellSize > G.Owner->CellSize) G.Owner = &O;
            if (G.Owner != &S) Counters["terrain_heightmap_shared"]++;
            if (S.HeightImage >= 0 && !Heights.count(S.HeightImage))
            {
                std::vector<uint8_t> Pixels;
                uint32_t W = 0, H = 0;
                if (Src.Pixels(S.HeightImage, Pixels, W, H) && W == H && W > 1 && Pixels.size() >= (size_t)W * H * 2)
                {
                    Heights[S.HeightImage] = std::move(Pixels);
                    HeightSizes[S.HeightImage] = W;
                }
            }
            const auto Found = Heights.find(S.HeightImage);
            if (S.Size < 2 || S.Size % 8 != 0 || S.CellSize <= 0 || Found == Heights.end())
            {
                Counters["terrain_surfaces_unread"]++;
                continue;
            }
            G.Heights = &Found->second;
            G.HeightSize = HeightSizes[S.HeightImage];
            if (const int64_t VisImage = Src.Off(At<uint64_t>(Img, S.Pos + Visibility)); VisImage >= 0)
            {
                uint32_t W = 0, H = 0;
                if (!Src.Pixels(VisImage, G.Visible, W, H) || W * 8 != S.Size || H * 4 != S.Size || G.Visible.size() < (size_t)W * H * 4)
                {
                    G.Visible.clear();
                    Counters["terrain_visibility_unread"]++;
                }
            }
            const auto Position = [&](uint32_t Row, uint32_t Col, float P[3]) {
                P[0] = S.OriginX + (float)Col * S.CellSize;
                P[1] = S.OriginY + (float)Row * S.CellSize;
                P[2] = G.Height(P[0], P[1]);
            };

            // drawn: the colour map over the grid, normals from the heights around a point
            Maps::Mesh Mesh;
            Mesh.Name = std::format("terrain_{}", K);
            Maps::Surface Sf;
            Sf.Material = std::format("terrain_{}_{}", Scene.Name, K);
            const uint32_t Step = StepFor(S.Size, Options.MaxDrawnQuads);
            const float D = S.CellSize * (float)Step;
            Walk(G, Step,
                 [&](uint32_t Row, uint32_t Col) {
                     Maps::Vertex V;
                     Position(Row, Col, V.Pos);
                     const float Dx = G.Height(V.Pos[0] + D, V.Pos[1]) - G.Height(V.Pos[0] - D, V.Pos[1]);
                     const float Dy = G.Height(V.Pos[0], V.Pos[1] + D) - G.Height(V.Pos[0], V.Pos[1] - D);
                     const float Nx = -Dx / (2 * D), Ny = -Dy / (2 * D), L = std::sqrt(Nx * Nx + Ny * Ny + 1.0f);
                     V.Normal[0] = Nx / L, V.Normal[1] = Ny / L, V.Normal[2] = 1.0f / L;
                     V.Uv[0] = ((float)Col + 0.5f) / (float)S.Size;
                     V.Uv[1] = ((float)Row + 0.5f) / (float)S.Size;
                     Sf.Vertices.push_back(V);
                     return (int64_t)Sf.Vertices.size() - 1;
                 },
                 // the game's front face is clockwise (Maps::Surface keeps the game's winding)
                 [&](uint32_t A, uint32_t B, uint32_t C) { Sf.Indices.insert(Sf.Indices.end(), { A, C, B }); });
            if (Sf.Indices.empty()) continue;
            Counters["terrain_triangles"] += Sf.Triangles();
            Counters["terrain_surfaces"]++;
            Any = true;
            Maps::Material& Mat = Scene.Materials[Sf.Material];
            Mat.Name = Sf.Material;
            if (const int64_t Color = Src.Off(At<uint64_t>(Img, S.Pos + ColorMap)); Color >= 0)
                if (const std::string Name = Src.ImageName(Color); !Name.empty())
                {
                    Mat.ColorMap = Name;
                    Mat.Textures.push_back({ 0, Name });
                    Scene.Images[Name];
                }
            Mesh.Surfaces.push_back(std::move(Sf));
            Scene.Objects.push_back(std::move(Mesh));

            // collision: the same grid, coarser, facing up
            auto& Out = Scene.Collision;
            const size_t Before = Out.Triangles.size() / 3;
            Walk(G, StepFor(S.Size, Options.MaxCollisionQuads),
                 [&](uint32_t Row, uint32_t Col) {
                     float P[3];
                     Position(Row, Col, P);
                     Out.Vertices.insert(Out.Vertices.end(), { P[0], P[1], P[2] });
                     return (int64_t)(Out.Vertices.size() / 3) - 1;
                 },
                 [&](uint32_t A, uint32_t B, uint32_t C) { Out.Triangles.insert(Out.Triangles.end(), { A, B, C }); });
            Counters["terrain_collision_triangles"] += Out.Triangles.size() / 3 - Before;
        }
        return Any;
    }
}
