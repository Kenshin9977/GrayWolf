#include "stdafx.h"

#include "MapCastWriter.h"

#include "CastExport.h"
#include "CastMaterials.h"
#include "ExportNaming.h"
#include "Hashing.h"
#include "MapMaterials.h"
#include "MapSceneWriter.h"
#include "SettingsManager.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>

namespace fs = std::filesystem;

namespace Maps
{
namespace
{
    // the model files' unit: centimetres from the game's inches, unless the unit setting is Inch
    float UnitScale()
    {
        return SettingsManager::GetSetting("unit", "Centimeter") == "Inch" ? 1.0f : 2.54f;
    }

    // Materials described once per directory (their image paths are relative to the file's own)
    class MaterialBook
    {
    public:
        MaterialBook(const MapScene& Scene, const Materials::IAdapter& Adapter) : Scene(Scene), Adapter(Adapter) {}
        CastMaterialSlots Slots(const std::string& Name, const fs::path& Dir)
        {
            const auto Key = std::make_pair(Name, Dir.generic_string());
            auto It = Book.find(Key);
            if (It == Book.end())
                It = Book.emplace(Key, Materials::CastSlots(DescribeMaterial(Scene, Name, Adapter, Dir).second)).first;
            return It->second;
        }

    private:
        const MapScene& Scene;
        const Materials::IAdapter& Adapter;
        std::map<std::pair<std::string, std::string>, CastMaterialSlots> Book;
    };

