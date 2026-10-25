#include "stdafx.h"

#include "MapGlbWriter.h"

#include "GltfSceneWriter.h"
#include "MapImportScript.h"
#include "MapMaterials.h"
#include "MapSceneWriter.h"
#include "MaterialDescription.h"
#include "PreviewTextures.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>
#include <map>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace Maps
{
namespace
{
    GltfSceneWriter::Vertex ToGltf(const Vertex& V)
    {
        GltfSceneWriter::Vertex O;
        for (int K = 0; K < 3; K++)
        {
            O.pos[K] = V.Pos[K];
            O.normal[K] = V.Normal[K];
        }
        O.uv[0] = V.Uv[0];
        O.uv[1] = V.Uv[1];
        for (int K = 0; K < 4; K++) O.color[K] = V.Color[K];
        return O;
    }

    // File as the .glb (in Dir) names it: relative, forward slashes
    // Text: newlines as the platform writes them (the import script); else the bytes as they are
    bool WriteFile(const fs::path& Path, const std::string& Text, bool AsText = false)
    {
        std::ofstream O(Path, AsText ? std::ios::trunc : std::ios::binary | std::ios::trunc);
        O.write(Text.data(), (std::streamsize)Text.size());
        return (bool)O;
    }

}

bool WriteGlb(const MapScene& Scene, const std::string& GlbPath, const Materials::IAdapter* Adapter,
              WriteStats& Stats, std::string& Error)
{
    const fs::path Dir = fs::path(GlbPath).parent_path();
    const std::string Stem = fs::path(GlbPath).stem().string();
    std::error_code Ec;
    fs::create_directories(Dir, Ec);

    GltfSceneWriter W;
    // the scene root converts CoD (Z up, inches) to glTF (Y up, metres): instancing is kept
    constexpr float S = 0.0254f;
    W.SetSceneRoot({ S, 0, 0, 0, 0, 0, -S, 0, 0, S, 0, 0, 0, 0, 0, 1 }, "cod_to_gltf");

    std::unordered_map<std::string, int> ImageIndex;
    const auto ImageOf = [&](const std::string& Name) -> int {
        if (Name.empty()) return -1;
        if (const auto It = ImageIndex.find(Name); It != ImageIndex.end()) return It->second;
        int Idx = -1;
        if (const auto It = Scene.Images.find(Name); It != Scene.Images.end())
        {
            if (!It->second.Png.empty()) Idx = W.AddImagePng(Name, It->second.Png);
            else if (!It->second.Gltf.empty()) Idx = W.AddImageUri(Name, RelativeTo(It->second.Gltf, Dir));
            else if (!It->second.File.empty()) Idx = W.AddImageUri(Name, RelativeTo(It->second.File, Dir));
        }
        if (Idx >= 0) Stats.Images++;
        ImageIndex.emplace(Name, Idx);
        return Idx;
    };

    // each material described once by the game's adapter: the sidecar, and the .glb's alpha mode
    const Materials::IAdapter& A = Adapter ? *Adapter : Materials::DefaultAdapter();
    std::map<std::string, std::pair<Materials::Source, Materials::Description>> Described;
    const auto DescriptionOf = [&](const std::string& Name) -> const Materials::Description& {
        if (const auto It = Described.find(Name); It != Described.end()) return It->second.second;
        return Described.emplace(Name, DescribeMaterial(Scene, Name, A, Dir)).first->second.second;
    };

    // The alpha mode: the description's; Auto (cutout when the colour map's alpha masks) judged on
    // the colour map now when its pixels are here, else after the run (ResolveAlpha).
    using GltfAlpha = GltfSceneWriter::Alpha;
    std::map<std::string, int> AlphaUsed;   // by colour map: 1 masks, 0 does not, -1 not written yet
    const auto AlphaOf = [&](const std::string& Name, const std::string& Color, float& Cutoff) {
        const Materials::Description& D = DescriptionOf(Name);
        Cutoff = D.AlphaCutoff;
        if (D.Alpha == Materials::AlphaMode::Cutout) return GltfAlpha::Mask;
        if (D.Alpha == Materials::AlphaMode::Blend) return GltfAlpha::Blend;
        const auto Img = Scene.Images.find(Color);
        if (D.Alpha != Materials::AlphaMode::Auto || Img == Scene.Images.end()) return GltfAlpha::Opaque;
        const std::string& File = !Img->second.File.empty() ? Img->second.File : Img->second.Gltf;
        auto Known = AlphaUsed.find(Color);
        if (Known == AlphaUsed.end())
        {
            bool Used = false;
            const bool Read = !Img->second.Png.empty() ? ColorMapAlphaUsed(Img->second.Png, ".png", Used)
                                                       : fs::exists(File, Ec) && ColorMapAlphaUsed(File, Used);
            Known = AlphaUsed.emplace(Color, Read ? (int)Used : -1).first;
        }
        if (Known->second < 0) Stats.AlphaLater.push_back({ Name, File, D.AlphaCutoff });
        return Known->second > 0 ? GltfAlpha::Mask : GltfAlpha::Opaque;
    };

    // one glTF material per scene material, on first use
    std::unordered_map<std::string, int> MaterialIndex;
    std::set<std::string> Missing;
    const auto MaterialOf = [&](const std::string& Name, bool& Textured) -> int {
        const auto Found = Scene.Materials.find(Name);
        const std::string Color = Found != Scene.Materials.end() ? Found->second.ColorMap : std::string();
        const int Img = ImageOf(Color);
        Textured = Img >= 0;
        if (const auto It = MaterialIndex.find(Name); It != MaterialIndex.end()) return It->second;
        if (Img < 0 && !Color.empty()) Missing.insert(Color);
        const float G = Img >= 0 ? 1.0f : 0.75f;   // white under a texture, else neutral grey
        float Cutoff = 0.5f;
        const GltfAlpha Alpha = Img >= 0 ? AlphaOf(Name, Color, Cutoff) : GltfAlpha::Opaque;
        float Rgb[3];   // times the material's colour tint (a white colour map's whole colour)
        Materials::BaseColorFactor(DescriptionOf(Name), Rgb);
        const int Idx = W.AddMaterial(Name.empty() ? "material" : Name, Img, G * Rgb[0], G * Rgb[1], G * Rgb[2], 1.0f, Alpha, Cutoff);
        MaterialIndex.emplace(Name, Idx);
        Stats.Materials++;
        Stats.AlphaMasked += Alpha == GltfAlpha::Mask;
        Stats.AlphaBlended += Alpha == GltfAlpha::Blend;
        return Idx;
    };

    // A surface as glTF data, its winding reversed (the game's front face is glTF's back).
    struct Part
    {
        std::vector<GltfSceneWriter::Vertex> V;
        std::vector<uint32_t> I;
        int Material = -1;
    };
    size_t Triangles = 0;
    bool Textured = false;
    const auto Convert = [&](const Surface& Sf) {
        Part P;
        P.V.reserve(Sf.Vertices.size());
        for (const auto& X : Sf.Vertices) P.V.push_back(ToGltf(X));
        P.I.reserve(Sf.Indices.size());
        Triangles = 0;
        for (size_t T = 0; T + 2 < Sf.Indices.size(); T += 3)
        {
            const uint32_t A = Sf.Indices[T], B = Sf.Indices[T + 1], C = Sf.Indices[T + 2];
            if (A >= P.V.size() || B >= P.V.size() || C >= P.V.size()) continue;
            P.I.push_back(A);
            P.I.push_back(C);
            P.I.push_back(B);
            Triangles++;
        }
        P.Material = MaterialOf(Sf.Material, Textured);
        return P;
    };
    const auto AddSurface = [&](const std::string& MeshName, const Surface& Sf) -> int {
        const Part P = Convert(Sf);
        const bool More = Sf.UvSets > 1 && Sf.MoreUvs.size() == Sf.Vertices.size() * (Sf.UvSets - 1) * 2;
        return W.AddMesh(MeshName, P.V, P.I, P.Material, More ? &Sf.MoreUvs : nullptr, More ? Sf.UvSets - 1 : 0);
    };

    for (const Mesh& M : Scene.Objects)
        for (const Surface& Sf : M.Surfaces)
        {
            const std::string Name = M.Surfaces.size() == 1 ? M.Name : M.Name + "/" + Sf.Material;
            const int Idx = AddSurface(Name, Sf);
            if (Idx < 0) continue;
            W.AddNode(Name, Idx, GltfSceneWriter::Identity());
            Stats.WorldMeshes++;
            Stats.WorldTriangles += Triangles;
            if (Textured) Stats.WorldSurfacesTextured++;
        }

    // Models, each converted once as one glTF mesh (a primitive per surface), then placed by
    // one node per placement: the import script turns a mesh several nodes share into one Blender
    // collection, instanced
    std::unordered_map<std::string, int> ModelMesh;
    const auto MeshOf = [&](const std::string& Model) -> int {
        if (const auto It = ModelMesh.find(Model); It != ModelMesh.end()) return It->second;
        int Idx = -1;
        if (const auto It = Scene.Models.find(Model); It != Scene.Models.end())
        {
            std::vector<Part> Parts;
            for (const Surface& Sf : It->second.Surfaces) Parts.push_back(Convert(Sf));
            std::vector<GltfSceneWriter::Part> Refs;
            for (const Part& P : Parts) Refs.push_back({ &P.V, &P.I, P.Material });
            Idx = W.AddMesh(It->second.Name, Refs);
            if (Idx >= 0)
            {
                Stats.Models++;
                for (const Part& P : Parts) Stats.ModelMeshes += !P.V.empty() && !P.I.empty();
            }
        }
        return ModelMesh.emplace(Model, Idx).first->second;
    };
    for (const auto* Placed : { &Scene.StaticInstances, &Scene.DynamicInstances, &Scene.SplinedInstances })
        for (const Instance& In : *Placed)
        {
            const auto Matrix = GltfSceneWriter::MatrixFromAxis(In.Axis, In.Origin, In.Scale);
            const int Mesh = MeshOf(In.Model);
            if (Mesh < 0)
            {
                // an empty at the spot, named after the model: what is missing and where
                W.AddNode(In.Model.empty() ? In.Name : In.Model, -1, Matrix);
                Stats.InstancesWithoutModel++;
                continue;
            }
            W.AddNode(In.Name, Mesh, Matrix);
            Stats.InstanceNodes++;
            Stats.Instances++;
        }

    // Sky: a model at the origin, or a texture for the import script
    if (!Scene.Sky.Model.empty())
        if (const int Mesh = MeshOf(Scene.Sky.Model); Mesh >= 0)
            W.AddNode("sky_" + std::to_string(Stats.SkyMeshes++), Mesh, GltfSceneWriter::Identity());
    // the import script, and the sky texture: its PNG bytes written beside the .glb, or the file
    // it names
    std::string SkyPath;
    if (const auto It = Scene.Images.find(Scene.Sky.Image); It != Scene.Images.end())
    {
        const Image& I = It->second;
        if (!I.Png.empty() && WriteFile(Dir / (Stem + "_sky.png"), std::string(I.Png.begin(), I.Png.end())))
            SkyPath = Stem + "_sky.png";
        else if (!I.File.empty())   // Blender reads DDS too
            SkyPath = RelativeTo(I.File, Dir);
    }
    bool AnyFile = false;   // a sidecar names files: none when every image is embedded
    for (const auto& [Name, I] : Scene.Images) AnyFile = AnyFile || !I.File.empty();
    const bool Sidecar = !MaterialIndex.empty() && AnyFile;
    WriteFile(Dir / (Stem + "_import.py"),
              ImportScript({ Stem + ".glb", Sidecar ? Stem + ".materials.json" : std::string(), SkyPath,
                             Scene.Sky.Sun[0] || Scene.Sky.Sun[1] || Scene.Sky.Sun[2] ? Scene.Sky.Sun : nullptr,
                             Scene.Sky.Atmosphere }), true);

    // Lights: the colour's brightness as the intensity, its hue as the colour (CoD's light
    // model is not physical: rough values, tuned in Blender)
    for (size_t I = 0; I < Scene.Lights.size(); I++)
    {
        const Light& L = Scene.Lights[I];
        float Br = std::max(L.Color[0], std::max(L.Color[1], L.Color[2]));
        const float Inv = Br > 1e-4f ? 1.0f / Br : 1.0f;
        const float R = L.Color[0] * Inv, G = L.Color[1] * Inv, B = L.Color[2] * Inv;
        const std::string Name = "light_" + std::to_string(I);
        if (L.Kind == Light::Type::Sun)
            W.AddLightNode(Name, GltfSceneWriter::LightType::Directional, R, G, B, Br * 4.0f, 0.0f,
                           GltfSceneWriter::MatrixLookDir(L.Origin, L.Dir));
        else if (L.Kind == Light::Type::Spot)
        {
            const float C = std::min(1.0f, std::max(-1.0f, L.CosHalfFovOuter));
            W.AddLightNode(Name, GltfSceneWriter::LightType::Spot, R, G, B, Br * 2000.0f, L.Radius,
                           GltfSceneWriter::MatrixLookDir(L.Origin, L.Dir), std::acos(C));
        }
        else
        {
            const float Ident[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
            W.AddLightNode(Name, GltfSceneWriter::LightType::Point, R, G, B, Br * 2000.0f, L.Radius,
                           GltfSceneWriter::MatrixFromAxis(Ident, L.Origin, 1.0f));
        }
        Stats.Lights++;
    }
    Stats.MissingTextures = Missing.size();

    if (!W.WriteGlb(GlbPath))
    {
        Error = "cannot write " + GlbPath;
        return false;
    }

    // The materials, described by the game's adapter (a sidecar names files: none when every
    // image is embedded)
    if (Sidecar)
    {
        Materials::Sidecar Side;
        Side.Game = Scene.Game;
        Side.Adapter = A.Name();
        for (const auto& [Name, Unused] : MaterialIndex)
        {
            DescriptionOf(Name);
            auto& [Src, D] = Described.at(Name);
            Side.Materials.push_back(D);
            Side.Sources.push_back(Src);
        }
        Materials::WriteSidecar((Dir / (Stem + ".materials.json")).string(), Side);
    }
    if (!Scene.MapEnts.empty()) WriteFile(Dir / (Stem + ".map_ents"), Scene.MapEnts);
    return true;
}

size_t ResolveAlpha(const std::string& GlbPath, const std::vector<WriteStats::LaterAlpha>& Later)
{
    // each colour map once, on a few threads
    std::map<std::string, bool> Files;
    for (const auto& L : Later) Files[L.ColorMap] = false;
    std::vector<std::pair<const std::string, bool>*> Work;
    for (auto& F : Files) Work.push_back(&F);
    std::atomic<size_t> Next{ 0 };
    std::vector<std::thread> Pool;
    for (unsigned K = 0; K < std::max(1u, std::min(8u, std::thread::hardware_concurrency())); K++)
        Pool.emplace_back([&]() {
            for (size_t I; (I = Next++) < Work.size();)
                if (!ColorMapAlphaUsed(Work[I]->first, Work[I]->second)) Work[I]->second = false;
        });
    for (auto& P : Pool) P.join();
    std::map<std::string, float> Masked;
    for (const auto& L : Later)
        if (Files[L.ColorMap]) Masked[L.Material] = L.Cutoff;
    if (Masked.empty()) return 0;
    // a .glb: 12-byte header, the JSON chunk (length, type, text), the binary chunk as it is
    std::ifstream In(GlbPath, std::ios::binary | std::ios::ate);
    std::string Bytes(In ? (size_t)In.tellg() : 0, '\0');
    In.seekg(0);
    In.read(Bytes.data(), (std::streamsize)Bytes.size());
    In.close();
    if (Bytes.size() < 20) return 0;
    uint32_t JsonLen = 0;
    std::memcpy(&JsonLen, Bytes.data() + 12, 4);
    if (20 + (size_t)JsonLen > Bytes.size()) return 0;
    nlohmann::json Doc = nlohmann::json::parse(Bytes.begin() + 20, Bytes.begin() + 20 + JsonLen, nullptr, false);
    if (Doc.is_discarded() || !Doc.contains("materials")) return 0;
    size_t Changed = 0;
    for (auto& M : Doc["materials"])
        if (const auto It = Masked.find(M.value("name", std::string())); It != Masked.end())
        {
            M["alphaMode"] = "MASK";
            M["alphaCutoff"] = It->second;
            Changed++;
        }
    std::string Text = Doc.dump();
    while (Text.size() % 4) Text += ' ';
    const std::string Rest = Bytes.substr(20 + JsonLen);
    uint32_t Header[5] = { 0x46546C67, 2, (uint32_t)(20 + Text.size() + Rest.size()), (uint32_t)Text.size(), 0x4E4F534A };
    std::ofstream Out(GlbPath, std::ios::binary | std::ios::trunc);
    Out.write(reinterpret_cast<const char*>(Header), sizeof(Header));
    Out.write(Text.data(), (std::streamsize)Text.size());
    Out.write(Rest.data(), (std::streamsize)Rest.size());
    return Out ? Changed : 0;
}

std::string Describe(const WriteStats& S)
{
    std::ostringstream Os;
    Os << "world_meshes=" << S.WorldMeshes << "\nworld_triangles=" << S.WorldTriangles
       << "\nworld_surfaces_textured=" << S.WorldSurfacesTextured << "\nmodels=" << S.Models
       << "\nmodel_meshes=" << S.ModelMeshes << "\ninstances=" << S.Instances << "\ninstance_nodes=" << S.InstanceNodes
       << "\ninstances_without_model=" << S.InstancesWithoutModel << "\nsky_meshes=" << S.SkyMeshes
       << "\nlights=" << S.Lights << "\nmaterials=" << S.Materials << "\nimages=" << S.Images
       << "\nmissing_textures=" << S.MissingTextures << "\nalpha_masked=" << S.AlphaMasked
       << "\nalpha_blended=" << S.AlphaBlended << "\n";
    return Os.str();
}

namespace
{
    class GlbSceneWriter final : public ISceneWriter
    {
    public:
        const char* Extension() const override { return ".glb"; }
        bool Write(const MapScene& Scene, const std::string& Path, const Materials::IAdapter& Adapter,
                   SceneWriteResult& Result, std::string& Error) const override
        {
            WriteStats Stats;
            if (!WriteGlb(Scene, Path, &Adapter, Stats, Error)) return false;
            Result.Counters = { { "map_alpha_masked", Stats.AlphaMasked }, { "map_world_meshes", Stats.WorldMeshes },
                                { "map_world_triangles", Stats.WorldTriangles }, { "map_models", Stats.Models },
                                { "map_instances", Stats.Instances },
                                { "map_instances_without_model", Stats.InstancesWithoutModel },
                                { "map_lights", Stats.Lights }, { "map_materials", Stats.Materials },
                                { "map_images", Stats.Images }, { "map_missing_textures", Stats.MissingTextures } };
            // Auto alpha on colour maps a dependency writes: judged once they are
            if (!Stats.AlphaLater.empty())
            {
                Result.Later = [Path, Later = Stats.AlphaLater]() { return ResolveAlpha(Path, Later); };
                Result.LaterCounter = "map_alpha_masked_later";
            }
            return true;
        }
    };
}

const ISceneWriter& GlbWriter()
{
    static const GlbSceneWriter Writer;
    return Writer;
}
}
