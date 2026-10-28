#include "stdafx.h"

#include "Games/Treyarch/TreyarchLevel.h"

#include "HuskyMath.h"
#include "MapLive.h"

#include <cstring>

bool TreyarchLevelZone::ReadLevel(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models, const Level32Layout& L,
                                  const std::function<XMaterial_t(uint64_t)>& Material)
{
    const int64_t World = Root("gfx_map");
    if (World < 0) return false;
    const auto& Img = Image();
    FILE* Dbg = Maps::DebugFile();
    Scene.Name = StringAt(PointerAt(World + L.BaseName));
    if (Scene.Name.empty()) Scene.Name = StringAt(PointerAt(World));
    if (Scene.Name.empty()) Scene.Name = Name();

    // ---- the world: its surfaces through the brush models
    const uint32_t VertexCount = At<uint32_t>(Img, World + L.VertexCount);
    const int64_t Vertices = Off(PointerAt(World + L.Vertices));
    const bool Packed = L.Format == VertexFormat::Packed;
    const uint64_t Bytes = Packed ? At<uint32_t>(Img, World + L.VertexBytes) : (uint64_t)VertexCount * 44;
    const uint32_t Stride = Packed ? (VertexCount ? (uint32_t)(Bytes / VertexCount) : 0) : 44;
    Maps::WorldLayout W;
    W.Image = &Img;
    W.PointerSize = 4;
    W.Surfaces = Off(PointerAt(World + L.Surfaces));
    W.SurfaceCount = At<uint32_t>(Img, World + L.SurfaceCount);
    W.Brushes = Off(PointerAt(World + L.Brushes));
    W.BrushCount = At<uint32_t>(Img, World + L.BrushCount);
    W.Indices = Off(PointerAt(World + L.Indices));
    W.IndexCount = At<uint32_t>(Img, World + L.IndexCount);
    W.SurfaceSize = L.SurfaceSize, W.FirstVertex = L.SurfaceFirstVertex, W.TriCount = L.SurfaceTris, W.BaseIndex = L.SurfaceBaseIndex;
    W.Material = L.SurfaceMaterial;
    W.VertexBytes = Packed ? Stride : 0;   // a packed surface's first vertex is a byte offset
    W.BrushSize = L.BrushSize, W.FirstSurface = L.BrushFirstSurface, W.SurfaceCountAt = L.BrushSurfaceCount;
    if (Dbg)
        std::fprintf(Dbg, "[map] %s vertices %u (%llu bytes, stride %u) at %llx indices %u surfaces %u brushes %u\n", Scene.Name.c_str(),
                     VertexCount, (unsigned long long)Bytes, Stride, (long long)Vertices, W.IndexCount, W.SurfaceCount, W.BrushCount);
    // a record by its index, a packed vertex by its byte offset in the block
    const auto Read = [Img, Vertices, VertexCount, Bytes, Stride, Packed](uint32_t Key, Maps::Vertex& Out) {
        if (Vertices < 0 || (Packed ? (uint64_t)Key + Stride > Bytes : Key >= VertexCount)) return false;
        const int64_t V = Vertices + (Packed ? (int64_t)Key : (int64_t)Key * Stride);
        if (V + Stride > (int64_t)Img.size()) return false;
        for (int K = 0; K < 3; K++) Out.Pos[K] = At<float>(Img, V + K * 4);
        for (int K = 0; K < 4; K++) Out.Color[K] = Img[(size_t)(V + 16 + K)];
        if (!Packed)
        {
            Out.Uv[0] = At<float>(Img, V + 20), Out.Uv[1] = At<float>(Img, V + 24);
            const float Scale = ((float)Img[(size_t)(V + 39)] + 192.0f) / 32385.0f;
            for (int K = 0; K < 3; K++) Out.Normal[K] = ((float)Img[(size_t)(V + 36 + K)] - 127.0f) * Scale;
            return true;
        }
        Out.Uv[0] = HuskyMath::HalfToFloat(At<uint16_t>(Img, V + 20));
        Out.Uv[1] = HuskyMath::HalfToFloat(At<uint16_t>(Img, V + 22));
        // each 10-bit field as the low bits of the float 3.0's pattern, sign-folded, less 3
        const uint32_t N = At<uint32_t>(Img, V + 28);
        for (int K = 0; K < 3; K++)
        {
            const uint32_t B = (N >> (10 * K)) & 0x3ff;
            const uint32_t U = B - 2 * (B & 0x200) + 0x40400000u;
            float F = 0;
            std::memcpy(&F, &U, 4);
            Out.Normal[K] = (F - 3.0f) * 8208.0312f;
        }
        return true;
    };
    const auto MaterialOf = [&](uint64_t Address) { return Off(Address) < 0 ? std::string() : Maps::AddMaterial(Scene, Material(Address)); };
    if (const size_t Bad = Maps::AddWorld(Scene, W, Read, MaterialOf)) Counter("map_triangles_unread") += Bad;
    AddStaticModels(Scene, Models, Off(PointerAt(World + L.SModels)), At<uint32_t>(Img, World + L.SModelCount), L.SModelSize,
                    L.SModelModel, L.SModelPlacement);

    // ---- the primary lights
    if (const int64_t Com = Root("com_map"); Com >= 0)
    {
        const int64_t Lights = Off(PointerAt(Com + L.Lights));
        const uint32_t N = At<uint32_t>(Img, Com + L.LightCount);
        for (uint32_t K = 0; Lights >= 0 && K < N; K++)
        {
            const int64_t P = Lights + (int64_t)K * L.LightSize;
            const uint8_t Type = Img[(size_t)P];
            if (Type == 0) continue;
            Maps::Light Li;
            Li.Kind = Type == 1 ? Maps::Light::Type::Sun : Type == 2 ? Maps::Light::Type::Spot : Maps::Light::Type::Point;
            for (int J = 0; J < 3; J++)
            {
                Li.Color[J] = At<float>(Img, P + 8 + J * 4);
                Li.Dir[J] = At<float>(Img, P + 0x14 + J * 4);
                Li.Origin[J] = At<float>(Img, P + 0x20 + J * 4);
            }
            Li.Radius = At<float>(Img, P + 0x2c);
            Li.CosHalfFovOuter = At<float>(Img, P + 0x30);
            Scene.Lights.push_back(Li);
        }
    }
    AddMapEnts(Scene);
    if (L.Clip.BrushSize) AddCollision(Scene, { "col_map_mp", "col_map_sp" }, L.Clip);

    // ---- the sky: a model the world names (skyBoxModel) when the zone holds it, else its image
    const std::string SkyModel = L.SkyModel >= 0 ? StringAt(PointerAt(World + L.SkyModel)) : std::string();
    if (!SkyModel.empty())
        for (const auto& I : Walk().Instances)
            if (I.Parent < 0 && OwnerIs(I, ModelType()) && ModelAt(Live().Origin() + I.Pos) == SkyModel)
            {
                Scene.Sky.Model = SkyModel;
                Models.emplace(SkyModel, Live().Origin() + I.Pos);
                break;
            }
    if (Scene.Sky.Model.empty() && L.SkyImage >= 0)
        if (const int64_t Sky = Off(PointerAt(World + L.SkyImage)); Sky >= 0)
        {
            const std::string SkyName = AssetName(DiskGame::Kind::Image, Sky);
            if (!SkyName.empty() && SkyName[0] != '$')
            {
                Scene.Sky.Image = SkyName;
                Scene.Images[SkyName];   // written by the image kind with the textures
            }
        }
    return true;
}
