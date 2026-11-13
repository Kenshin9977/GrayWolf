#include "stdafx.h"

// The level of a Modern Warfare 2 or 3 map zone, read from the zone the iw4 / iw5 (64-bit rebuild)
// or iw4x86 (the original) tables walked. Every offset is one the game's loader reads
// (tables.generated.txt: gfx_map, com_map, col_map_sp, map_ents and the tables they call); where
// the games and builds differ, Layout::Map says.

#include "Games/IW4/IwDiskZone.h"

#include "ExportNaming.h"
#include "MapLive.h"

#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace Iw64
{
    namespace
    {
        // GfxSky: skySurfCount (i32) at 0; srfTriangles_t at the start of a GfxSurface {vertexLayerData,
        // firstVertex 4, vertexCount u16 8, triCount u16 10, baseIndex 12}
        constexpr int64_t SkySurfCount = 0, SurfaceFirstVertex = 4, SurfaceTris = 10, SurfaceBaseIndex = 12;
        // GfxWorldDpvsStatic: smodelCount 0
        constexpr int64_t DpvsSModelCount = 0;
        // GfxBrushModel (60 bytes, no pointer): surfaceCount u16 at 52, startSurfIndex u16 at 54
        constexpr int64_t BrushSize = 60, BrushSurfaceCount = 52, BrushFirstSurface = 54;
        // GfxWorldVertex (44 bytes): xyz 0, binormalSign 12, color 16, texCoord 20, lmapCoord 28,
        // normal 36 (PackedUnitVector), tangent 40
        constexpr int64_t VertexSize = 44, VertexColor = 16, VertexUv = 20, VertexNormal = 36;
        // ComPrimaryLight: color 4, dir 16
        constexpr int64_t LightColor = 4, LightDir = 16;

        // MW3's entity string names a key by its number among the game's constant script strings
        // (MW2 quotes the name). C2M's table: 1668 holds worldspawn and the
        // script_ classes, 1669 and 1677 three numbers (an origin, angles), 1670 the names of xmodels
        // the zone defines (the map_entity_models count)
        const std::pair<const char*, const char*> NumberedKeys[] = {
            { "1668", "classname" }, { "1669", "origin" }, { "1670", "model" }, { "1677", "angles" } };

        // PackedUnitVector (IW3-IW5): x, y, z around 127, scaled by the fourth byte
        void UnpackNormal(uint32_t Packed, float Out[3])
        {
            const float Scale = ((float)((Packed >> 24) & 0xff) + 192.0f) / 32385.0f;
            for (int K = 0; K < 3; K++) Out[K] = ((float)((Packed >> (8 * K)) & 0xff) - 127.0f) * Scale;
        }

        // clipMap_t's brushes (the tables' col_map_sp). The rebuild: MW2 keeps them in clipMap_t
        // (materials 0x28/0x30, leaf brush nodes 0x78/0x80, brushes u16 0x110 / 0x118, Bounds 0x120,
        // contents 0x128; brush models 0x100/0x108, cmodel_t 0x44), MW3 in a ClipInfo at 0x10
        // (materials 0x10/0x18, nodes 0x40/0x48, brushes 0x60/0x68, Bounds 0x70, contents 0x78; brush
        // models 0x120/0x128, cmodel_t 0x50 with a ClipInfo pointer at 0x20); ClipMaterial 0x10 (name,
        // surface 8, contents 0xc), cbrush_t 0x30 (u16 numsides, sides 8, axial material numbers
        // 0x18), cbrushside_t 0x10 (plane, u16 material number 8), cLeafBrushNode_s 0x18.
        Maps::ClipLayout ClipOf(const Layout& G)
        {
            Maps::ClipLayout C;
            if (G.PointerSize == 4)
            {
                if (&G == &Mw3x86)
                {
                    // the original MW3 (iw5x86, clipMap_t 0x100): a 0x40-byte ClipInfo at 8 (materials
                    // 8/0xc, leaf brush nodes 0x20/0x24, brushes u16 0x30, records 0x34, Bounds 0x38,
                    // contents 0x3c), collision vertices 0x64/0x68 and u16 triangles 0x6c/0x70, brush
                    // models 0x90/0x94 (cmodel_t 0x48, its ClipInfo pointer at 0x1c, its leaf at 0x20)
                    C.InfoAt = 8;
                    C.Info.MaterialCount = 8, C.Info.Materials = 0xc, C.Info.BrushCount = 0x30, C.Info.Brushes = 0x34;
                    C.Info.BrushBounds = 0x38, C.Info.BrushContents = 0x3c, C.Info.LeafBrushNodeCount = 0x20, C.Info.LeafBrushNodes = 0x24;
                    C.ModelCount = 0x90, C.Models = 0x94, C.ModelSize = 0x48, C.ModelInfo = 0x1c, C.ModelLeafBrushNode = 0x20 + 0x24;
                    C.VertexCount = 0x64, C.Vertices = 0x68, C.TriangleCount = 0x6c, C.Triangles = 0x70;
                }
                else
                {
                    // the original MW2 (iw4x86, clipMap_t 0x100): materials 0x18/0x1c, leaf brush nodes
                    // 0x40/0x44, collision vertices 0x58/0x5c and u16 triangles 0x60/0x64, brush models
                    // 0x84/0x88 (cmodel_t 0x44), brushes u16 0x8c, records 0x90, Bounds 0x94, contents 0x98
                    C.Info.MaterialCount = 0x18, C.Info.Materials = 0x1c, C.Info.BrushCount = 0x8c, C.Info.Brushes = 0x90;
                    C.Info.BrushBounds = 0x94, C.Info.BrushContents = 0x98, C.Info.LeafBrushNodeCount = 0x40, C.Info.LeafBrushNodes = 0x44;
                    C.ModelCount = 0x84, C.Models = 0x88, C.ModelSize = 0x44, C.ModelLeafBrushNode = 0x1c + 0x24;
                    C.VertexCount = 0x58, C.Vertices = 0x5c, C.TriangleCount = 0x60, C.Triangles = 0x64;
                }
                // both: ClipMaterial 0xc (name, surface 4, contents 8), cbrush_t 0x24 (u16 numsides,
                // sides 4, axial material numbers 0xc), cbrushside_t 8 (plane, u16 material number 4),
                // cLeafBrushNode_s 0x14
                C.MaterialSize = 0xc, C.MaterialName = 0, C.MaterialSurface = 4, C.MaterialContents = 8;
                C.BrushSize = 0x24, C.BrushSideCount = 0, C.BrushSideCountBytes = 2, C.BrushSides = 4, C.AxialMaterial = 0xc;
                C.SideSize = 8, C.SidePlane = 0, C.SideMaterial = 4, C.SideMaterialBytes = 2;
                C.NodeSize = 0x14;
                return C;
            }
            if (&G == &Mw3)
            {
                C.InfoAt = 0x10;
                C.Info.MaterialCount = 0x10, C.Info.Materials = 0x18, C.Info.BrushCount = 0x60, C.Info.Brushes = 0x68;
                C.Info.BrushBounds = 0x70, C.Info.BrushContents = 0x78, C.Info.LeafBrushNodeCount = 0x40, C.Info.LeafBrushNodes = 0x48;
                C.ModelCount = 0x120, C.Models = 0x128, C.ModelSize = 0x50, C.ModelInfo = 0x20, C.ModelLeafBrushNode = 0x28 + 0x24;
                C.VertexCount = 0xc8, C.Vertices = 0xd0, C.TriangleCount = 0xd8, C.Triangles = 0xe0;
            }
            else
            {
                C.Info.MaterialCount = 0x28, C.Info.Materials = 0x30, C.Info.BrushCount = 0x110, C.Info.Brushes = 0x118;
                C.Info.BrushBounds = 0x120, C.Info.BrushContents = 0x128, C.Info.LeafBrushNodeCount = 0x78, C.Info.LeafBrushNodes = 0x80;
                C.ModelCount = 0x100, C.Models = 0x108, C.ModelSize = 0x44, C.ModelLeafBrushNode = 0x1c + 0x24;
                C.VertexCount = 0xa8, C.Vertices = 0xb0, C.TriangleCount = 0xb8, C.Triangles = 0xc0;
            }
            C.MaterialSize = 0x10, C.MaterialName = 0, C.MaterialSurface = 8, C.MaterialContents = 0xc;
            C.BrushSize = 0x30, C.BrushSideCount = 0, C.BrushSideCountBytes = 2, C.BrushSides = 8, C.AxialMaterial = 0x18;
            C.SideSize = 0x10, C.SidePlane = 0, C.SideMaterial = 8, C.SideMaterialBytes = 2;
            C.NodeSize = 0x18;
            return C;
        }
    }

    bool IwDiskZone::ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models)
    {
        const int64_t World = Root("gfx_map");
        if (World < 0) return false;
        const Maps::ClipLayout Clip = ClipOf(*Game.Readers);
        const Layout::MapLayout& M = Game.Readers->Map;
        const auto& Img = Image();
        FILE* Dbg = Maps::DebugFile();
        Scene.Name = StringAt(PointerAt(World + M.WorldBaseName));
        if (Scene.Name.empty()) Scene.Name = Name();

        // ---- world geometry: the brush models' surfaces (model 0 is the level)
        const uint32_t VertexCount = At<uint32_t>(Img, World + M.VertexCount), IndexCount = At<uint32_t>(Img, World + M.IndexCount);
        const int64_t Vertices = Off(PointerAt(World + M.Vertices)), Indices = Off(PointerAt(World + M.Indices));
        const int64_t Dpvs = World + M.Dpvs;
        Maps::WorldLayout L;
        L.Image = &Img;
        L.Surfaces = Off(PointerAt(Dpvs + M.DpvsSurfaces)), L.SurfaceCount = At<uint32_t>(Img, World + M.WorldSurfaceCount);
        L.Brushes = Off(PointerAt(World + M.Brushes)), L.BrushCount = (uint32_t)std::max(At<int32_t>(Img, World + M.BrushCount), 0);
        L.Indices = Indices, L.IndexCount = IndexCount;
        L.SurfaceSize = M.SurfaceSize, L.FirstVertex = SurfaceFirstVertex, L.TriCount = SurfaceTris, L.BaseIndex = SurfaceBaseIndex;
        L.Material = M.SurfaceMaterial;
        L.BrushSize = BrushSize, L.FirstSurface = BrushFirstSurface, L.SurfaceCountAt = BrushSurfaceCount, L.BrushFieldBytes = 2;
        L.PointerSize = (int)Game.Readers->PointerSize;
        if (Dbg)
            std::fprintf(Dbg, "[map] %s vertices %u at %llx indices %u at %llx surfaces %u at %llx brushes %u at %llx (model 0: %u from %u)\n",
                         Scene.Name.c_str(), VertexCount, (long long)Vertices, IndexCount, (long long)Indices, L.SurfaceCount,
                         (long long)L.Surfaces, L.BrushCount, (long long)L.Brushes,
                         L.Brushes >= 0 ? (unsigned)At<uint16_t>(Img, L.Brushes + BrushSurfaceCount) : 0u,
                         L.Brushes >= 0 ? (unsigned)At<uint16_t>(Img, L.Brushes + BrushFirstSurface) : 0u);
        const auto Fetch = [&](uint32_t I, Maps::Vertex& V) {
            if (I >= VertexCount || Vertices < 0) return false;
            const int64_t P = Vertices + (int64_t)I * VertexSize;
            for (int K = 0; K < 3; K++) V.Pos[K] = At<float>(Img, P + K * 4);
            const uint32_t Color = At<uint32_t>(Img, P + VertexColor);
            std::memcpy(V.Color, &Color, 4);
            V.Uv[0] = At<float>(Img, P + VertexUv);
            V.Uv[1] = At<float>(Img, P + VertexUv + 4);
            UnpackNormal(At<uint32_t>(Img, P + VertexNormal), V.Normal);
            return true;
        };
        const auto MaterialOf = [&](uint64_t Address) {
            return Off(Address) < 0 ? std::string() : Maps::AddMaterial(Scene, ReadXMaterial(Address, *Game.Readers));
        };
        // the skies' surfaces (GfxSky: skySurfCount, skyStartSurfs): the box drawn with the sky
        // image, which the import script puts in the world's background instead
        std::unordered_set<uint32_t> SkySurfaces;
        const int32_t SkyCount = At<int32_t>(Img, World + M.WorldSkyCount);
        const int64_t Skies = Off(PointerAt(World + M.WorldSkies));
        for (int32_t K = 0; Skies >= 0 && K < SkyCount; K++)
        {
            const int64_t Sky = Skies + (int64_t)K * M.SkySize;
            const int64_t List = Off(PointerAt(Sky + M.SkySurfs));
            for (int32_t J = 0, N = At<int32_t>(Img, Sky + SkySurfCount); List >= 0 && J < N; J++)
                SkySurfaces.insert(At<uint32_t>(Img, List + (int64_t)J * 4));
        }
        for (const uint32_t S : SkySurfaces)
            if (L.Surfaces >= 0 && S < L.SurfaceCount)
                Counter("map_sky_triangles_skipped") += At<uint16_t>(Img, L.Surfaces + (int64_t)S * M.SurfaceSize + SurfaceTris);
        L.Skip = &SkySurfaces;
        if (const size_t Bad = Maps::AddWorld(Scene, L, Fetch, MaterialOf)) Counter("map_triangles_unread") += Bad;

        AddStaticModels(Scene, Models, Off(PointerAt(Dpvs + M.DpvsSModels)), At<uint32_t>(Img, Dpvs + DpvsSModelCount),
                        M.SModelSize, M.SModelModel);

        // ---- dynamic entities (the clip map, two lists: DynEntityDef, its pose and model), lights:
        // col_map_sp, or col_map_mp in a multiplayer zone (MW2's other type; MW3's one type as the
        // multiplayer exe names it)
        AddDynamicEntities(Scene, Models, Root("col_map_sp") >= 0 ? "col_map_sp" : "col_map_mp", M.DynCount, M.Dyns, M.DynSize);
        LightLayout Lights;
        Lights.Size = M.LightSize, Lights.Color = LightColor, Lights.Dir = LightDir, Lights.Origin = M.LightOrigin;
        Lights.Radius = M.LightRadius, Lights.CosHalfFov = M.LightCosOuter, Lights.Def = M.LightSize - Game.Readers->PointerSize;
        Lights.Count = M.LightCount, Lights.Array = M.Lights;
        AddPrimaryLights(Scene, Lights);

        // ---- the sky: the first sky's image
        if (At<int32_t>(Img, World + M.WorldSkyCount) > 0)
            if (const int64_t Sky = Off(PointerAt(World + M.WorldSkies)); Sky >= 0)
                if (const int64_t SkyImg = Off(PointerAt(Sky + M.SkyImage)); SkyImg >= 0)
                    if (const std::string N = StringAt(PointerAt(SkyImg + Game.Readers->Image.Name)); !N.empty())
                    {
                        Scene.Sky.Image = N;
                        Scene.Images[N];
                    }

        // ---- the entity string, and the models its script and misc models place
        AddMapEnts(Scene);
        std::vector<Maps::Entity> Ents;
        const bool Parsed = !Scene.MapEnts.empty() && Maps::ParseEntities(Scene.MapEnts, Ents);
        if (Dbg) std::fprintf(Dbg, "[map] entity string %zu bytes, %zu entities, parsed %d\n", Scene.MapEnts.size(), Ents.size(), (int)Parsed);
        for (const auto& [Number, Key] : NumberedKeys) Scene.EntityKeys[Number] = Key;
        if (Clip.BrushSize) AddCollision(Scene, { "col_map_sp", "col_map_mp" }, Clip);
        AddTriggers(Scene, M.Triggers);   // MapEnts' MapTriggers
        for (auto& E : Ents)   // MW3's numbered keys, by name
            for (const auto& [Number, Key] : NumberedKeys)
                if (const auto It = E.find(Number); It != E.end())
                {
                    E[Key] = It->second;
                    E.erase(It);
                }
        if (Parsed)
        {
            std::unordered_map<std::string, uint64_t> InZone;   // lowered name -> the model defined here
            for (const auto& I : Walk().Instances)
            {
                if (!OwnerIs(I, "xmodel")) continue;
                const std::string N = AssetName(DiskGame::Kind::Model, I.Pos);
                if (!IsReference(DiskGame::Kind::Model, N, I.Pos)) InZone.emplace(ExportNaming::Lower(N), Live().Addr((uint64_t)I.Pos));
            }
            for (size_t K = 0; K < Ents.size(); K++)
            {
                const auto& E = Ents[K];
                const auto Get = [&](const char* Key) { const auto It = E.find(Key); return It == E.end() ? std::string() : It->second; };
                const std::string Class = Get("classname"), Model = Get("model");
                if (Dbg && !Model.empty()) std::fprintf(Dbg, "[ent] %zu %s %s\n", K, Class.c_str(), Model.c_str());
                if (Model.empty() || Model[0] == '*' || (Class != "script_model" && Class != "misc_model")) continue;
                Maps::Instance In;
                In.Name = "ent_" + std::to_string(K);
                In.Model = Model;
                Maps::EntityFloats(E, "origin", In.Origin, 3);
                float Angles[3] = { 0, 0, 0 };
                Maps::EntityFloats(E, "angles", Angles, 3);
                Maps::AnglesToAxis(Angles, In.Axis);
                Maps::EntityFloats(E, "modelscale", &In.Scale, 1);
                Scene.DynamicInstances.push_back(In);
                const auto It = InZone.find(ExportNaming::Lower(Model));
                // a model this zone does not define is needed from the zone that does
                Models.emplace(Model, It == InZone.end() ? 0 : It->second);
                Counter(It == InZone.end() ? "map_entity_models_elsewhere" : "map_entity_models")++;
            }
        }
        if (Dbg)
            std::fprintf(Dbg, "[map] %s objects %zu static %zu dynamic %zu lights %zu sky '%s'\n", Scene.Name.c_str(),
                         Scene.Objects.size(), Scene.StaticInstances.size(), Scene.DynamicInstances.size(), Scene.Lights.size(),
                         Scene.Sky.Image.c_str());
        return true;
    }
}