    // a Model node: one Cast mesh per surface, the materials its surfaces use
    size_t AddModel(CastNode& Root, const std::string& Name, const Mesh& M, float Scale, MaterialBook& Book,
                    const fs::path& Dir)
    {
        auto Model = Root.AddNode(CastNodeId::Model, Hashing::HashXXHashString("model|" + Name));
        Model->SetProperty("n", Name);
        std::map<std::string, uint64_t> MaterialHash;
        for (const Surface& S : M.Surfaces)
            if (!S.Material.empty() && !MaterialHash.count(S.Material))
                MaterialHash[S.Material] = Cast::AddMaterial(*Model, Book.Slots(S.Material, Dir))->Hash;

        size_t Meshes = 0;
        for (const Surface& S : M.Surfaces)
        {
            if (S.Vertices.empty() || S.Indices.size() < 3) continue;
            // the surface's UV sets: the vertex's, then its other layers' (u1, u2...)
            const uint32_t More = S.UvSets > 1 && S.MoreUvs.size() == S.Vertices.size() * (S.UvSets - 1) * 2 ? S.UvSets - 1 : 0;
            CastMeshBuilder Builder(*Model, Hashing::HashXXHashString(Name + "|mesh" + std::to_string(Meshes)),
                                    S.Vertices.size(), 1 + More);
            std::vector<Vector2> Uvs(1 + More);
            for (size_t K = 0; K < S.Vertices.size(); K++)
            {
                const Vertex& V = S.Vertices[K];
                uint32_t Packed = 0;
                std::memcpy(&Packed, V.Color, 4);
                Uvs[0] = Vector2(V.Uv[0], V.Uv[1]);
                for (uint32_t J = 0; J < More; J++)
                    Uvs[1 + J] = Vector2(S.MoreUvs[(K * More + J) * 2], S.MoreUvs[(K * More + J) * 2 + 1]);
                Builder.Vertex(Vector3(V.Pos[0] * Scale, V.Pos[1] * Scale, V.Pos[2] * Scale),
                               Vector3(V.Normal[0], V.Normal[1], V.Normal[2]), Packed, Uvs.data());
            }
            for (size_t K = 0; K + 2 < S.Indices.size(); K += 3)
                Builder.Face(S.Indices[K], S.Indices[K + 1], S.Indices[K + 2]);
            if (const auto H = MaterialHash.find(S.Material); H != MaterialHash.end())
                Builder.Material(H->second);
            Meshes++;
        }
        return Meshes;
    }
}

void QuatFromAxis(const float A[9], float Q[4])
{
    // R's columns are the local axes, the rows of A: R[r][c] = A[c * 3 + r]
    const float R00 = A[0], R10 = A[1], R20 = A[2], R01 = A[3], R11 = A[4], R21 = A[5], R02 = A[6], R12 = A[7], R22 = A[8];
    const float Trace = R00 + R11 + R22;
    float X, Y, Z, W;
    if (Trace > 0)
    {
        const float S = std::sqrt(Trace + 1.0f) * 2;
        W = 0.25f * S; X = (R21 - R12) / S; Y = (R02 - R20) / S; Z = (R10 - R01) / S;
    }
    else if (R00 > R11 && R00 > R22)
    {
        const float S = std::sqrt(1.0f + R00 - R11 - R22) * 2;
        W = (R21 - R12) / S; X = 0.25f * S; Y = (R01 + R10) / S; Z = (R02 + R20) / S;
    }
    else if (R11 > R22)
    {
        const float S = std::sqrt(1.0f + R11 - R00 - R22) * 2;
        W = (R02 - R20) / S; X = (R01 + R10) / S; Y = 0.25f * S; Z = (R12 + R21) / S;
    }
    else
    {
        const float S = std::sqrt(1.0f + R22 - R00 - R11) * 2;
        W = (R10 - R01) / S; X = (R02 + R20) / S; Y = (R12 + R21) / S; Z = 0.25f * S;
    }
    const float L = std::sqrt(X * X + Y * Y + Z * Z + W * W);
    Q[0] = X / L; Q[1] = Y / L; Q[2] = Z / L; Q[3] = W / L;
}

bool WriteCastScene(const MapScene& Scene, const std::string& CastPath, const Materials::IAdapter* Adapter,
                    CastStats& Stats, std::string& Error)
{
    const fs::path Path(CastPath), Dir = Path.parent_path(), ModelDir = Dir / "models";
    const float Scale = UnitScale();
    MaterialBook Book(Scene, Adapter ? *Adapter : Materials::DefaultAdapter());
    std::error_code Ec;
    fs::create_directories(ModelDir, Ec);

    // each placed model once, in its own space: models/<model>.cast
    std::map<std::string, std::string> ModelFile;   // model -> path relative to the scene file
    std::set<std::string> Placed;
    for (const auto* List : { &Scene.StaticInstances, &Scene.DynamicInstances, &Scene.SplinedInstances })
        for (const Instance& I : *List) Placed.insert(I.Model);
    if (!Scene.Sky.Model.empty()) Placed.insert(Scene.Sky.Model);
    for (const std::string& Name : Placed)
    {
        const auto It = Scene.Models.find(Name);
        if (It == Scene.Models.end()) continue;
        const std::string File = ExportNaming::SafeFileName(Name) + ".cast";
        CastNode Root;
        Cast::AddMetadata(Root);
        AddModel(Root, Name, It->second, Scale, Book, ModelDir);
        if (!Cast::WriteFile(Root, (ModelDir / File).string()))
        {
            Error = "cannot write " + (ModelDir / File).string();
            return false;
        }
        ModelFile[Name] = "models/" + File;
        Stats.Models++;
    }

    // the scene: the world, then a placement per instance (their files resolved from the scene's
    // own directory: without a scene root, Cast's Blender importer refuses the instances)
    CastNode Root;
    Cast::AddMetadata(Root, ".");
    for (const Mesh& World : Scene.Objects)
        Stats.WorldMeshes += AddModel(Root, World.Name.empty() ? Scene.Name + "_world" : World.Name, World, Scale, Book, Dir);
    const auto Place = [&](const Instance& I, const std::string& Name) {
        const auto File = ModelFile.find(I.Model);
        if (File == ModelFile.end())
        {
            Stats.InstancesWithoutModel++;
            return;
        }
        const uint64_t Hash = Hashing::HashXXHashString("instance|" + Name);
        auto Node = Root.AddNode(CastNodeId::Instance, Hash);
        auto Ref = Node->AddNode(CastNodeId::File, Hashing::HashXXHashString("file|" + Name));
        Ref->SetProperty("p", File->second);
        Node->SetProperty("n", Name);
        Node->SetProperty("rf", CastPropertyId::Integer64, Ref->Hash);
        float Q[4];
        QuatFromAxis(I.Axis, Q);
        Node->SetProperty("p", CastPropertyId::Vector3, Vector3(I.Origin[0] * Scale, I.Origin[1] * Scale, I.Origin[2] * Scale));
        Node->SetProperty("r", CastPropertyId::Vector4, Quaternion(Q[0], Q[1], Q[2], Q[3]));
        Node->SetProperty("s", CastPropertyId::Vector3, Vector3(I.Scale, I.Scale, I.Scale));
        Stats.Instances++;
    };
    size_t Serial = 0;
    for (const auto* List : { &Scene.StaticInstances, &Scene.DynamicInstances, &Scene.SplinedInstances })
        for (const Instance& I : *List)
            Place(I, (I.Name.empty() ? I.Model : I.Name) + "_" + std::to_string(Serial++));
    if (!Scene.Sky.Model.empty())
    {
        Instance Sky;
        Sky.Model = Scene.Sky.Model;
        Place(Sky, "sky");
    }
    Stats.Materials = Scene.Materials.size();

    if (!Cast::WriteFile(Root, CastPath))
    {
        Error = "cannot write " + CastPath;
        return false;
    }
    return true;
}

namespace
{
    class CastSceneWriter final : public ISceneWriter
    {
    public:
        const char* Extension() const override { return ".cast"; }
        bool Write(const MapScene& Scene, const std::string& Path, const Materials::IAdapter& Adapter,
                   SceneWriteResult& Result, std::string& Error) const override
        {
            CastStats Stats;
            if (!WriteCastScene(Scene, Path, &Adapter, Stats, Error))
            {
                Error = "cast scene: " + Error;
                return false;
            }
            Result.Counters = { { "map_cast_scenes", 1 }, { "map_cast_models", Stats.Models },
                                { "map_cast_instances", Stats.Instances }, { "map_cast_world_meshes", Stats.WorldMeshes } };
            return true;
        }
    };
}

const ISceneWriter& CastWriter()
{
    static const CastSceneWriter Writer;
    return Writer;
}
}
