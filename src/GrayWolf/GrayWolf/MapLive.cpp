#include "stdafx.h"

#include "MapLive.h"

#include "CoDAssetType.h"
#include "ExportNaming.h"
#include "ExportWriters.h"
#include "HalfFloats.h"
#include "Strings.h"
#include "WraithModel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <set>
#include <sstream>

namespace Maps
{
namespace
{
    constexpr uint32_t ColorMapSemantic = 0xA0AB1041;   // colorMap, the same hash from T5 to T8

    // the model file's name for a material (MaterialDescription's sidecar uses the same)
    std::string MaterialName(const std::string& Game) { return Strings::Replace(Game, "@", "_"); }
}

std::string AddMaterial(MapScene& Scene, const XMaterial_t& M)
{
    const std::string Name = MaterialName(M.MaterialName);
    if (Scene.Materials.count(Name)) return Name;
    Material& Out = Scene.Materials[Name];
    Out.Name = Name;
    Out.Techset = M.TechsetName;
    Out.Constants = M.Constants;
    Out.SortKey = M.SortKey;
    for (const XImage_t& I : M.Images)
    {
        if (I.ImageName.empty()) continue;
        Out.Textures.push_back({ I.SemanticHash, I.ImageName });
        Scene.Images[I.ImageName];
    }
    // the colour: what the reader calls the diffuse map, else the colorMap semantic, else the first
    for (const XImage_t& I : M.Images)
        if (Out.ColorMap.empty() && I.ImageUsage == ImageUsageType::DiffuseMap && !I.ImageName.empty()) Out.ColorMap = I.ImageName;
    for (const XImage_t& I : M.Images)
        if (Out.ColorMap.empty() && I.SemanticHash == ColorMapSemantic && !I.ImageName.empty()) Out.ColorMap = I.ImageName;
    if (Out.ColorMap.empty() && !Out.Textures.empty()) Out.ColorMap = Out.Textures.front().Image;
    return Name;
}

bool AddModel(MapScene& Scene, const std::string& Name, const ExportWriters::LiveModel& M,
              const std::function<bool(const XMaterial_t&)>& Drawn, size_t* Hidden)
{
    if (!M.Mesh || !M.Generic || M.Lod < 0 || (size_t)M.Lod >= M.Generic->ModelLods.size()) return false;
    std::set<std::string> Skip;   // the mesh's names of the materials not drawn
    for (const XMaterial_t& Mat : M.Generic->ModelLods[(size_t)M.Lod].Materials)
    {
        if (Drawn && !Drawn(Mat))
        {
            Skip.insert(Mat.MaterialName);
            Skip.insert(MaterialName(Mat.MaterialName));
            continue;
        }
        AddMaterial(Scene, Mat);
    }
    Mesh Out;
    Out.Name = Name;
    for (const WraithSubmesh& Sub : M.Mesh->Submeshes)
    {
        if (Sub.Faces.empty() || Sub.Verticies.empty()) continue;
        Surface Sf;
        const int32_t Mat = Sub.MaterialIndicies.empty() ? -1 : Sub.MaterialIndicies[0];
        if (Mat >= 0 && (size_t)Mat < M.Mesh->Materials.size()) Sf.Material = M.Mesh->Materials[(size_t)Mat].MaterialName;
        if (!Skip.empty() && Skip.count(Sf.Material))
        {
            if (Hidden) *Hidden += Sub.Faces.size();
            continue;
        }
        Sf.Vertices.reserve(Sub.Verticies.size());
        for (const WraithVertex& V : Sub.Verticies)
        {
            Vertex O;
            O.Pos[0] = V.Position.X;
            O.Pos[1] = V.Position.Y;
            O.Pos[2] = V.Position.Z;
            O.Normal[0] = V.Normal.X;
            O.Normal[1] = V.Normal.Y;
            O.Normal[2] = V.Normal.Z;
            if (!V.UVLayers.empty())
            {
                O.Uv[0] = V.UVLayers[0].U;
                O.Uv[1] = V.UVLayers[0].V;
            }
            for (int K = 0; K < 4; K++) O.Color[K] = V.Color[K];
            Sf.Vertices.push_back(O);
        }
        Sf.Indices.reserve(Sub.Faces.size() * 3);
        for (const WraithFace& F : Sub.Faces)
        {
            Sf.Indices.push_back(F.Index1);
            Sf.Indices.push_back(F.Index2);
            Sf.Indices.push_back(F.Index3);
        }
        Out.Surfaces.push_back(std::move(Sf));
    }
    if (Out.Surfaces.empty()) return false;
    Scene.Models[Name] = std::move(Out);
    return true;
}

void KeepTerrain(MapScene& Scene, std::map<std::string, uint64_t>& Models)
{
    Models.clear();
    std::erase_if(Scene.Objects, [](const Mesh& M) { return M.Name.rfind("terrain", 0) != 0; });
    Scene.Models.clear();
    Scene.StaticInstances.clear();
    Scene.DynamicInstances.clear();
    Scene.SplinedInstances.clear();
    Scene.Lights.clear();
    Scene.MapEnts.clear();
    Scene.EntityKeys.clear();
    Scene.Collision = Maps::Collision();
    Scene.Sky = Maps::Sky();
    std::set<std::string> Used, Images;
    for (const Mesh& M : Scene.Objects)
        for (const Surface& S : M.Surfaces) Used.insert(S.Material);
    std::erase_if(Scene.Materials, [&](const auto& M) { return !Used.count(M.first); });
    for (const auto& [Name, M] : Scene.Materials)
    {
        if (!M.ColorMap.empty()) Images.insert(M.ColorMap);
        for (const Texture& T : M.Textures) Images.insert(T.Image);
    }
    std::erase_if(Scene.Images, [&](const auto& I) { return !Images.count(I.first); });
}

FILE* DebugFile()
{
    static FILE* F = [] {
        char Path[MAX_PATH] = {};
        FILE* Out = nullptr;
        if (GetEnvironmentVariableA("GH_MAPDEBUG", Path, MAX_PATH) > 0) fopen_s(&Out, Path, "a");
        return Out;
    }();
    if (F) std::fflush(F);
    return F;
}

Instance PackedPlacement(const std::vector<uint8_t>& Image, int64_t Pos, bool HalfScale)
{
    using ExportNaming::At;
    Instance In;
    for (int A = 0; A < 3; A++) In.Origin[A] = (float)At<int32_t>(Image, Pos + A * 4) / 4096.0f;
    float Q[4];
    for (int A = 0; A < 4; A++) Q[A] = (float)At<uint16_t>(Image, Pos + 12 + A * 2) / 65535.0f * 2.0f - 1.0f;
    AxisFromQuat(Q, In.Axis);
    In.Scale = HalfScale ? HalfFloats::ToFloat(At<uint16_t>(Image, Pos + 20)) : At<float>(Image, Pos + 20);
    return In;
}

std::string InlineName(const std::vector<uint8_t>& Image, int64_t Pos, size_t Max)
{
    std::string S;
    for (size_t I = 0; Pos >= 0 && (size_t)Pos + I < Image.size() && I < Max; I++)
    {
        const uint8_t C = Image[(size_t)Pos + I];
        if (C == 0) return S;
        if (C < 0x20 || C > 0x7e) return std::string();
        S += (char)C;
    }
    return std::string();   // no NUL within Max
}

bool ParseEntities(const std::string& Text, std::vector<Entity>& Out)
{
    size_t I = 0;
    const auto Skip = [&] {
        while (I < Text.size() && std::isspace((unsigned char)Text[I])) I++;
    };
    const auto Quoted = [&](std::string& S) {
        Skip();
        if (I >= Text.size() || Text[I] != '"') return false;
        const size_t End = Text.find('"', I + 1);
        if (End == std::string::npos) return false;
        S = Text.substr(I + 1, End - I - 1);
        I = End + 1;
        return true;
    };
    // a key may also be a bare token (MW3: its script string's number)
    const auto Key = [&](std::string& S) {
        Skip();
        if (I < Text.size() && Text[I] == '"') return Quoted(S);
        const size_t From = I;
        while (I < Text.size() && !std::isspace((unsigned char)Text[I]) && Text[I] != '"' && Text[I] != '}') I++;
        S = Text.substr(From, I - From);
        return !S.empty();
    };
    while (true)
    {
        Skip();
        if (I >= Text.size()) return true;
        if (Text[I++] != '{') return false;
        Entity E;
        while (true)
        {
            Skip();
            if (I < Text.size() && Text[I] == '}')
            {
                I++;
                break;
            }
            std::string K, V;
            if (!Key(K) || !Quoted(V)) return false;
            E[K] = V;
        }
        Out.push_back(std::move(E));
    }
}

bool EntityFloats(const Entity& E, const char* Key, float* Out, int Count)
{
    const auto It = E.find(Key);
    if (It == E.end()) return false;
    std::istringstream In(It->second);
    for (int I = 0; I < Count; I++)
        if (!(In >> Out[I])) return false;
    return true;
}

// rows forward, left, up
void AnglesToAxis(const float A[3], float Axis[9])
{
    constexpr float Rad = 3.14159265358979f / 180.0f;
    const float Sp = std::sin(A[0] * Rad), Cp = std::cos(A[0] * Rad), Sy = std::sin(A[1] * Rad), Cy = std::cos(A[1] * Rad),
                Sr = std::sin(A[2] * Rad), Cr = std::cos(A[2] * Rad);
    const float F[3] = { Cp * Cy, Cp * Sy, -Sp };
    const float R[3] = { -Sr * Sp * Cy + Cr * Sy, -Sr * Sp * Sy - Cr * Cy, -Sr * Cp };
    const float U[3] = { Cr * Sp * Cy + Sr * Sy, Cr * Sp * Sy - Sr * Cy, Cr * Cp };
    for (int I = 0; I < 3; I++)
    {
        Axis[I] = F[I];
        Axis[3 + I] = -R[I];
        Axis[6 + I] = U[I];
    }
}

namespace
{
    void MoveMesh(Mesh& Object, const float Origin[3], const float Axis[9])
    {
        for (auto& Sf : Object.Surfaces)
            for (auto& V : Sf.Vertices)
            {
                const float P[3] = { V.Pos[0], V.Pos[1], V.Pos[2] }, N[3] = { V.Normal[0], V.Normal[1], V.Normal[2] };
                for (int K = 0; K < 3; K++)
                {
                    V.Pos[K] = Origin[K] + P[0] * Axis[K] + P[1] * Axis[3 + K] + P[2] * Axis[6 + K];
                    V.Normal[K] = N[0] * Axis[K] + N[1] * Axis[3 + K] + N[2] * Axis[6 + K];
                }
            }
    }
}

bool PlaceBrushModel(MapScene& Scene, uint32_t N, const float Origin[3], const float Axis[9])
{
    const std::string Name = "brushmodel_" + std::to_string(N);
    for (auto& Object : Scene.Objects)
        if (Object.Name == Name)
        {
            MoveMesh(Object, Origin, Axis);
            return true;
        }
    return false;
}

std::vector<uint32_t> PlaceBrushModels(MapScene& Scene, const std::vector<Entity>& Ents)
{
    std::unordered_map<std::string, const Entity*> ByModel;
    for (const auto& E : Ents)
        if (const auto It = E.find("model"); It != E.end() && !It->second.empty() && It->second[0] == '*')
            ByModel.emplace("brushmodel_" + It->second.substr(1), &E);
    std::vector<uint32_t> Unplaced;
    for (auto& Object : Scene.Objects)
    {
        if (Object.Name.compare(0, 11, "brushmodel_") != 0) continue;
        const auto Found = ByModel.find(Object.Name);
        if (Found == ByModel.end())
        {
            Unplaced.push_back((uint32_t)std::strtoul(Object.Name.c_str() + 11, nullptr, 10));
            continue;
        }
        float Origin[3] = { 0, 0, 0 }, Angles[3] = { 0, 0, 0 }, Axis[9];
        EntityFloats(*Found->second, "origin", Origin, 3);
        EntityFloats(*Found->second, "angles", Angles, 3);
        AnglesToAxis(Angles, Axis);
        MoveMesh(Object, Origin, Axis);
    }
    return Unplaced;
}

void AxisFromQuat(const float Q[4], float Axis[9])
{
    float X = Q[0], Y = Q[1], Z = Q[2], W = Q[3];
    const float L = std::sqrt(X * X + Y * Y + Z * Z + W * W);
    if (L > 1e-8f)
    {
        X /= L; Y /= L; Z /= L; W /= L;
    }
    // rows: the local X, Y and Z axes in world space
    Axis[0] = 1 - 2 * (Y * Y + Z * Z); Axis[1] = 2 * (X * Y + Z * W);     Axis[2] = 2 * (X * Z - Y * W);
    Axis[3] = 2 * (X * Y - Z * W);     Axis[4] = 1 - 2 * (X * X + Z * Z); Axis[5] = 2 * (Y * Z + X * W);
    Axis[6] = 2 * (X * Z + Y * W);     Axis[7] = 2 * (Y * Z - X * W);     Axis[8] = 1 - 2 * (X * X + Y * Y);
}

bool WorldBuilder::Triangle(const std::string& Material, uint32_t A, uint32_t B, uint32_t C)
{
    if (A == B || A == C || B == C) return true;   // degenerate: nothing to draw
    auto [It, New] = Group.try_emplace(Material, Out.Surfaces.size());
    if (New)
    {
        Out.Surfaces.emplace_back();
        Out.Surfaces.back().Material = Material;
        Remap.emplace_back();
    }
    Surface& Sf = Out.Surfaces[It->second];
    auto& Map = Remap[It->second];
    uint32_t Local[3];
    const uint32_t In[3] = { A, B, C };
    for (int K = 0; K < 3; K++)
    {
        const auto Found = Map.find(In[K]);
        if (Found != Map.end())
        {
            Local[K] = Found->second;
            continue;
        }
        Vertex V;
        if (!Read(In[K], V)) return false;
        Local[K] = (uint32_t)Sf.Vertices.size();
        if (MoreUvs)
        {
            // the surface's first vertex says how many sets it has (its material's layers)
            float Uvs[MaxMoreUvs * 2] = {};
            const uint32_t N = std::min(MoreUvs(In[K], Uvs, MaxMoreUvs), MaxMoreUvs);
            if (Sf.Vertices.empty()) Sf.UvSets = 1 + N;
            Sf.MoreUvs.insert(Sf.MoreUvs.end(), Uvs, Uvs + (Sf.UvSets - 1) * 2);
        }
        Sf.Vertices.push_back(V);
        Map.emplace(In[K], Local[K]);
    }
    Sf.Indices.insert(Sf.Indices.end(), Local, Local + 3);
    Count++;
    return true;
}

void BuildWorld(MapScene& Scene, uint32_t BrushCount, uint32_t SurfaceCount, const std::function<std::pair<uint32_t, uint32_t>(uint32_t)>& Range,
                const WorldBuilder::Fetch& Read, const std::function<void(uint32_t, WorldBuilder&)>& Surface, const WorldBuilder::FetchUvs& MoreUvs)
{
    for (uint32_t B = 0; B < BrushCount; B++)
    {
        const auto [First, End] = Range(B);
        if (End <= First || First >= SurfaceCount) continue;
        WorldBuilder Mesh(B == 0 ? "world" : "brushmodel_" + std::to_string(B), Read, MoreUvs);
        for (uint32_t S = First; S < End && S < SurfaceCount; S++) Surface(S, Mesh);
        if (Mesh.Triangles()) Scene.Objects.push_back(Mesh.Take());
    }
}

size_t AddWorld(MapScene& Scene, const WorldLayout& L, const WorldBuilder::Fetch& Read,
                const std::function<std::string(uint64_t)>& MaterialOf)
{
    if (L.Image == nullptr || L.Surfaces < 0 || L.Brushes < 0 || L.Indices < 0) return 0;
    const auto& Img = *L.Image;
    const auto U32 = [&](int64_t At) { return ExportNaming::At<uint32_t>(Img, At); };
    const auto Field = [&](int64_t At) {
        return L.BrushFieldBytes == 2 ? (uint32_t)ExportNaming::At<uint16_t>(Img, At) : U32(At);
    };
    size_t Bad = 0;
    std::unordered_map<uint64_t, std::string> Names;
    const auto Range = [&](uint32_t B) {
        const int64_t Brush = L.Brushes + (int64_t)B * L.BrushSize;
        const uint32_t First = Field(Brush + L.FirstSurface);
        const int32_t Count = (int32_t)Field(Brush + L.SurfaceCountAt);
        return std::make_pair(First, Count <= 0 ? First : First + (uint32_t)Count);
    };
    // the surface being added, for MoreUvs (a vertex's number in it)
    int64_t Current = -1;
    uint32_t CurrentBase = 0;
    WorldBuilder::FetchUvs MoreUvs;
    if (L.MoreUvs && L.VertexBytes == 0)
        MoreUvs = [&](uint32_t I, float* Out, uint32_t Max) { return I >= CurrentBase ? L.MoreUvs(Current, I - CurrentBase, Out, Max) : 0u; };
    BuildWorld(Scene, L.BrushCount, L.SurfaceCount, Range, Read, [&](uint32_t S, WorldBuilder& Mesh) {
        if (L.Skip && L.Skip->count(S)) return;
        const int64_t Sf = L.Surfaces + (int64_t)S * L.SurfaceSize;
        const uint32_t Base = U32(Sf + L.FirstVertex) / std::max(L.FirstVertexUnit, 1u) + (L.VertexBase ? L.VertexBase(Sf) : 0u),
                       BaseIndex = U32(Sf + L.BaseIndex);
        Current = Sf, CurrentBase = Base;
        const uint16_t Tris = ExportNaming::At<uint16_t>(Img, Sf + L.TriCount);
        const uint64_t MatAddress = L.PointerSize == 4 ? ExportNaming::At<uint32_t>(Img, Sf + L.Material)
                                                       : ExportNaming::At<uint64_t>(Img, Sf + L.Material);
        auto Found = Names.find(MatAddress);
        if (Found == Names.end()) Found = Names.emplace(MatAddress, MaterialOf(MatAddress)).first;
        for (uint32_t T = 0; T < Tris; T++)
        {
            const uint32_t I = BaseIndex + T * 3;
            if (I + 2 >= L.IndexCount) break;
            const auto Idx = [&](uint32_t K) {
                const uint32_t Local = ExportNaming::At<uint16_t>(Img, L.Indices + (int64_t)(I + K) * 2);
                return L.VertexBytes ? Base + Local * L.VertexBytes : Local + Base;
            };
            if (!Mesh.Triangle(Found->second, Idx(0), Idx(1), Idx(2))) Bad++;
        }
    }, MoreUvs);
    return Bad;
}
}
