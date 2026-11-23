#include "stdafx.h"

// The 32-bit IW level (CoD4, World at War, Call of Duty Online), read from the zone the way C2M's
// ModernWarfare.cs and WorldatWar.cs read it, every offset one the title's
// zonekit tables read (MapLayout, given by the title): the GfxWorld's brush models over its
// surfaces, one index and one vertex buffer; the static models of its GfxWorldDpvsStatic; the
// clipMap_t's dynamic entities; the ComWorld's primary lights; the models the entity string
// places; the sky.

#include "Games/IW/Iw32Level.h"

#include "MapLive.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

namespace
{
    // GfxWorldVertex (44): position, binormal sign, colour (16), uv (20), lightmap uv, then the
    // normal (36) as a PackedUnitVec: three bytes around 127 and a scale byte (the live model
    // translator's unpacking)
    constexpr int64_t VertexSize = 44, VertexColor = 16, VertexUv = 20, VertexNormal = 36;
    // srfTriangles_t at the start of a GfxSurface: first vertex (u32 at 4), triangle count (u16 at
    // 10), first index (u32 at 12)
    constexpr int64_t SurfaceFirstVertex = 4, SurfaceTris = 10, SurfaceBaseIndex = 12;
    // DynEntityDef: a GfxPlacement (quaternion at +4, origin at +0x14), the model at +0x20, a brush
    // model's index (u16) at +0x24
    constexpr int64_t DynQuat = 4, DynOrigin = 0x14, DynModel = 0x20, DynBrush = 0x24;
    constexpr int64_t WorldBaseName = 4;

    void UnpackNormal(uint32_t Packed, float Out[3])
    {
        uint8_t B[4];
        std::memcpy(B, &Packed, 4);
        const float Scale = ((float)B[3] + 192.0f) / 32385.0f;
        for (int K = 0; K < 3; K++) Out[K] = ((float)B[K] - 127.0f) * Scale;
    }
}

bool Iw32LevelZone::ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models)
{
    const int64_t World = Root("gfx_map");
    if (World < 0) return false;
    const MapLayout M = Map();
    const auto& Img = Image();
    FILE* Dbg = Maps::DebugFile();
    Scene.Name = StringAt(PointerAt(World + WorldBaseName));
    if (Scene.Name.empty()) Scene.Name = Name();

    // ---- world: the brush models' surfaces
    const uint32_t VertexCount = At<uint32_t>(Img, World + M.VertexCount), IndexCount = At<uint32_t>(Img, World + M.IndexCount);
    const int64_t Vertices = Off(PointerAt(World + M.Vertices)), Indices = Off(PointerAt(World + M.Indices));
    const uint32_t SurfaceCount = At<uint32_t>(Img, World + M.SurfaceCount);
    const int64_t Surfaces = Off(PointerAt(World + M.Surfaces));
    const int32_t BrushCount = At<int32_t>(Img, World + M.BrushCount);
    const int64_t Brushes = Off(PointerAt(World + M.Brushes));
    if (Dbg)
        std::fprintf(Dbg, "[map] %s vertices %u at %llx indices %u at %llx surfaces %u at %llx brushes %d at %llx\n", Scene.Name.c_str(),
                     VertexCount, (long long)Vertices, IndexCount, (long long)Indices, SurfaceCount, (long long)Surfaces, BrushCount, (long long)Brushes);
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
    Maps::WorldLayout L;
    L.Image = &Img;
    L.Surfaces = Surfaces, L.SurfaceCount = SurfaceCount;
    L.Brushes = Brushes, L.BrushCount = (uint32_t)std::max(BrushCount, 0);
    L.Indices = Indices, L.IndexCount = IndexCount;
    L.SurfaceSize = M.SurfaceSize, L.FirstVertex = SurfaceFirstVertex, L.TriCount = SurfaceTris, L.BaseIndex = SurfaceBaseIndex;
    L.Material = M.SurfaceMaterial;
    L.BrushSize = M.BrushSize, L.FirstSurface = M.BrushFirstSurface, L.SurfaceCountAt = M.BrushSurfaceCount;
    L.PointerSize = 4, L.BrushFieldBytes = M.BrushField;
    if (M.LayerData >= 0 && M.LayerDataSize >= 0)
        L.MoreUvs = Maps::NamedLayerUvs(Img, Off(At<uint32_t>(Img, World + M.LayerData)), At<uint32_t>(Img, World + M.LayerDataSize), M.SurfaceMaterial, 4,
                                        [&](uint64_t A) { return Off(A) < 0 ? std::string() : StringAt(At<uint32_t>(Img, Off(A))); });
    // a shadow caster's surfaces (drawn into shadow maps only) and a sky's (the sky image, drawn
    // by view direction) are left out of the world: the game does not draw them as textured geometry
    std::set<std::string> Hidden;
    const auto MaterialOf = [&](uint64_t Address) {
        if (Off(Address) < 0) return std::string();
        const XMaterial_t X = Material(Address);
        const std::string N = Maps::AddMaterial(Scene, X);
        if (!MaterialDrawn(X) || X.TechsetName == "wc_sky" || X.TechsetName == "mc_sky") Hidden.insert(N);
        return N;
    };
    if (const size_t Bad = Maps::AddWorld(Scene, L, Fetch, MaterialOf)) Counter("map_triangles_unread") += Bad;
    for (auto& Object : Scene.Objects)
        for (size_t K = Object.Surfaces.size(); K-- > 0;)
            if (Hidden.count(Object.Surfaces[K].Material))
            {
                const bool Sky = Scene.Materials[Object.Surfaces[K].Material].Techset.find("sky") != std::string::npos;
                Counter(Sky ? "map_sky_triangles_skipped" : "map_shadow_triangles_skipped") += Object.Surfaces[K].Triangles();
                Object.Surfaces.erase(Object.Surfaces.begin() + (std::ptrdiff_t)K);
            }
    Scene.Objects.erase(std::remove_if(Scene.Objects.begin(), Scene.Objects.end(), [](const Maps::Mesh& O) { return O.Surfaces.empty(); }),
                        Scene.Objects.end());
    if (!Hidden.empty())
    {
        // and their images, unless a material drawn uses them too
        for (const auto& N : Hidden) Scene.Materials.erase(N);
        std::set<std::string> Used;
        for (const auto& [Name, Mat] : Scene.Materials)
            for (const auto& Tx : Mat.Textures) Used.insert(Tx.Image);
        for (auto It = Scene.Images.begin(); It != Scene.Images.end();)
            It = Used.count(It->first) ? std::next(It) : Scene.Images.erase(It);
    }

    const int64_t SModels = Off(PointerAt(World + M.SModels));
    AddStaticModels(Scene, Models, SModels < 0 ? -1 : SModels + M.SModelPlacement, At<uint32_t>(Img, World + M.SModelCount),
                    M.SModelSize, M.SModelModel - M.SModelPlacement);

    std::vector<std::pair<uint16_t, Maps::Instance>> DynBrushes;
    int64_t Clip = Root("col_map_mp");
    if (Clip < 0) Clip = Root("col_map_sp");
    for (int List = 0; Clip >= 0 && List < 2; List++)
    {
        const uint16_t Count = At<uint16_t>(Img, Clip + M.DynCounts + List * 2);
        const int64_t Dyn = Off(PointerAt(Clip + M.DynLists + List * 4));
        for (uint16_t K = 0; Dyn >= 0 && K < Count; K++)
        {
            const int64_t E = Dyn + (int64_t)K * M.DynSize;
            Maps::Instance In;
            In.Name = "dynent_" + std::to_string(List) + "_" + std::to_string(K);
            float Q[4];
            for (int J = 0; J < 4; J++) Q[J] = At<float>(Img, E + DynQuat + J * 4);
            Maps::AxisFromQuat(Q, In.Axis);
            for (int J = 0; J < 3; J++) In.Origin[J] = At<float>(Img, E + DynOrigin + J * 4);
            const uint64_t Model = PointerAt(E + DynModel);
            In.Model = ModelAt(Model);
            if (In.Model.empty())
            {
                // a dynamic brush (a breakable window, a fence): its brush model, placed by the pose
                if (const uint16_t Brush = At<uint16_t>(Img, E + DynBrush)) DynBrushes.push_back({ Brush, In });
                continue;
            }
            Scene.DynamicInstances.push_back(In);
            Models.emplace(In.Model, Model);
        }
    }

    // ---- lights: type 1 sun, 2 spot, 3 omni
    if (const int64_t Com = Root("com_map"); Com >= 0)
    {
        const uint32_t Count = At<uint32_t>(Img, Com + 8);
        const int64_t Lights = Off(PointerAt(Com + 0xc));
        for (uint32_t K = 0; Lights >= 0 && K < Count; K++)
        {
            const int64_t Li = Lights + (int64_t)K * M.LightSize, C = Li + M.LightColor;
            const uint8_t Type = At<uint8_t>(Img, Li);
            if (Type < 1 || Type > 3) continue;
            Maps::Light Out;
            Out.Kind = Type == 1 ? Maps::Light::Type::Sun : Type == 2 ? Maps::Light::Type::Spot : Maps::Light::Type::Point;
            for (int J = 0; J < 3; J++)
            {
                Out.Color[J] = At<float>(Img, C + J * 4);
                Out.Dir[J] = At<float>(Img, C + 12 + J * 4);
                Out.Origin[J] = At<float>(Img, C + 24 + J * 4);
            }
            Out.Radius = At<float>(Img, C + 36);
            Out.CosHalfFovOuter = At<float>(Img, C + 40);
            Scene.Lights.push_back(Out);
            Counter(Type == 1 ? "map_lights_sun" : Type == 2 ? "map_lights_spot" : "map_lights_omni")++;
        }
    }

    // ---- the models the entity string places (script models, vehicles; "*n" is a brush model,
    // already in the world), found among the zone's models by name
    AddMapEnts(Scene);
    if (const Maps::ClipLayout C = ClipBrushes(); C.BrushSize) AddCollision(Scene, { "col_map_mp", "col_map_sp" }, C);
    if (const int64_t At = TriggersAt(); At >= 0) AddTriggers(Scene, At);
    std::map<std::string, uint64_t> ZoneModels;
    for (const auto& I : Walk().Instances)
        if (OwnerIs(I, ModelType()))
        {
            const std::string N = ModelAt(Live().Origin() + (uint64_t)I.Pos);
            if (!N.empty()) ZoneModels.emplace(ExportNaming::Lower(N), Live().Origin() + (uint64_t)I.Pos);
        }
    std::vector<Maps::Entity> Ents;
    if (!Scene.MapEnts.empty() && !Maps::ParseEntities(Scene.MapEnts, Ents)) Counter("map_ents_unparsed")++;
    // the brush models (doors, buyable boards, destructibles) are centred on their entity
    // (or on their dynamic entity's pose)
    std::vector<uint32_t> Unplaced = Maps::PlaceBrushModels(Scene, Ents);
    for (const auto& [Brush, Pose] : DynBrushes)
        if (const auto It = std::find(Unplaced.begin(), Unplaced.end(), Brush); It != Unplaced.end())
        {
            Maps::PlaceBrushModel(Scene, Brush, Pose.Origin, Pose.Axis);
            Unplaced.erase(It);
            Counter("map_brushmodels_dynamic")++;
        }
    if (!Unplaced.empty()) Counter("map_brushmodels_unplaced") += Unplaced.size();
    for (size_t K = 0; K < Ents.size(); K++)
    {
        const auto& E = Ents[K];
        const auto It = E.find("model");
        if (It == E.end() || It->second.empty() || It->second[0] == '*') continue;
        // placed things (script_model, script_vehicle, misc_*), not spawners (actor_*: a body
        // where an AI will appear)
        const auto Class = E.find("classname");
        const std::string Cls = Class == E.end() ? std::string() : Class->second;
        if (Cls.compare(0, 7, "script_") != 0 && Cls.compare(0, 5, "misc_") != 0)
        {
            Counter("map_ent_models_spawners")++;
            continue;
        }
        const auto Found = ZoneModels.find(ExportNaming::Lower(It->second));
        if (Found == ZoneModels.end())
        {
            Counter("map_ent_models_elsewhere")++;   // a model another zone holds (fx markers and the like)
            continue;
        }
        Maps::Instance In;
        In.Name = Cls + "_" + std::to_string(K);
        In.Model = ModelAt(Found->second);
        Maps::EntityFloats(E, "origin", In.Origin, 3);
        float Angles[3] = { 0, 0, 0 };
        Maps::EntityFloats(E, "angles", Angles, 3);
        Maps::AnglesToAxis(Angles, In.Axis);
        Maps::EntityFloats(E, "modelscale", &In.Scale, 1);
        Scene.DynamicInstances.push_back(In);
        Models.emplace(In.Model, Found->second);
        Counter("map_ent_models")++;
    }

    // ---- the sky: its image (a cube map, in GfxWorld or its first sky), and World at War's sky
    // model by name
    int64_t SkyImage = M.SkyImage >= 0 ? Off(PointerAt(World + M.SkyImage)) : -1;
    if (M.Skies >= 0 && SkyImage < 0)
        if (const int64_t Sky = Off(PointerAt(World + M.Skies)); Sky >= 0) SkyImage = Off(PointerAt(Sky + M.SkyImageInSky));
    if (SkyImage >= 0)
    {
        Scene.Sky.Image = AssetName(DiskGame::Kind::Image, SkyImage);
        if (!Scene.Sky.Image.empty()) Scene.Images[Scene.Sky.Image];
    }
    if (M.SkyModel >= 0)
    {
        const std::string Sky = StringAt(PointerAt(World + M.SkyModel));
        if (const auto Found = ZoneModels.find(ExportNaming::Lower(Sky)); !Sky.empty() && Found != ZoneModels.end())
        {
            Scene.Sky.Model = ModelAt(Found->second);
            Models.emplace(Scene.Sky.Model, Found->second);
        }
        else if (!Sky.empty())
            Counter("map_sky_model_elsewhere")++;
    }
    if (Dbg)
    {
        // the layout's own checks: brush models cover disjoint surface ranges inside the array,
        // placements carry orthonormal axes, dynamic entities unit quaternions
        uint64_t Covered = 0, Overlap = 0, Bad = 0;
        std::vector<uint8_t> Seen(SurfaceCount, 0);
        for (int32_t B = 0; Brushes >= 0 && B < BrushCount; B++)
        {
            const int64_t Br = Brushes + (int64_t)B * M.BrushSize;
            const auto F = [&](int64_t P) { return M.BrushField == 2 ? (uint32_t)At<uint16_t>(Img, P) : At<uint32_t>(Img, P); };
            const uint32_t First = F(Br + M.BrushFirstSurface), Count = F(Br + M.BrushSurfaceCount);
            if (Count == 0) continue;
            if ((uint64_t)First + Count > SurfaceCount) { Bad++; continue; }
            for (uint32_t S = First; S < First + Count; S++) (Seen[S]++ ? Overlap : Covered)++;
        }
        size_t Skewed = 0;
        for (const auto& In : Scene.StaticInstances)
        {
            for (int R = 0; R < 3; R++)
            {
                const float* A = In.Axis + R * 3;
                if (std::fabs(A[0] * A[0] + A[1] * A[1] + A[2] * A[2] - 1.0f) > 0.01f) { Skewed++; break; }
            }
        }
        std::fprintf(Dbg, "[map] %s surfaces covered %llu/%u overlap %llu bad ranges %llu; static axes not unit %zu/%zu\n", Scene.Name.c_str(),
                     (unsigned long long)Covered, SurfaceCount, (unsigned long long)Overlap, (unsigned long long)Bad, Skewed, Scene.StaticInstances.size());
        std::fprintf(Dbg, "[map] %s objects %zu smodels %zu dyn %zu lights %zu models %zu sky %s / %s\n", Scene.Name.c_str(), Scene.Objects.size(),
                     Scene.StaticInstances.size(), Scene.DynamicInstances.size(), Scene.Lights.size(), Models.size(),
                     Scene.Sky.Image.c_str(), Scene.Sky.Model.c_str());
    }
    return true;
}
