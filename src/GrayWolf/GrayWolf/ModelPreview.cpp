#include "stdafx.h"

#include "ModelPreview.h"
#include "AppLog.h"
#include "CastReader.h"
#include "ExportNaming.h"

#include "Strings.h"
#include "DirectXTex.h"
// Declarations only - TINYGLTF_IMPLEMENTATION is already compiled once, in WraithX's GLTFExport.cpp
// (the map/model .glb writer); defining it again here would duplicate every symbol at link time.
#include <tiny_gltf.h>

#include <objbase.h>
#include <fstream>
#include <sstream>
#include <array>
#include <vector>
#include <string>
#include <utility>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include <map>
#include <thread>
#include <atomic>
#include <set>
#include <chrono>
#include <cfloat>

// Turns an already-loaded DirectXTex image into row-tight RGBA8 - decompressing BC1-BC7 or
// converting any other non-RGBA8 format first; shared with the asset browser's image preview.
bool ScratchImageToRGBA8(DirectX::ScratchImage& Img, const DirectX::TexMetadata& Meta,
                          int& OutW, int& OutH, std::vector<uint8_t>& OutRgba)
{
    DirectX::ScratchImage Work;
    const DirectX::Image* Src = Img.GetImage(0, 0, 0);
    if (DirectX::IsCompressed(Meta.format))
    {
        if (FAILED(DirectX::Decompress(Img.GetImages(), Img.GetImageCount(), Meta, DXGI_FORMAT_R8G8B8A8_UNORM, Work)))
            return false;
        Src = Work.GetImage(0, 0, 0);
    }
    else if (Meta.format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        if (FAILED(DirectX::Convert(Img.GetImages(), Img.GetImageCount(), Meta, DXGI_FORMAT_R8G8B8A8_UNORM,
                                    DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, Work)))
            return false;
        Src = Work.GetImage(0, 0, 0);
    }
    if (!Src) return false;

    OutW = (int)Src->width; OutH = (int)Src->height;
    OutRgba.resize((size_t)OutW * OutH * 4);
    for (int y = 0; y < OutH; y++)
        std::memcpy(OutRgba.data() + (size_t)y * OutW * 4, Src->pixels + (size_t)y * Src->rowPitch, (size_t)OutW * 4);
    return true;
}

bool DecodeImageFileToRGBA8(const std::string& Path, int& OutW, int& OutH, std::vector<uint8_t>& OutRgba)
{
    OutW = OutH = 0;
    std::string Ext = std::filesystem::path(Path).extension().string();
    for (auto& c : Ext) c = (char)tolower((unsigned char)c);
    const std::wstring W = Strings::ToUnicodeString(Path);

    DirectX::TexMetadata Meta{};
    DirectX::ScratchImage Img;
    HRESULT Hr = (Ext == ".dds") ? DirectX::LoadFromDDSFile(W.c_str(), DirectX::DDS_FLAGS_NONE, &Meta, Img)
               : (Ext == ".tga") ? DirectX::LoadFromTGAFile(W.c_str(), &Meta, Img)
               : DirectX::LoadFromWICFile(W.c_str(), DirectX::WIC_FLAGS_NONE, &Meta, Img);
    if (FAILED(Hr)) return false;
    return ScratchImageToRGBA8(Img, Meta, OutW, OutH, OutRgba);
}

bool ReadSEModelHeaderCounts(const std::string& Path, uint32_t& Bones, uint32_t& Meshes, uint32_t& Materials)
{
    // "SEModel"(7), ver(2), hdrSize(2), 3 flag bytes, then boneCount(u32)@14, meshCount@18,
    // materialCount@22.
    Bones = Meshes = Materials = 0;
    std::ifstream In(Path, std::ios::binary);
    uint8_t H[26];
    if (!In.read((char*)H, sizeof(H)) || std::memcmp(H, "SEModel", 7) != 0) return false;
    std::memcpy(&Bones, H + 14, 4);
    std::memcpy(&Meshes, H + 18, 4);
    std::memcpy(&Materials, H + 22, 4);
    return true;
}

// One bulk read of a whole file (one fread, not istreambuf_iterator's byte-at-a-time copy).
bool ReadFileBulk(const std::string& Path, std::vector<uint8_t>& Out)
{
    std::ifstream In(Path, std::ios::binary | std::ios::ate);
    if (!In) return false;
    const std::streamsize Size = In.tellg();
    if (Size < 0) return false;
    In.seekg(0, std::ios::beg);
    Out.resize((size_t)Size);
    return Size == 0 || (bool)In.read(reinterpret_cast<char*>(Out.data()), Size);
}

namespace
{
    struct Vec3 { float x = 0, y = 0, z = 0; };
    struct Vec2 { float u = 0, v = 0; };

    // A forward-only cursor over the model bytes with bounds checks; any over-read trips Bad.
    struct Cursor
    {
        const uint8_t* P;
        size_t N, O = 0;
        bool Bad = false;

        Cursor(const uint8_t* p, size_t n) : P(p), N(n) {}

        void Skip(size_t n) { if (O + n > N) { Bad = true; O = N; } else O += n; }
        uint8_t U8()  { if (O + 1 > N) { Bad = true; return 0; } return P[O++]; }
        uint16_t U16() { uint16_t v = 0; if (O + 2 > N) { Bad = true; return 0; } std::memcpy(&v, P + O, 2); O += 2; return v; }
        uint32_t U32() { uint32_t v = 0; if (O + 4 > N) { Bad = true; return 0; } std::memcpy(&v, P + O, 4); O += 4; return v; }
        int32_t I32()  { return (int32_t)U32(); }
        float F32()   { float v = 0;    if (O + 4 > N) { Bad = true; return 0; } std::memcpy(&v, P + O, 4); O += 4; return v; }
        Vec3 V3()     { Vec3 v; v.x = F32(); v.y = F32(); v.z = F32(); return v; }
        Vec2 V2()     { Vec2 v; v.u = F32(); v.v = F32(); return v; }
        std::string CStr() { std::string s; while (O < N && P[O] != 0) s.push_back((char)P[O++]); if (O < N) O++; else Bad = true; return s; }
    };

    // SEModel presence flags (see SEModelExport.cpp).
    enum { DATA_BONE = 1 << 0, DATA_MESH = 1 << 1, DATA_MAT = 1 << 2 };
    enum { BONE_GLOBAL = 1 << 0, BONE_LOCAL = 1 << 1, BONE_SCALE = 1 << 2 };
    enum { MESH_UV = 1 << 0, MESH_NORMAL = 1 << 1, MESH_COLOR = 1 << 2, MESH_WEIGHT = 1 << 3 };

    struct Bone
    {
        std::string Name;
        int32_t Parent = -1;
        Vec3 Pos{};
        float Qx = 0, Qy = 0, Qz = 0, Qw = 1; // global rotation quaternion
    };

    struct Geo
    {
        std::vector<Vec3> Pos;
        std::vector<Vec3> Nrm;
        std::vector<Vec2> UV;
        std::vector<uint32_t> Idx;   // 3 indices per triangle
        std::vector<int32_t> TriMat; // material index per triangle (into MatName), -1 if none
        std::vector<std::string> MatName;
        // Parallel to MatName: the diffuse map path each material entry itself carries, relative to
        // the model file (e.g. "..\\..\\images\\<texture>.dds") - empty when a material has none.
        std::vector<std::string> MatDiffuse;
        std::vector<std::string> MatNormal, MatSpecular;   // likewise
        std::vector<Bone> Bones;     // skeleton, for part alignment / seating AND skeletal preview
        // Flattened PREVIEW_MAX_BONE_INFLUENCES-per-vertex skin weights (see ModelPreview.h). Empty
        // when the source has no per-vertex weight data, except a single-bone rigid part, whose vertices
        // all weight 1.0 to that bone.
        std::vector<int32_t> VtxBoneIdx;
        std::vector<float> VtxBoneWeight;
    };

    // Parses a SEModel v1 buffer to positions, normals, first-layer UVs, triangles (with per-triangle
    // material index), the material name list, and (for skeletal preview) per-vertex bone weights.
    // Colors are skipped; bones/weights are read (not skipped) exactly per the layout so the stream
    // stays aligned. Returns false on a bad header or a desync.
    bool ParseSEModel(const std::vector<uint8_t>& Bytes, Geo& Out)
    {
        if (Bytes.size() < 26 || std::memcmp(Bytes.data(), "SEModel", 7) != 0)
            return false;

        Cursor C(Bytes.data(), Bytes.size());
        C.Skip(7);                       // magic
        C.Skip(2);                       // version
        C.Skip(2);                       // header size
        const uint8_t DataFlags = C.U8();
        const uint8_t BoneFlags = C.U8();
        const uint8_t MeshFlags = C.U8();
        const uint32_t BoneCount = C.U32();
        const uint32_t MeshCount = C.U32();
        const uint32_t MatCount = C.U32();
        C.Skip(3);                       // reserved

        // a bone takes 6 bytes at least: a count beyond the file is a corrupt header, not a reserve
        if ((DataFlags & DATA_BONE) && BoneCount > Bytes.size() / 6)
            return false;

        if (DataFlags & DATA_BONE)
        {
            std::vector<std::string> Names;
            Names.reserve(BoneCount);
            for (uint32_t i = 0; i < BoneCount; i++) Names.push_back(C.CStr());   // tags
            for (uint32_t i = 0; i < BoneCount; i++)
            {
                Bone B;
                B.Name = (i < Names.size()) ? Names[i] : std::string();
                C.Skip(1);                                  // flags
                B.Parent = C.I32();                         // parent index
                if (BoneFlags & BONE_GLOBAL)                // global pos + rotation quaternion (x,y,z,w)
                {
                    B.Pos = C.V3();
                    B.Qx = C.F32(); B.Qy = C.F32(); B.Qz = C.F32(); B.Qw = C.F32();
                }
                if (BoneFlags & BONE_LOCAL)  { C.Skip(28); }
                if (BoneFlags & BONE_SCALE)  { C.Skip(12); }
                Out.Bones.push_back(std::move(B));
            }
        }

        const size_t WeightIdSize = (BoneCount <= 0xFF) ? 1 : (BoneCount <= 0xFFFF) ? 2 : 4;

        for (uint32_t m = 0; m < MeshCount && !C.Bad; m++)
        {
            C.Skip(1);                   // mesh flags
            const uint8_t MeshMatCount = C.U8();
            const uint8_t MaxSkin = C.U8();
            const uint32_t VtxCount = C.U32();
            const uint32_t FaceCount = C.U32();
            if (C.Bad) return false;
            const size_t FaceIdx = (VtxCount <= 0xFF) ? 1 : (VtxCount <= 0xFFFF) ? 2 : 4;
            if ((uint64_t)VtxCount * 12 + (uint64_t)FaceCount * 3 * FaceIdx > C.N - C.O) return false;

            const uint32_t Base = (uint32_t)Out.Pos.size();

            for (uint32_t v = 0; v < VtxCount; v++) Out.Pos.push_back(C.V3());

            // UV layers: MeshMatCount sets per vertex; keep layer 0.
            Out.UV.resize(Out.Pos.size());
            if (MeshFlags & MESH_UV)
            {
                for (uint32_t v = 0; v < VtxCount; v++)
                {
                    for (uint8_t l = 0; l < MeshMatCount; l++)
                    {
                        Vec2 uv = C.V2();
                        if (l == 0) Out.UV[Base + v] = uv;
                    }
                }
            }

            Out.Nrm.resize(Out.Pos.size());
            if (MeshFlags & MESH_NORMAL)
                for (uint32_t v = 0; v < VtxCount; v++) Out.Nrm[Base + v] = C.V3();

            if (MeshFlags & MESH_COLOR)  C.Skip((size_t)VtxCount * 4);

            // Weights: MaxSkin fixed slots per vertex (unused slots already zero-weighted by the
            // writer - see SEModelExport.cpp), each (boneId as WeightIdSize bytes, weight as float32).
            // Keep only the first PREVIEW_MAX_BONE_INFLUENCES slots and renormalize if MaxSkin exceeds that.
            Out.VtxBoneIdx.resize(Out.Pos.size() * PREVIEW_MAX_BONE_INFLUENCES, -1);
            Out.VtxBoneWeight.resize(Out.Pos.size() * PREVIEW_MAX_BONE_INFLUENCES, 0.0f);
            if (MeshFlags & MESH_WEIGHT)
            {
                for (uint32_t v = 0; v < VtxCount; v++)
                {
                    float Sum = 0.0f;
                    for (uint8_t s = 0; s < MaxSkin; s++)
                    {
                        const uint32_t Bi = (WeightIdSize == 1) ? C.U8() : (WeightIdSize == 2) ? C.U16() : C.U32();
                        const float W = C.F32();
                        if (s < PREVIEW_MAX_BONE_INFLUENCES && W > 0.0f)
                        {
                            const size_t Slot = (Base + v) * PREVIEW_MAX_BONE_INFLUENCES + s;
                            Out.VtxBoneIdx[Slot] = (int32_t)Bi; Out.VtxBoneWeight[Slot] = W; Sum += W;
                        }
                    }
                    if (Sum > 1e-6f && std::abs(Sum - 1.0f) > 1e-4f) // renormalize only if it doesn't already sum to ~1
                        for (uint8_t s = 0; s < PREVIEW_MAX_BONE_INFLUENCES; s++)
                            Out.VtxBoneWeight[(Base + v) * PREVIEW_MAX_BONE_INFLUENCES + s] /= Sum;
                }
            }

            const size_t FirstTri = Out.Idx.size() / 3;
            for (uint32_t f = 0; f < FaceCount && !C.Bad; f++)
            {
                uint32_t a, b, c;
                if (FaceIdx == 1) { a = C.U8(); b = C.U8(); c = C.U8(); }
                else if (FaceIdx == 2) { a = C.U16(); b = C.U16(); c = C.U16(); }
                else { a = C.U32(); b = C.U32(); c = C.U32(); }
                if (a >= VtxCount || b >= VtxCount || c >= VtxCount) return false;   // a face past the mesh
                Out.Idx.push_back(Base + a);
                Out.Idx.push_back(Base + b);
                Out.Idx.push_back(Base + c);
            }

            // Per-layer material reference indices; layer 0 is this mesh's material for every triangle.
            int32_t MeshMat = -1;
            for (uint8_t l = 0; l < MeshMatCount; l++) { int32_t mi = C.I32(); if (l == 0) MeshMat = mi; }
            const size_t TriEnd = Out.Idx.size() / 3;
            Out.TriMat.resize(TriEnd, -1);
            for (size_t t = FirstTri; t < TriEnd; t++) Out.TriMat[t] = MeshMat;
        }

        // Material block: name, isSimple(1), diffuse, normal, specular.
        if (DataFlags & DATA_MAT)
        {
            for (uint32_t i = 0; i < MatCount && !C.Bad; i++)
            {
                Out.MatName.push_back(C.CStr());
                C.Skip(1); // isSimpleMaterial
                Out.MatDiffuse.push_back(C.CStr()); // diffuse map, relative to the model file
                Out.MatNormal.push_back(C.CStr());
                Out.MatSpecular.push_back(C.CStr());
            }
        }

        if (Out.Nrm.size() < Out.Pos.size()) Out.Nrm.resize(Out.Pos.size());
        if (Out.UV.size()  < Out.Pos.size()) Out.UV.resize(Out.Pos.size());

        // No MESH_WEIGHT data but exactly one bone: every vertex rides that bone (rigid single-bone parts,
        // most weapon attachments). Anything else with no weight data stays unweighted rather than guess.
        if (!(MeshFlags & MESH_WEIGHT) && Out.Bones.size() == 1 && !Out.Pos.empty())
        {
            Out.VtxBoneIdx.assign(Out.Pos.size() * PREVIEW_MAX_BONE_INFLUENCES, -1);
            Out.VtxBoneWeight.assign(Out.Pos.size() * PREVIEW_MAX_BONE_INFLUENCES, 0.0f);
            for (size_t v = 0; v < Out.Pos.size(); v++) { Out.VtxBoneIdx[v * PREVIEW_MAX_BONE_INFLUENCES] = 0; Out.VtxBoneWeight[v * PREVIEW_MAX_BONE_INFLUENCES] = 1.0f; }
        }
        return !Out.Idx.empty(); // tolerate a truncated material tail
    }

    inline Vec3 Sub(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    inline Vec3 Cross(const Vec3& a, const Vec3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
    inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline Vec3 Norm(const Vec3& a) { float l = std::sqrt(Dot(a, a)); return l > 1e-8f ? Vec3{ a.x / l, a.y / l, a.z / l } : Vec3{ 0,0,1 }; }

    struct Quat { float x = 0, y = 0, z = 0, w = 1; };
    inline Quat QConj(const Quat& q) { return { -q.x, -q.y, -q.z, q.w }; }
    // Rotate a vector by a unit quaternion: v + 2w(u x v) + 2(u x (u x v)), u = q.xyz.
    inline Vec3 QRot(const Quat& q, const Vec3& v)
    {
        const Vec3 u{ q.x, q.y, q.z };
        const Vec3 t{ 2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x) };
        const Vec3 c{ u.y * t.z - u.z * t.y, u.z * t.x - u.x * t.z, u.x * t.y - u.y * t.x };
        return { v.x + q.w * t.x + c.x, v.y + q.w * t.y + c.y, v.z + q.w * t.z + c.z };
    }

    // Standard (Shepperd's method) 3x3 rotation matrix -> unit quaternion, row-major M[row][col].
    inline Quat MatToQuat(const float M[3][3])
    {
        Quat q;
        const float Tr = M[0][0] + M[1][1] + M[2][2];
        if (Tr > 0.0f)
        {
            const float S = std::sqrt(Tr + 1.0f) * 2.0f;
            q.w = 0.25f * S;
            q.x = (M[2][1] - M[1][2]) / S; q.y = (M[0][2] - M[2][0]) / S; q.z = (M[1][0] - M[0][1]) / S;
        }
        else if (M[0][0] > M[1][1] && M[0][0] > M[2][2])
        {
            const float S = std::sqrt(1.0f + M[0][0] - M[1][1] - M[2][2]) * 2.0f;
            q.w = (M[2][1] - M[1][2]) / S; q.x = 0.25f * S;
            q.y = (M[0][1] + M[1][0]) / S; q.z = (M[0][2] + M[2][0]) / S;
        }
        else if (M[1][1] > M[2][2])
        {
            const float S = std::sqrt(1.0f + M[1][1] - M[0][0] - M[2][2]) * 2.0f;
            q.w = (M[0][2] - M[2][0]) / S; q.x = (M[0][1] + M[1][0]) / S;
            q.y = 0.25f * S; q.z = (M[1][2] + M[2][1]) / S;
        }
        else
        {
            const float S = std::sqrt(1.0f + M[2][2] - M[0][0] - M[1][1]) * 2.0f;
            q.w = (M[1][0] - M[0][1]) / S; q.x = (M[0][2] + M[2][0]) / S;
            q.y = (M[1][2] + M[2][1]) / S; q.z = 0.25f * S;
        }
        return q;
    }

    inline Quat QMulQ(const Quat& a, const Quat& b)
    {
        return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                 a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
    }

    // Parses a .cast model (CastReader.h) into the same Geo as ParseSEModel: its skeleton (world
    // transforms, composed from the local ones when a file has only those), every mesh (positions,
    // normals, the first UV layer, triangles wound back as SEModel has them, the weights of
    // the first PREVIEW_MAX_BONE_INFLUENCES influences, the mesh's material) and the materials with
    // their albedo (or diffuse), normal and specular file paths, relative to the model file.
    bool ParseCastModel(const std::vector<uint8_t>& Bytes, Geo& Out)
    {
        std::vector<CastFile::Node> Roots;
        if (!CastFile::Parse(Bytes, Roots)) return false;
        const CastFile::Node* Model = nullptr;
        for (const auto& Root : Roots)
            for (const auto& Child : Root.Children)
                if (Model == nullptr && Child.Id == CastFile::IdModel) Model = &Child;
        if (Model == nullptr) return false;

        const auto V3 = [](const CastFile::Property* P, size_t I) {
            return (P && P->Floats() >= (I + 1) * 3) ? Vec3{ P->Float(I * 3), P->Float(I * 3 + 1), P->Float(I * 3 + 2) } : Vec3{};
        };
        const auto Q4 = [](const CastFile::Property* P) {
            return (P && P->Floats() >= 4) ? Quat{ P->Float(0), P->Float(1), P->Float(2), P->Float(3) } : Quat{};
        };

        std::unordered_map<uint64_t, int32_t> MaterialIndex;
        for (const auto& Node : Model->Children)
        {
            if (Node.Id == CastFile::IdSkeleton)
            {
                for (const auto& B : Node.Children)
                {
                    if (B.Id != CastFile::IdBone) continue;
                    Bone Parsed;
                    Parsed.Name = B.String("n");
                    Parsed.Parent = (int32_t)(uint32_t)B.Integer("p", 0xFFFFFFFF);
                    Quat Q;
                    if (B.Find("wp") != nullptr || B.Find("wr") != nullptr)
                    {
                        Parsed.Pos = V3(B.Find("wp"), 0);
                        Q = Q4(B.Find("wr"));
                    }
                    else if (Parsed.Parent >= 0 && (size_t)Parsed.Parent < Out.Bones.size())
                    {
                        const Bone& P = Out.Bones[Parsed.Parent];
                        const Quat PQ{ P.Qx, P.Qy, P.Qz, P.Qw };
                        const Vec3 L = QRot(PQ, V3(B.Find("lp"), 0));
                        Parsed.Pos = { P.Pos.x + L.x, P.Pos.y + L.y, P.Pos.z + L.z };
                        Q = QMulQ(PQ, Q4(B.Find("lr")));
                    }
                    else
                    {
                        Parsed.Pos = V3(B.Find("lp"), 0);
                        Q = Q4(B.Find("lr"));
                    }
                    Parsed.Qx = Q.x; Parsed.Qy = Q.y; Parsed.Qz = Q.z; Parsed.Qw = Q.w;
                    Out.Bones.push_back(std::move(Parsed));
                }
            }
            else if (Node.Id == CastFile::IdMaterial)
            {
                std::unordered_map<uint64_t, std::string> Files;
                for (const auto& F : Node.Children)
                    if (F.Id == CastFile::IdFile) Files[F.Hash] = F.String("p");
                const auto Slot = [&](const char* A, const char* B) {
                    for (const char* Name : { A, B })
                        if (Name != nullptr)
                            if (auto It = Files.find(Node.Integer(Name)); Node.Find(Name) && It != Files.end()) return It->second;
                    return std::string();
                };
                MaterialIndex[Node.Hash] = (int32_t)Out.MatName.size();
                Out.MatName.push_back(Node.String("n"));
                Out.MatDiffuse.push_back(Slot("albedo", "diffuse"));
                Out.MatNormal.push_back(Slot("normal", nullptr));
                Out.MatSpecular.push_back(Slot("specular", nullptr));
            }
        }

        bool Weighted = false;
        for (const auto& Mesh : Model->Children)
        {
            if (Mesh.Id != CastFile::IdMesh) continue;
            const auto* Vp = Mesh.Find("vp");
            const auto* Vn = Mesh.Find("vn");
            const auto* U0 = Mesh.Find("u0");
            const auto* F = Mesh.Find("f");
            const auto* Wb = Mesh.Find("wb");
            const auto* Wv = Mesh.Find("wv");
            if (Vp == nullptr || F == nullptr) continue;
            const size_t Count = Vp->Floats() / 3;
            const size_t MaxInfluence = (size_t)Mesh.Integer("mi", 0);
            const uint32_t Base = (uint32_t)Out.Pos.size();

            for (size_t V = 0; V < Count; V++)
            {
                Out.Pos.push_back(V3(Vp, V));
                Out.Nrm.push_back(V3(Vn, V));
                Out.UV.push_back((U0 && U0->Floats() >= (V + 1) * 2) ? Vec2{ U0->Float(V * 2), U0->Float(V * 2 + 1) } : Vec2{});
            }

            Out.VtxBoneIdx.resize(Out.Pos.size() * PREVIEW_MAX_BONE_INFLUENCES, -1);
            Out.VtxBoneWeight.resize(Out.Pos.size() * PREVIEW_MAX_BONE_INFLUENCES, 0.0f);
            if (MaxInfluence > 0 && Wb && Wv && Wb->Count >= Count * MaxInfluence && Wv->Floats() >= Count * MaxInfluence)
            {
                Weighted = true;
                for (size_t V = 0; V < Count; V++)
                {
                    float Sum = 0.0f;
                    size_t Slot = 0;
                    for (size_t S = 0; S < MaxInfluence && Slot < PREVIEW_MAX_BONE_INFLUENCES; S++)
                    {
                        const float W = Wv->Float(V * MaxInfluence + S);
                        if (W <= 0.0f) continue;
                        const size_t At = (Base + V) * PREVIEW_MAX_BONE_INFLUENCES + Slot++;
                        Out.VtxBoneIdx[At] = (int32_t)Wb->Integer(V * MaxInfluence + S);
                        Out.VtxBoneWeight[At] = W;
                        Sum += W;
                    }
                    if (Sum > 1e-6f && std::abs(Sum - 1.0f) > 1e-4f)
                        for (size_t S = 0; S < PREVIEW_MAX_BONE_INFLUENCES; S++)
                            Out.VtxBoneWeight[(Base + V) * PREVIEW_MAX_BONE_INFLUENCES + S] /= Sum;
                }
            }

            // Cast's triangles are wound the other way (CastMeshBuilder::Face): back to SEModel's
            for (uint32_t T = 0; T + 2 < F->Count; T += 3)
            {
                const uint64_t A = F->Integer(T), B = F->Integer(T + 1), C = F->Integer(T + 2);
                if (A >= Count || B >= Count || C >= Count) return false;
                Out.Idx.push_back(Base + (uint32_t)C);
                Out.Idx.push_back(Base + (uint32_t)B);
                Out.Idx.push_back(Base + (uint32_t)A);
            }
            int32_t Mat = -1;
            if (const auto* M = Mesh.Find("m"))
                if (auto It = MaterialIndex.find(M->Integer(0)); It != MaterialIndex.end()) Mat = It->second;
            Out.TriMat.resize(Out.Idx.size() / 3, Mat);
        }

        // as ParseSEModel: no weights and one bone, every vertex rides it
        if (!Weighted && Out.Bones.size() == 1 && !Out.Pos.empty())
            for (size_t V = 0; V < Out.Pos.size(); V++)
            {
                Out.VtxBoneIdx[V * PREVIEW_MAX_BONE_INFLUENCES] = 0;
                Out.VtxBoneWeight[V * PREVIEW_MAX_BONE_INFLUENCES] = 1.0f;
            }
        return !Out.Idx.empty();
    }

    // Parses a Wraith-written XMODEL_EXPORT (text) buffer into the same Geo shape ParseSEModel
    // produces, so both formats share the rest of the preview pipeline (seating, rasterizing,
    // GPU upload). Mirrors XMEExport.cpp's writer line for line: NUMBONES/BONE (declaration, then
    // one OFFSET/SCALE/X/Y/Z transform block per bone), NUMVERTS(32)/VERT(32) (position + a
    // "BONES k" weight list we don't need for a static preview), NUMFACES/TRI (three
    // VERT/NORMAL/COLOR/UV corner records per face - COLOR is skipped too), NUMOBJECTS/OBJECT
    // (submesh names, not needed for rendering, skipped), NUMMATERIALS/MATERIAL (name + optional
    // "color:<path>" diffuse - the COLOR/TRANSPARENCY/... Phong tail is skipped up to the next
    // MATERIAL line or EOF). Driven by the NUM* counters rather than by sniffing line content, so
    // it never has to disambiguate "BONE" (declaration / transform header / vertex weight all
    // start with it) or "VERT" vs "VERT32" - each is read only where the fixed grammar puts it.
    bool ParseXModelExport(const std::vector<uint8_t>& Bytes, Geo& Out)
    {
        if (Bytes.empty()) return false;
        std::string Text(reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
        if (Text.find("MODEL") == std::string::npos) return false;

        std::istringstream In(Text);
        std::string Line;
        auto Trim = [](std::string s) {
            const size_t a = s.find_first_not_of(" \t\r\n");
            if (a == std::string::npos) return std::string();
            const size_t b = s.find_last_not_of(" \t\r\n");
            return s.substr(a, b - a + 1);
        };
        auto NextNonBlank = [&]() -> bool {
            while (std::getline(In, Line)) { Line = Trim(Line); if (!Line.empty() && Line[0] != '/') return true; }
            return false;
        };
        // "VERT"/"NUMVERTS" are prefixes of "VERT32"/"NUMVERTS32" - sscanf's lenient literal
        // matching would silently misparse "VERT32 5" as VERT-index 32, so match the exact
        // keyword (with its trailing separator) explicitly instead.
        auto ReadIdxAfter = [](const std::string& L, const char* Kw) -> long {
            const size_t N = std::strlen(Kw);
            if (L.compare(0, N, Kw) != 0) return -1;
            return std::atol(L.c_str() + N);
        };
        // Manual int/float readers (not sscanf, which this project builds with as an error under
        // C4996) that advance a cursor past comma/space-separated fields, e.g. "OFFSET 1.0, 2.0, 3.0".
        auto NextInt = [](const char*& p) -> long {
            while (*p == ' ' || *p == ',' || *p == '\t') p++;
            char* End = nullptr; const long V = std::strtol(p, &End, 10); p = End; return V;
        };
        auto NextFloat = [](const char*& p) -> float {
            while (*p == ' ' || *p == ',' || *p == '\t') p++;
            char* End = nullptr; const float V = std::strtof(p, &End); p = End; return V;
        };

        // Skip the "MODEL" / "VERSION 6" header lines by searching forward for NUMBONES rather than
        // assuming a fixed line count - tolerates header changes and the leading "//" comment lines.
        uint32_t NumBones = 0;
        {
            bool Found = false;
            while (NextNonBlank()) { const long V = ReadIdxAfter(Line, "NUMBONES "); if (V >= 0) { NumBones = (uint32_t)V; Found = true; break; } }
            if (!Found) return false;
        }

        std::vector<std::string> BoneName(NumBones);
        std::vector<int32_t> BoneParent(NumBones, -1);
        std::vector<Vec3> BonePos(NumBones);
        std::vector<Quat> BoneRot(NumBones);

        for (uint32_t i = 0; i < NumBones; i++)
        {
            if (!NextNonBlank()) return false; // BONE idx parent "name"
            int Idx = 0, Parent = -1;
            { const char* p = Line.c_str() + 5; Idx = (int)NextInt(p); Parent = (int)NextInt(p); } // "BONE "
            const size_t Q0 = Line.find('"'), Q1 = Line.rfind('"');
            const std::string Name = (Q1 != std::string::npos && Q1 > Q0) ? Line.substr(Q0 + 1, Q1 - Q0 - 1) : std::string();
            if (Idx >= 0 && (uint32_t)Idx < NumBones) { BoneName[Idx] = Name; BoneParent[Idx] = Parent; }
        }

        auto ReadXyz = [&](const char* Kw, float& a, float& b, float& c) {
            const size_t N = std::strlen(Kw);
            const char* p = Line.c_str() + (Line.compare(0, N, Kw) == 0 ? N : 0);
            a = NextFloat(p); b = NextFloat(p); c = NextFloat(p);
        };
        for (uint32_t i = 0; i < NumBones; i++)
        {
            if (!NextNonBlank()) return false; // "BONE idx" transform header
            int Idx = -1; { const char* p = Line.c_str() + 5; Idx = (int)NextInt(p); } // "BONE "
            Vec3 Pos{}; float M[3][3] = { {1,0,0},{0,1,0},{0,0,1} };
            for (int f = 0; f < 5; f++) // OFFSET, SCALE, X, Y, Z
            {
                if (!NextNonBlank()) return false;
                float a = 0, b = 0, c = 0;
                if (Line.compare(0, 7, "OFFSET ") == 0)     { ReadXyz("OFFSET ", a, b, c); Pos = { a, b, c }; }
                else if (Line.compare(0, 2, "X ") == 0)     { ReadXyz("X ", a, b, c); M[0][0] = a; M[0][1] = b; M[0][2] = c; }
                else if (Line.compare(0, 2, "Y ") == 0)     { ReadXyz("Y ", a, b, c); M[1][0] = a; M[1][1] = b; M[1][2] = c; }
                else if (Line.compare(0, 2, "Z ") == 0)     { ReadXyz("Z ", a, b, c); M[2][0] = a; M[2][1] = b; M[2][2] = c; }
                // SCALE is read (implicitly skipped) but not applied - CoD static exports are unscaled.
            }
            if (Idx >= 0 && (uint32_t)Idx < NumBones) { BonePos[Idx] = Pos; BoneRot[Idx] = MatToQuat(M); }
        }
        for (uint32_t i = 0; i < NumBones; i++)
        {
            Bone B; B.Name = BoneName[i]; B.Parent = BoneParent[i]; B.Pos = BonePos[i];
            B.Qx = BoneRot[i].x; B.Qy = BoneRot[i].y; B.Qz = BoneRot[i].z; B.Qw = BoneRot[i].w;
            Out.Bones.push_back(std::move(B));
        }

        if (!NextNonBlank()) return false; // NUMVERTS or NUMVERTS32
        long NV = ReadIdxAfter(Line, "NUMVERTS32 ");
        if (NV < 0) NV = ReadIdxAfter(Line, "NUMVERTS ");
        const uint32_t NumVerts = (uint32_t)std::max(0L, NV);
        Out.Pos.assign(NumVerts, Vec3{}); Out.Nrm.assign(NumVerts, Vec3{}); Out.UV.assign(NumVerts, Vec2{});
        Out.VtxBoneIdx.assign((size_t)NumVerts * PREVIEW_MAX_BONE_INFLUENCES, -1);
        Out.VtxBoneWeight.assign((size_t)NumVerts * PREVIEW_MAX_BONE_INFLUENCES, 0.0f);

        for (uint32_t v = 0; v < NumVerts; v++)
        {
            if (!NextNonBlank()) return false; // VERT idx / VERT32 idx
            long Idx = ReadIdxAfter(Line, "VERT32 ");
            if (Idx < 0) Idx = ReadIdxAfter(Line, "VERT ");
            if (!NextNonBlank()) return false; // OFFSET x, y, z
            float x = 0, y = 0, z = 0; ReadXyz("OFFSET ", x, y, z);
            if (Idx >= 0 && (uint32_t)Idx < NumVerts) Out.Pos[Idx] = { x, y, z };
            if (!NextNonBlank()) return false; // BONES k
            const long K = std::max(0L, ReadIdxAfter(Line, "BONES "));
            float WSum = 0.0f;
            for (long w = 0; w < K; w++)
            {
                if (!NextNonBlank()) return false; // BONE boneIdx weight
                int Bi = 0; float W = 0.0f;
                { const char* p = Line.c_str() + 5; Bi = (int)NextInt(p); W = NextFloat(p); } // "BONE "
                if (w < PREVIEW_MAX_BONE_INFLUENCES && W > 0.0f && Idx >= 0 && (uint32_t)Idx < NumVerts)
                {
                    const size_t Slot = (size_t)Idx * PREVIEW_MAX_BONE_INFLUENCES + w;
                    Out.VtxBoneIdx[Slot] = Bi; Out.VtxBoneWeight[Slot] = W; WSum += W;
                }
            }
            if (Idx >= 0 && (uint32_t)Idx < NumVerts && WSum > 1e-6f && std::abs(WSum - 1.0f) > 1e-4f)
                for (int s = 0; s < PREVIEW_MAX_BONE_INFLUENCES; s++)
                    Out.VtxBoneWeight[(size_t)Idx * PREVIEW_MAX_BONE_INFLUENCES + s] /= WSum;
        }

        if (!NextNonBlank()) return false; // NUMFACES
        const uint32_t NumFaces = (uint32_t)std::max(0L, ReadIdxAfter(Line, "NUMFACES "));
        Out.Idx.reserve((size_t)NumFaces * 3);
        Out.TriMat.reserve(NumFaces);

        for (uint32_t f = 0; f < NumFaces; f++)
        {
            if (!NextNonBlank()) return false; // TRI submesh material 0 0
            int Submesh = 0, Mat = -1;
            { const char* p = Line.c_str() + 4; Submesh = (int)NextInt(p); Mat = (int)NextInt(p); } // "TRI "

            uint32_t Corner[3] = {};
            for (int c = 0; c < 3; c++)
            {
                if (!NextNonBlank()) return false; // VERT idx / VERT32 idx
                long VIdx = ReadIdxAfter(Line, "VERT32 ");
                if (VIdx < 0) VIdx = ReadIdxAfter(Line, "VERT ");
                if (!NextNonBlank()) return false; // NORMAL x y z
                float nx = 0, ny = 0, nz = 0;
                { const char* p = Line.c_str() + 7; nx = NextFloat(p); ny = NextFloat(p); nz = NextFloat(p); } // "NORMAL "
                if (!NextNonBlank()) return false; // COLOR r g b a - unused for a textured preview
                if (!NextNonBlank()) return false; // UV 1 u v
                int Layer = 0; float u = 0, uv2 = 0;
                { const char* p = Line.c_str() + 3; Layer = (int)NextInt(p); u = NextFloat(p); uv2 = NextFloat(p); } // "UV "

                if (VIdx >= 0 && (uint32_t)VIdx < Out.Pos.size()) { Out.Nrm[VIdx] = { nx, ny, nz }; Out.UV[VIdx] = { u, uv2 }; }
                Corner[c] = (VIdx >= 0) ? (uint32_t)VIdx : 0;
            }
            Out.Idx.push_back(Corner[0]); Out.Idx.push_back(Corner[1]); Out.Idx.push_back(Corner[2]);
            Out.TriMat.push_back(Mat);
        }

        // NUMOBJECTS/OBJECT: submesh names, not needed for rendering - skip the whole block. Then
        // NUMMATERIALS/MATERIAL. Both are always present in our own writer's output (see
        // XMEExport.cpp); a truncated file just means "no materials, but the mesh itself is good".
        if (!NextNonBlank()) return !Out.Idx.empty();
        const long NumObjects = std::max(0L, ReadIdxAfter(Line, "NUMOBJECTS "));
        for (long i = 0; i < NumObjects; i++) if (!NextNonBlank()) return !Out.Idx.empty();

        if (!NextNonBlank()) return !Out.Idx.empty();
        const long NumMaterials = std::max(0L, ReadIdxAfter(Line, "NUMMATERIALS "));
        Out.MatName.resize((size_t)NumMaterials);
        for (long i = 0; i < NumMaterials; i++)
        {
            if (!NextNonBlank()) break;
            int Idx = 0; { const char* p = Line.c_str() + 9; Idx = (int)NextInt(p); } // "MATERIAL "
            const size_t Q0 = Line.find('"'), Q1 = (Q0 != std::string::npos) ? Line.find('"', Q0 + 1) : std::string::npos;
            const std::string Name = (Q1 != std::string::npos) ? Line.substr(Q0 + 1, Q1 - Q0 - 1) : std::string();
            if (Idx >= 0 && (uint32_t)Idx < (uint32_t)NumMaterials) Out.MatName[Idx] = Name;

            // Skip this material's COLOR/TRANSPARENCY/.../PHONG tail up to the next MATERIAL line.
            std::streampos Save = In.tellg();
            while (NextNonBlank())
            {
                if (Line.compare(0, 9, "MATERIAL ") == 0) { In.clear(); In.seekg(Save); break; }
                Save = In.tellg();
            }
        }

        return !Out.Idx.empty();
    }

    // A decoded RGBA texture in memory.
    struct Tex
    {
        int W = 0, H = 0;
        std::vector<uint8_t> Rgba;
        bool Ok() const { return W > 0 && H > 0 && !Rgba.empty(); }

        void Sample(float u, float v, float& r, float& g, float& b) const
        {
            // Wrap. The loaded UVs have a top-left origin, as the image rows and the GPU preview's
            // (a flip here showed every texture upside down: decal text mirrored)
            float fu = u - std::floor(u);
            float fv = v - std::floor(v);
            float fx = fu * (W - 1), fy = fv * (H - 1);
            int x0 = (int)fx, y0 = (int)fy;
            int x1 = std::min(x0 + 1, W - 1), y1 = std::min(y0 + 1, H - 1);
            float tx = fx - x0, ty = fy - y0;
            auto px = [&](int x, int y, int c) { return (float)Rgba[((size_t)y * W + x) * 4 + c]; };
            for (int c = 0; c < 3; c++)
            {
                float top = px(x0, y0, c) * (1 - tx) + px(x1, y0, c) * tx;
                float bot = px(x0, y1, c) * (1 - tx) + px(x1, y1, c) * tx;
                float val = top * (1 - ty) + bot * ty;
                (c == 0 ? r : c == 1 ? g : b) = val;
            }
        }
    };

    // Loads a PNG/DDS/TGA into an RGBA8 Tex via the shared decoder above. Empty Tex on failure.
    Tex LoadTexture(const std::wstring& Path)
    {
        Tex T;
        DecodeImageFileToRGBA8(Strings::ToNormalString(Path), T.W, T.H, T.Rgba);
        return T;
    }

    // Finds a material's diffuse (colour) map on disk, for exports that don't carry an embedded
    // path (see ResolveDiffuseTexture below): the older <modelDir>/_images/<materialName>/ layout
    // with a "_c" suffix. Falls back to any PNG/DDS/TGA in that folder.
    std::wstring FindDiffuse(const std::filesystem::path& ModelDir, const std::string& MatName)
    {
        namespace fs = std::filesystem;
        std::error_code Ec;
        fs::path Folder = ModelDir / "_images" / MatName;
        if (!fs::is_directory(Folder, Ec)) return {};

        fs::path FirstPng;
        for (auto& E : fs::directory_iterator(Folder, Ec))
        {
            if (Ec || !E.is_regular_file(Ec)) continue;
            auto P = E.path();
            auto Ext = P.extension().string();
            for (auto& c : Ext) c = (char)tolower((unsigned char)c);
            if (Ext != ".png" && Ext != ".dds" && Ext != ".tga") continue;
            if (FirstPng.empty()) FirstPng = P;
            auto Stem = P.stem().string();
            for (auto& c : Stem) c = (char)tolower((unsigned char)c);
            // "_c" or "_c-<hash>" marks the colour map.
            auto pos = Stem.rfind("_c");
            if (pos != std::string::npos && (pos + 2 == Stem.size() || Stem[pos + 2] == '-'))
                return P.wstring();
        }
        return FirstPng.wstring();
    }

    // Resolves a material's diffuse texture: the path the .semodel entry carries (relative to the
    // model file; the OS collapses its doubled backslashes), else the older per-material _images/
    // folder scan above.
    std::wstring ResolveDiffuseTexture(const std::filesystem::path& ModelDir, const std::string& MatName,
                                        const std::string& MatDiffuse)
    {
        if (!MatDiffuse.empty())
        {
            std::error_code Ec;
            const std::filesystem::path P = ModelDir / MatDiffuse;
            if (std::filesystem::is_regular_file(P, Ec)) return P.wstring();
        }
        return FindDiffuse(ModelDir, MatName);
    }
}

namespace
{
    // A model file's materials made ready for the GPU preview, sharing one texture cache and one
    // directory listing per folder for every part of a load.
    struct PreviewMaterials
    {
        std::vector<PreviewMaterial> List;

        // The exporter's description of the model file's materials (<model>.materials.json),
        // paths made absolute; a model without one is described by the default adapter.
        void ReadSidecar(const std::string& ModelPath)
        {
            Described.clear();
            Materials::Sidecar S;
            if (!Materials::ReadSidecar(Materials::SidecarPath(ModelPath), S)) return;
            const std::filesystem::path Dir = std::filesystem::path(ModelPath).parent_path();
            for (auto& D : S.Materials)
            {
                std::vector<Materials::Map*> Maps = { &D.Albedo, &D.Normal, &D.Roughness, &D.Metalness, &D.Specular, &D.Occlusion,
                                                      &D.Emissive, &D.Tint.Image, &D.Tint.Offset, &D.Color.Mask, &D.Transmission.Image, &D.AlphaMap.Image };
                for (auto& L : D.Layers)
                    for (Materials::Map* M : { &L.Albedo, &L.Normal, &L.Roughness, &L.Mask }) Maps.push_back(M);
                for (Materials::Map* M : Maps)
                    if (!M->Empty()) M->Path = (Dir / M->Path).lexically_normal().string();
                // an emission through the engine's plain white image takes its colour and strength
                // from constants the export does not carry: added whole, it washed the surface white
                if (!D.Emissive.Empty() && std::filesystem::path(D.Emissive.Path).stem().string().rfind("$white", 0) == 0)
                    D.Emissive = {};
                Described[D.Name] = std::move(D);
            }
        }

        // The material's description (its maps are read by Load, for every material at once).
        PreviewMaterial Describe(const std::filesystem::path& ModelDir, const std::string& Name, const std::string& Diffuse,
                                 const std::string& Normal, const std::string& Specular)
        {
            PreviewMaterial M;
            const auto Known = Described.find(Name);
            M.Desc = Known != Described.end() ? Known->second
                   : Materials::DefaultAdapter().Describe(SourceOf(ModelDir, Name, Diffuse, Normal, Specular));
            return M;
        }

        // Reads and prepares every map of every material, each file once, on a few threads (a
        // character is 30 materials and 100+ maps); resolves Auto alpha from the colour maps.
        bool Load(const std::function<bool()>& Cancelled)
        {
            struct Job { std::string Path; PreviewTexUse Use; float Cutoff; PreviewImage Img; bool AlphaUsed = false; };
            std::vector<Job> Jobs;
            std::map<std::string, size_t> Index;
            auto Want = [&](const Materials::Map& M, PreviewTexUse Use, float Cutoff) -> int {
                if (M.Empty()) return -1;
                const std::string Key = M.Path + "|" + std::to_string((int)Use) + "|" + std::to_string(Cutoff);
                auto It = Index.find(Key);
                if (It == Index.end()) { It = Index.emplace(Key, Jobs.size()).first; Jobs.push_back({ M.Path, Use, Cutoff }); }
                return (int)It->second;
            };
            struct Slots { int Albedo, Normal, Roughness, Metalness, Specular, Occlusion, Emissive, Tint, TintOffset, Layer[2][3], Mask, ColorMask, Transmission, AlphaMap; };
            std::vector<Slots> Of(List.size());
            for (size_t i = 0; i < List.size(); i++)
            {
                const Materials::Description& D = List[i].Desc;
                const float Cut = D.Alpha == Materials::AlphaMode::Cutout ? D.AlphaCutoff : -1.0f;
                Of[i] = { Want(D.Albedo, PreviewTexUse::Color, Cut), Want(D.Normal, PreviewTexUse::Normal, -1),
                          Want(D.Roughness, PreviewTexUse::Data, -1), Want(D.Metalness, PreviewTexUse::Data, -1),
                          Want(D.Specular, PreviewTexUse::Color, -1), Want(D.Occlusion, PreviewTexUse::Data, -1),
                          Want(D.Emissive, PreviewTexUse::Color, -1), Want(D.Tint.Image, PreviewTexUse::Color, -1),
                          Want(D.Tint.Offset, PreviewTexUse::Data, -1), { { -1, -1, -1 }, { -1, -1, -1 } }, -1,
                          Want(D.Color.Mask, PreviewTexUse::Data, -1), Want(D.Transmission.Image, PreviewTexUse::Data, -1),
                          Want(D.AlphaMap.Image, PreviewTexUse::Data, -1) };
                for (size_t l = 0; l < 2 && l < D.Layers.size(); l++)
                {
                    const Materials::Layer& L = D.Layers[l];
                    Of[i].Layer[l][0] = Want(L.Albedo, PreviewTexUse::Color, -1);
                    Of[i].Layer[l][1] = Want(L.Normal, PreviewTexUse::Normal, -1);
                    Of[i].Layer[l][2] = Want(L.Roughness, PreviewTexUse::Data, -1);
                    if (l == 1) Of[i].Mask = Want(L.Mask, PreviewTexUse::Data, -1);
                }
            }

            std::atomic<size_t> Next{ 0 };
            auto Work = [&] {
                const HRESULT Com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // WIC decodes PNGs
                for (size_t k; (k = Next++) < Jobs.size() && !(Cancelled && Cancelled());)
                {
                    Job& J = Jobs[k];
                    AppLog::Timer T("texture " + J.Path, spdlog::level::debug, 250);
                    if (!PreparePreviewImage(J.Path, J.Use, J.Cutoff, J.Img, &J.AlphaUsed)) T.Detail("(did not decode)");
                }
                if (SUCCEEDED(Com)) CoUninitialize();
            };
            const size_t Threads = std::min<size_t>(std::max(1u, std::thread::hardware_concurrency() / 2), std::min<size_t>(8, Jobs.size()));
            std::vector<std::thread> Pool;
            for (size_t t = 1; t < Threads; t++) Pool.emplace_back(Work);
            Work();
            for (auto& T : Pool) T.join();
            if (Cancelled && Cancelled()) return false;

            auto Img = [&](int Slot) { return Slot < 0 ? PreviewImage{} : Jobs[Slot].Img; };
            for (size_t i = 0; i < List.size(); i++)
            {
                PreviewMaterial& M = List[i];
                const Materials::Description& D = M.Desc;
                M.Albedo = Img(Of[i].Albedo); M.Normal = Img(Of[i].Normal); M.Roughness = Img(Of[i].Roughness);
                M.Metalness = Img(Of[i].Metalness); M.Specular = Img(Of[i].Specular); M.Occlusion = Img(Of[i].Occlusion);
                M.Emissive = Img(Of[i].Emissive);
                M.Tint = Img(Of[i].Tint); M.TintOffset = Img(Of[i].TintOffset); M.LayerMask = Img(Of[i].Mask);
                M.ColorMask = Img(Of[i].ColorMask);
                M.Transmission = Img(Of[i].Transmission); M.AlphaMap = Img(Of[i].AlphaMap);
                for (int l = 0; l < 2; l++)
                {
                    M.LayerAlbedo[l] = Img(Of[i].Layer[l][0]);
                    M.LayerNormal[l] = Img(Of[i].Layer[l][1]);
                    M.LayerRoughness[l] = Img(Of[i].Layer[l][2]);
                }
                M.Alpha = D.Alpha;
                if (D.Alpha == Materials::AlphaMode::Auto)
                {
                    const bool Used = Of[i].Albedo >= 0 && Jobs[Of[i].Albedo].AlphaUsed;
                    M.Alpha = Used ? Materials::AlphaMode::Cutout : Materials::AlphaMode::Opaque;
                    if (Used) PreserveAlphaCoverage(M.Albedo, D.AlphaCutoff);
                }
            }
            return true;
        }

    private:
        std::map<std::string, Materials::Description> Described;
        std::map<std::string, std::vector<std::string>> Dirs;

        const std::vector<std::string>& Listing(const std::filesystem::path& Dir)
        {
            auto It = Dirs.find(Dir.string());
            if (It != Dirs.end()) return It->second;
            std::vector<std::string> Names;
            std::error_code Ec;
            for (const auto& E : std::filesystem::directory_iterator(Dir, Ec))
                if (E.is_regular_file(Ec)) Names.push_back(E.path().filename().string());
            return Dirs.emplace(Dir.string(), std::move(Names)).first->second;
        }

        // The model file's own references, then their siblings: the files named like a referenced
        // one with another suffix (i_x_c.dds -> i_x_g.dds, i_x_o.dds...), for the adapter to sort.
        Materials::Source SourceOf(const std::filesystem::path& ModelDir, const std::string& Name, const std::string& Diffuse,
                                   const std::string& Normal, const std::string& Specular)
        {
            namespace fs = std::filesystem;
            Materials::Source S;
            S.Name = Name;
            std::set<std::string> Seen;
            auto Add = [&](const fs::path& P, Materials::Role Hint) {
                std::error_code Ec;
                if (!fs::is_regular_file(P, Ec)) return;
                const std::string Norm = P.lexically_normal().string();
                if (Seen.insert(ExportNaming::Lower(Norm)).second) S.Images.push_back({ Norm, Hint, 0 });
            };
            if (!Diffuse.empty()) Add(ModelDir / Diffuse, Materials::Role::Albedo);
            if (S.Images.empty())
            {
                const std::wstring Found = ResolveDiffuseTexture(ModelDir, Name, Diffuse);
                if (!Found.empty()) Add(fs::path(Found), Materials::Role::Albedo);
            }
            if (!Normal.empty()) Add(ModelDir / Normal, Materials::Role::Normal);
            if (!Specular.empty()) Add(ModelDir / Specular, Materials::Role::Specular);

            const auto Refs = S.Images;
            for (const auto& Ref : Refs)
            {
                const fs::path P(Ref.Path);
                const std::string Stem = P.stem().string();
                const size_t Cut = Stem.rfind('_');
                if (Cut == std::string::npos || Materials::RoleFromName(Stem) == Materials::Role::Unknown) continue;
                const std::string Base = ExportNaming::Lower(Stem.substr(0, Cut + 1));
                for (const auto& F : Listing(P.parent_path()))
                {
                    const std::string L = ExportNaming::Lower(F);
                    if (L.size() <= Base.size() || L.compare(0, Base.size(), Base) != 0) continue;
                    const std::string Rest = fs::path(L.substr(Base.size())).stem().string();
                    if (Rest.find('_') == std::string::npos) Add(P.parent_path() / F, Materials::Role::Unknown);
                }
            }
            return S;
        }
    };
}

namespace
{
    // Any model file the preview reads, by its extension: .cast, .xmodel_export, else SEModel
    bool ParseModelBytes(const std::string& Ext, const std::vector<uint8_t>& Bytes, Geo& Out)
    {
        if (Ext == ".cast") return ParseCastModel(Bytes, Out);
        if (Ext == ".xmodel_export") return ParseXModelExport(Bytes, Out);
        return ParseSEModel(Bytes, Out);
    }

    // Parses one .semodel and merges its geometry into Combined (offsetting vertex and material
    // indices), appending its diffuse textures to Textures. Lets several parts share one scene.
    // A part's bones joined to the skeleton built so far (LoadRigGeometry): one of the same name is
    // that bone; the others are appended, seated as the part's vertices are, the part's root under
    // the bone it was seated on. Returns each part bone's index in Combined.Bones.
    std::vector<int32_t> JoinSkeleton(Geo& Combined, const Geo& G, bool Xform, const Quat& Rref, const Quat& RpartConj,
                                      const Vec3& Tpart, const Vec3& Tref, int32_t SeatedOn)
    {
        const auto Mul = [](const Quat& a, const Quat& b) {
            return Quat{ a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                         a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
        };
        std::unordered_map<std::string, int32_t> Have;
        for (size_t b = 0; b < Combined.Bones.size(); b++) Have.emplace(Combined.Bones[b].Name, (int32_t)b);
        std::vector<int32_t> Map(G.Bones.size(), -1);
        for (size_t b = 0; b < G.Bones.size(); b++)
        {
            if (const auto It = Have.find(G.Bones[b].Name); It != Have.end()) { Map[b] = It->second; continue; }
            Bone B = G.Bones[b];
            if (Xform)
            {
                const Vec3 v = QRot(Rref, QRot(RpartConj, Sub(B.Pos, Tpart)));
                B.Pos = { v.x + Tref.x, v.y + Tref.y, v.z + Tref.z };
                const Quat Q = Mul(Rref, Mul(RpartConj, Quat{ B.Qx, B.Qy, B.Qz, B.Qw }));
                B.Qx = Q.x; B.Qy = Q.y; B.Qz = Q.z; B.Qw = Q.w;
            }
            // listed parent first: a parent of the part is mapped already
            B.Parent = (B.Parent >= 0 && (size_t)B.Parent < b) ? Map[B.Parent] : SeatedOn;
            Map[b] = (int32_t)Combined.Bones.size();
            Have.emplace(B.Name, Map[b]);
            Combined.Bones.push_back(std::move(B));
        }
        return Map;
    }

    bool LoadPart(const std::string& SemodelPath, Geo& Combined, std::vector<Tex>& Textures,
                  const std::function<bool()>& Cancelled = {}, PreviewMaterials* Maps = nullptr, bool Join = false)
    {
        AppLog::Timer Part("model part " + SemodelPath);
        std::vector<uint8_t> Bytes;
        {
            AppLog::Timer Read("read model file");
            if (!ReadFileBulk(SemodelPath, Bytes)) return false;
            Read.Detail(std::to_string(Bytes.size() >> 10) + " KB");
        }

        std::string Ext = std::filesystem::path(SemodelPath).extension().string();
        for (auto& c : Ext) c = (char)tolower((unsigned char)c);

        Geo G;
        {
            AppLog::Timer Parse("parse model");
            const bool Ok = ParseModelBytes(Ext, Bytes, G);
            Parse.Detail(std::to_string(G.Pos.size()) + " verts, " + std::to_string(G.Idx.size() / 3) + " tris");
            if (!Ok) return false;
        }

        const bool WasFirstPart = Combined.Pos.empty(); // only this part's bones become Combined.Bones (below)
        const uint32_t VtxBase = (uint32_t)Combined.Pos.size();
        const int32_t MatBase = (int32_t)Textures.size();

        // Seat this part into the first part's space with a RIGID transform, not just translation. Each
        // part is exported in its own bind space (a head is rooted at its neck bone at the origin, its
        // geometry running along a local axis). To place it we take a bone it shares with the reference
        // part and apply that bone's reference world transform to the part's local frame:
        //   V' = R_ref * ( conj(R_part) * (V - T_part) ) + T_ref
        // Using the part's ROOT bone (its whole mesh hangs off it).
        bool Xform = false;
        Quat Rref, RpartConj;
        Vec3 Tpart{ 0,0,0 }, Tref{ 0,0,0 };
        int32_t SeatedOn = -1;   // the Combined bone the part's root hangs from (Join)
        if (!Combined.Bones.empty() && !G.Bones.empty())
        {
            const Bone* Root = &G.Bones[0];
            for (const auto& B : G.Bones) if (B.Parent < 0) { Root = &B; break; }
            const Bone* Ref = nullptr;
            for (const auto& Rb : Combined.Bones) if (Rb.Name == Root->Name) { Ref = &Rb; break; }
            // a rig's weapon on its hands: the root (j_gun) on the socket the game attaches it to
            if (!Ref && Join)
                for (const char* Socket : { "tag_weapon", "tag_weapon_right", "tag_weapon_ri", "tag_inhand", "tag_weapon_le", "tag_weapon_left" })
                {
                    for (const auto& Rb : Combined.Bones) if (Rb.Name == Socket) { Ref = &Rb; break; }
                    if (Ref) break;
                }
            if (Ref) SeatedOn = (int32_t)(Ref - Combined.Bones.data());
            if (!Ref) // fallback: any shared bone
                for (const auto& Pb : G.Bones) { for (const auto& Rb : Combined.Bones) if (Rb.Name == Pb.Name) { Ref = &Rb; Root = &Pb; break; } if (Ref) break; }
            if (Ref)
            {
                Rref = { Ref->Qx, Ref->Qy, Ref->Qz, Ref->Qw };
                RpartConj = QConj({ Root->Qx, Root->Qy, Root->Qz, Root->Qw });
                Tpart = Root->Pos; Tref = Ref->Pos;
                Xform = true;
            }
        }
        else
        {
            Combined.Bones = G.Bones; // reference skeleton
        }

        if (Xform)
        {
            for (const auto& P : G.Pos)
            {
                const Vec3 v = QRot(Rref, QRot(RpartConj, Sub(P, Tpart)));
                Combined.Pos.push_back({ v.x + Tref.x, v.y + Tref.y, v.z + Tref.z });
            }
            for (const auto& N : G.Nrm)
                Combined.Nrm.push_back(QRot(Rref, QRot(RpartConj, N)));
        }
        else
        {
            Combined.Pos.insert(Combined.Pos.end(), G.Pos.begin(), G.Pos.end());
            Combined.Nrm.insert(Combined.Nrm.end(), G.Nrm.begin(), G.Nrm.end());
        }
        Combined.UV.insert(Combined.UV.end(), G.UV.begin(), G.UV.end());
        for (uint32_t i : G.Idx) Combined.Idx.push_back(VtxBase + i);
        for (int32_t m : G.TriMat) Combined.TriMat.push_back(m < 0 ? -1 : MatBase + m);

        // Skin weights: G's bone indices only match Combined.Bones for the first part loaded. A later
        // part (seated via Xform) never gets its bones appended, so its weights would reference the wrong
        // skeleton: left unweighted (rigid) instead. Joined (a rig), its bones are the skeleton's.
        if (Join && !WasFirstPart && !G.Bones.empty())
        {
            const std::vector<int32_t> Map = JoinSkeleton(Combined, G, Xform, Rref, RpartConj, Tpart, Tref, SeatedOn);
            if (Combined.VtxBoneIdx.empty() && !G.VtxBoneIdx.empty())
            {
                Combined.VtxBoneIdx.assign((size_t)VtxBase * PREVIEW_MAX_BONE_INFLUENCES, -1);
                Combined.VtxBoneWeight.assign((size_t)VtxBase * PREVIEW_MAX_BONE_INFLUENCES, 0.0f);
            }
            if (!Combined.VtxBoneIdx.empty())
            {
                if (G.VtxBoneIdx.size() == G.Pos.size() * PREVIEW_MAX_BONE_INFLUENCES)
                    for (size_t k = 0; k < G.VtxBoneIdx.size(); k++)
                    {
                        const int32_t B = G.VtxBoneIdx[k];
                        Combined.VtxBoneIdx.push_back(B >= 0 && (size_t)B < Map.size() ? Map[B] : -1);
                        Combined.VtxBoneWeight.push_back(G.VtxBoneWeight[k]);
                    }
                else
                {
                    Combined.VtxBoneIdx.resize(Combined.VtxBoneIdx.size() + G.Pos.size() * PREVIEW_MAX_BONE_INFLUENCES, -1);
                    Combined.VtxBoneWeight.resize(Combined.VtxBoneWeight.size() + G.Pos.size() * PREVIEW_MAX_BONE_INFLUENCES, 0.0f);
                }
            }
        }
        else if (WasFirstPart && !G.VtxBoneIdx.empty())
        {
            Combined.VtxBoneIdx.insert(Combined.VtxBoneIdx.end(), G.VtxBoneIdx.begin(), G.VtxBoneIdx.end());
            Combined.VtxBoneWeight.insert(Combined.VtxBoneWeight.end(), G.VtxBoneWeight.begin(), G.VtxBoneWeight.end());
        }
        else if (!Combined.VtxBoneIdx.empty())
        {
            // A prior part had weights but this one can't contribute any - pad so indices still line
            // up 1:1 with Combined.Pos (unweighted = rigid, not "no data" for the whole model).
            Combined.VtxBoneIdx.resize(Combined.VtxBoneIdx.size() + G.Pos.size() * PREVIEW_MAX_BONE_INFLUENCES, -1);
            Combined.VtxBoneWeight.resize(Combined.VtxBoneWeight.size() + G.Pos.size() * PREVIEW_MAX_BONE_INFLUENCES, 0.0f);
        }

        // Resolve + load each material's diffuse texture, appended so indices line up with MatBase.
        const std::filesystem::path ModelDir = std::filesystem::path(SemodelPath).parent_path();
        if (Maps) Maps->ReadSidecar(SemodelPath);
        size_t Found = 0, Loaded = 0;
        for (size_t m = 0; m < G.MatName.size(); m++)
        {
            if (Cancelled && Cancelled()) return false;
            const std::string& Name = G.MatName[m];
            const std::string& Diffuse = m < G.MatDiffuse.size() ? G.MatDiffuse[m] : std::string();
            Tex T;
            if (Maps)
            {
                const auto At = [](const std::vector<std::string>& V, size_t i) { return i < V.size() ? V[i] : std::string(); };
                Maps->List.push_back(Maps->Describe(ModelDir, Name, Diffuse, At(G.MatNormal, m), At(G.MatSpecular, m)));
                if (!Maps->List.back().Desc.Albedo.Empty()) { Found++; Loaded++; }
                Textures.push_back(std::move(T));
                continue;
            }
            std::wstring D = ResolveDiffuseTexture(ModelDir, Name, Diffuse);
            if (!D.empty())
            {
                Found++;
                AppLog::Timer Load("texture " + Strings::ToNormalString(D), spdlog::level::debug, 250);
                T = LoadTexture(D);
                if (T.Ok()) Loaded++;
                else Load.Detail("(did not decode)");
            }
            else
            {
                AppLog::Log().debug("model '{}': material '{}' texture not found (wanted '{}')", SemodelPath, Name, Diffuse);
            }
            Textures.push_back(std::move(T));
        }
        Part.Detail(": " + std::to_string(G.MatName.size()) + " materials, " + std::to_string(Found) + " textures found, " +
                    std::to_string(Loaded) + " loaded");
        return true;
    }

    // Rasterizes the framed model into an RGBA8 buffer (transparent background), viewed from an
    // azimuth of Yaw radians around the up axis. Shared by the single render and the turntable.
    // View: 0 the 3/4 view framed on the bulk of the geometry, 1 straight down on its whole
    // bounds (a map's layout), 2 the 3/4 view closer in on its middle (a map's detail).
    bool RasterizeToRGBA(const Geo& G, const std::vector<Tex>& Textures, int Size, float Yaw,
                         std::vector<uint8_t>& OutPx, int View = 0)
    {
        if (Size < 8) Size = 8;
        if (G.Pos.empty() || G.Idx.empty()) return false;

        // Bounding sphere to frame the model: mean position for the center and a 90th-percentile radius,
        // both robust to a single outlier mesh (a map .glb's skybox).
        Vec3 Sum{};
        for (const auto& P : G.Pos) { Sum.x += P.x; Sum.y += P.y; Sum.z += P.z; }
        const float N = (float)G.Pos.size();
        const Vec3 Ctr{ Sum.x / N, Sum.y / N, Sum.z / N };
        std::vector<float> Dists(G.Pos.size());
        for (size_t i = 0; i < G.Pos.size(); i++) Dists[i] = std::sqrt(Dot(Sub(G.Pos[i], Ctr), Sub(G.Pos[i], Ctr)));
        float Radius = *std::max_element(Dists.begin(), Dists.end());
        if (Dists.size() >= 20)
        {
            const size_t P90 = (size_t)(Dists.size() * 0.90);
            std::nth_element(Dists.begin(), Dists.begin() + P90, Dists.end());
            Radius = Dists[P90];
        }
        if (Radius < 1e-6f) Radius = 1.0f;
        Vec3 Ctr2 = Ctr;
        if (View == 1)
        {
            Vec3 Mn = G.Pos[0], Mx = G.Pos[0];
            for (const auto& P : G.Pos)
            {
                Mn = { std::min(Mn.x, P.x), std::min(Mn.y, P.y), std::min(Mn.z, P.z) };
                Mx = { std::max(Mx.x, P.x), std::max(Mx.y, P.y), std::max(Mx.z, P.z) };
            }
            Ctr2 = { (Mn.x + Mx.x) * 0.5f, (Mn.y + Mx.y) * 0.5f, (Mn.z + Mx.z) * 0.5f };
            Radius = std::max(Mx.x - Mn.x, Mx.y - Mn.y) * 0.5f + 1e-3f;
        }
        else if (View == 2)
            Radius *= 0.35f;

        // Camera: 3/4 view, CoD models Z-up. Yaw rotates the azimuth around Up (for turntables).
        const Vec3 Up = View == 1 ? Vec3{ 0, 1, 0 } : Vec3{ 0, 0, 1 };
        const Vec3 D0 = View == 1 ? Vec3{ 0, 0, -1 } : Vec3{ -0.55f, -0.8f, -0.32f };
        const float ca = std::cos(Yaw), sa = std::sin(Yaw);
        const Vec3 Dir = Norm({ D0.x * ca - D0.y * sa, D0.x * sa + D0.y * ca, D0.z });
        const Vec3 Right = Norm(Cross(Dir, Up));
        const Vec3 CamUp = Cross(Right, Dir);
        const Vec3 Light = Norm({ -0.4f, -0.5f, 0.75f });

        const float Margin = 0.92f;
        const float Scale = (Size * 0.5f * Margin) / Radius;
        const float Half = Size * 0.5f;

        struct SV { float sx, sy, depth; };
        std::vector<SV> V(G.Pos.size());
        for (size_t i = 0; i < G.Pos.size(); i++)
        {
            const Vec3 d = Sub(G.Pos[i], Ctr2);
            V[i].sx = Half + Dot(d, Right) * Scale;
            V[i].sy = Half - Dot(d, CamUp) * Scale;
            V[i].depth = Dot(d, Dir);
        }

        const int W = Size, H = Size;
        std::vector<float> ZBuf((size_t)W * H, 1e30f);
        OutPx.assign((size_t)W * H * 4, 0); // RGBA, transparent background

        auto Edge = [](float ax, float ay, float bx, float by, float cx, float cy) {
            return (cx - ax) * (by - ay) - (cy - ay) * (bx - ax);
        };

        const size_t TriCount = G.Idx.size() / 3;
        for (size_t t = 0; t < TriCount; t++)
        {
            const uint32_t iA = G.Idx[t * 3], iB = G.Idx[t * 3 + 1], iC = G.Idx[t * 3 + 2];
            const SV& A = V[iA]; const SV& B = V[iB]; const SV& C = V[iC];

            const int32_t Mat = t < G.TriMat.size() ? G.TriMat[t] : -1;
            const Tex* Texture = (Mat >= 0 && Mat < (int)Textures.size() && Textures[Mat].Ok()) ? &Textures[Mat] : nullptr;

            Vec3 fn = Norm(Cross(Sub(G.Pos[iB], G.Pos[iA]), Sub(G.Pos[iC], G.Pos[iA])));

            int minX = (int)std::floor(std::min({ A.sx, B.sx, C.sx }));
            int maxX = (int)std::ceil (std::max({ A.sx, B.sx, C.sx }));
            int minY = (int)std::floor(std::min({ A.sy, B.sy, C.sy }));
            int maxY = (int)std::ceil (std::max({ A.sy, B.sy, C.sy }));
            minX = std::max(0, minX); minY = std::max(0, minY);
            maxX = std::min(W - 1, maxX); maxY = std::min(H - 1, maxY);
            if (minX > maxX || minY > maxY) continue;

            const float area = Edge(A.sx, A.sy, B.sx, B.sy, C.sx, C.sy);
            if (std::fabs(area) < 1e-6f) continue;
            const float inv = 1.0f / area;

            for (int y = minY; y <= maxY; y++)
            {
                const float py = y + 0.5f;
                for (int x = minX; x <= maxX; x++)
                {
                    const float px = x + 0.5f;
                    float w0 = Edge(B.sx, B.sy, C.sx, C.sy, px, py) * inv;
                    float w1 = Edge(C.sx, C.sy, A.sx, A.sy, px, py) * inv;
                    float w2 = Edge(A.sx, A.sy, B.sx, B.sy, px, py) * inv;
                    if ((w0 < 0 || w1 < 0 || w2 < 0) && (w0 > 0 || w1 > 0 || w2 > 0)) continue;

                    const float depth = w0 * A.depth + w1 * B.depth + w2 * C.depth;
                    const size_t idx = (size_t)y * W + x;
                    if (depth >= ZBuf[idx]) continue;
                    ZBuf[idx] = depth;

                    // Smooth (per-pixel) normal: barycentric blend of vertex normals, fall back to face normal.
                    Vec3 N{ w0 * G.Nrm[iA].x + w1 * G.Nrm[iB].x + w2 * G.Nrm[iC].x,
                            w0 * G.Nrm[iA].y + w1 * G.Nrm[iB].y + w2 * G.Nrm[iC].y,
                            w0 * G.Nrm[iA].z + w1 * G.Nrm[iB].z + w2 * G.Nrm[iC].z };
                    N = (Dot(N, N) > 1e-8f) ? Norm(N) : fn;

                    float shade = 0.32f + 0.68f * std::fabs(Dot(N, Light)); // two-sided ambient+diffuse

                    float br = 205, bg = 208, bb = 214; // default clay grey
                    if (Texture)
                    {
                        float u = w0 * G.UV[iA].u + w1 * G.UV[iB].u + w2 * G.UV[iC].u;
                        float v = w0 * G.UV[iA].v + w1 * G.UV[iB].v + w2 * G.UV[iC].v;
                        Texture->Sample(u, v, br, bg, bb);
                    }

                    OutPx[idx * 4 + 0] = (uint8_t)std::min(255.0f, br * shade);
                    OutPx[idx * 4 + 1] = (uint8_t)std::min(255.0f, bg * shade);
                    OutPx[idx * 4 + 2] = (uint8_t)std::min(255.0f, bb * shade);
                    OutPx[idx * 4 + 3] = 255;
                }
            }
        }
        return true;
    }

    // Wraps an RGBA8 buffer and writes it as a PNG via WIC.
    bool SavePng(const std::vector<uint8_t>& Px, int W, int H, const std::string& OutPngPath)
    {
        DirectX::Image Img{};
        Img.width = (size_t)W; Img.height = (size_t)H;
        Img.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        Img.rowPitch = (size_t)W * 4; Img.slicePitch = (size_t)W * H * 4;
        Img.pixels = const_cast<uint8_t*>(Px.data());
        return SavePreviewPng(Img, OutPngPath);
    }

    bool RasterizeAndSave(const Geo& G, const std::vector<Tex>& Textures, const std::string& OutPngPath, int Size)
    {
        if (Size < 8) Size = 8;
        std::vector<uint8_t> Px;
        if (!RasterizeToRGBA(G, Textures, Size, 0.0f, Px)) return false;
        return SavePng(Px, Size, Size, OutPngPath);
    }
} // namespace

bool SavePreviewPng(const DirectX::Image& Img, const std::string& OutPngPath)
{
    std::error_code Ec;
    std::filesystem::create_directories(std::filesystem::path(OutPngPath).parent_path(), Ec);
    // the pixels are sRGB encoded: tagged so (an untagged UNORM image is written as gAMA 1.0, which
    // colour managed viewers such as Photos show washed out)
    return SUCCEEDED(DirectX::SaveToWICFile(Img, DirectX::WIC_FLAGS_FORCE_SRGB,
                                            DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG),
                                            Strings::ToUnicodeString(OutPngPath).c_str()));
}

bool RenderModelToPNG(const std::string& SemodelPath, const std::string& OutPngPath, int Size)
{
    Geo G;
    std::vector<Tex> Textures;
    if (!LoadPart(SemodelPath, G, Textures)) return false;
    return RasterizeAndSave(G, Textures, OutPngPath, Size);
}

bool RenderModelsToPNG(const std::vector<std::string>& SemodelPaths, const std::string& OutPngPath, int Size)
{
    Geo G;
    std::vector<Tex> Textures;
    bool Any = false;
    for (const auto& P : SemodelPaths) Any |= LoadPart(P, G, Textures);
    if (!Any) return false;
    return RasterizeAndSave(G, Textures, OutPngPath, Size);
}

bool RenderModelTurntable(const std::vector<std::string>& SemodelPaths, const std::string& OutPngPath,
                          int Size, int Frames)
{
    if (Size < 8) Size = 8;
    if (Frames < 1) Frames = 1;

    Geo G;
    std::vector<Tex> Textures;
    bool Any = false;
    for (const auto& P : SemodelPaths) Any |= LoadPart(P, G, Textures);
    if (!Any) return false;

    // Tile the frames into a near-square contact sheet.
    const int Cols = std::max(1, (int)std::ceil(std::sqrt((double)Frames)));
    const int Rows = (Frames + Cols - 1) / Cols;
    const int Pad = 6;
    const int SheetW = Cols * Size + (Cols + 1) * Pad;
    const int SheetH = Rows * Size + (Rows + 1) * Pad;
    std::vector<uint8_t> Sheet((size_t)SheetW * SheetH * 4, 0);

    const float TwoPi = 6.28318530717958648f;
    bool AnyFrame = false;
    for (int f = 0; f < Frames; f++)
    {
        std::vector<uint8_t> Frame;
        if (!RasterizeToRGBA(G, Textures, Size, TwoPi * f / Frames, Frame)) continue;
        AnyFrame = true;

        const int Col = f % Cols, Row = f / Cols;
        const int Ox = Pad + Col * (Size + Pad), Oy = Pad + Row * (Size + Pad);
        for (int y = 0; y < Size; y++)
            std::memcpy(&Sheet[((size_t)(Oy + y) * SheetW + Ox) * 4], &Frame[(size_t)y * Size * 4], (size_t)Size * 4);
    }
    if (!AnyFrame) return false;

    return SavePng(Sheet, SheetW, SheetH, OutPngPath);
}

std::string DumpModelInfo(const std::string& SemodelPath)
{
    std::vector<uint8_t> Bytes;
    if (!ReadFileBulk(SemodelPath, Bytes)) return {};
    std::string Ext = std::filesystem::path(SemodelPath).extension().string();
    for (auto& c : Ext) c = (char)tolower((unsigned char)c);
    Geo G;
    const bool Ok = ParseModelBytes(Ext, Bytes, G);
    if (!Ok) return {};

    char Buf[512];
    std::string S;
    Vec3 Mn = G.Pos.empty() ? Vec3{} : G.Pos[0], Mx = Mn;
    for (const auto& P : G.Pos)
    {
        Mn.x = std::min(Mn.x, P.x); Mn.y = std::min(Mn.y, P.y); Mn.z = std::min(Mn.z, P.z);
        Mx.x = std::max(Mx.x, P.x); Mx.y = std::max(Mx.y, P.y); Mx.z = std::max(Mx.z, P.z);
    }
    std::snprintf(Buf, sizeof(Buf), "verts=%zu tris=%zu bones=%zu\nbbox min=(%.1f,%.1f,%.1f) max=(%.1f,%.1f,%.1f)\n",
        G.Pos.size(), G.Idx.size() / 3, G.Bones.size(), Mn.x, Mn.y, Mn.z, Mx.x, Mx.y, Mx.z);
    S += Buf;
    // Bones relevant to seating (root + head/neck/spine/tag), plus a count cap.
    int Shown = 0;
    for (const auto& B : G.Bones)
    {
        const bool Relevant = B.Parent < 0 || B.Name.find("head") != std::string::npos ||
            B.Name.find("neck") != std::string::npos || B.Name.find("spine") != std::string::npos ||
            B.Name.find("tag") != std::string::npos;
        if (!Relevant || Shown >= 20) continue;
        std::snprintf(Buf, sizeof(Buf), "  [%s] parent=%d pos=(%.2f,%.2f,%.2f) quat=(%.3f,%.3f,%.3f,%.3f)\n",
            B.Name.c_str(), B.Parent, B.Pos.x, B.Pos.y, B.Pos.z, B.Qx, B.Qy, B.Qz, B.Qw);
        S += Buf; Shown++;
    }
    return S;
}

namespace
{
    bool LoadGeometry(const std::vector<std::string>& SemodelPaths, PreviewGeometry& Out, const std::function<bool()>& Cancelled, bool Join)
{
    Geo G;
    std::vector<Tex> Textures;
    PreviewMaterials Maps;
    bool Any = false;
    for (const auto& P : SemodelPaths) Any |= LoadPart(P, G, Textures, Cancelled, &Maps, Join);
    if ((Cancelled && Cancelled()) || !Any || G.Pos.empty() || G.Idx.empty()) return false;

    Out.Pos.reserve(G.Pos.size() * 3);
    Out.Nrm.reserve(G.Pos.size() * 3);
    Out.UV.reserve(G.Pos.size() * 2);
    for (size_t i = 0; i < G.Pos.size(); i++)
    {
        Out.Pos.push_back(G.Pos[i].x); Out.Pos.push_back(G.Pos[i].y); Out.Pos.push_back(G.Pos[i].z);
        Out.Nrm.push_back(G.Nrm[i].x); Out.Nrm.push_back(G.Nrm[i].y); Out.Nrm.push_back(G.Nrm[i].z);
        Out.UV.push_back(G.UV[i].u);   Out.UV.push_back(G.UV[i].v);
    }

    // Per-vertex material, derived from each triangle's material (a vertex belongs to one mesh).
    Out.VtxMat.assign(G.Pos.size(), -1);
    const size_t TriCount = G.Idx.size() / 3;
    for (size_t t = 0; t < TriCount; t++)
    {
        const int32_t Mat = t < G.TriMat.size() ? G.TriMat[t] : -1;
        Out.VtxMat[G.Idx[t * 3]] = Mat;
        Out.VtxMat[G.Idx[t * 3 + 1]] = Mat;
        Out.VtxMat[G.Idx[t * 3 + 2]] = Mat;
    }

    Out.Idx = G.Idx;

    Out.Bones.resize(G.Bones.size());
    for (size_t i = 0; i < G.Bones.size(); i++)
    {
        PreviewBone& B = Out.Bones[i];
        B.Name = G.Bones[i].Name; B.Parent = G.Bones[i].Parent;
        B.Pos[0] = G.Bones[i].Pos.x; B.Pos[1] = G.Bones[i].Pos.y; B.Pos[2] = G.Bones[i].Pos.z;
        B.Rot[0] = G.Bones[i].Qx; B.Rot[1] = G.Bones[i].Qy; B.Rot[2] = G.Bones[i].Qz; B.Rot[3] = G.Bones[i].Qw;
    }
    Out.VtxBoneIdx = G.VtxBoneIdx;
    Out.VtxBoneWeight = G.VtxBoneWeight;

    Out.Materials.resize(Textures.size());
    for (size_t i = 0; i < Textures.size(); i++)
    {
        Out.Materials[i].Width = Textures[i].W;
        Out.Materials[i].Height = Textures[i].H;
        Out.Materials[i].Rgba = std::move(Textures[i].Rgba);
    }
    {
        AppLog::Timer T("prepare textures");
        if (!Maps.Load(Cancelled)) return false;
    }
    Out.MaterialMaps = std::move(Maps.List);
    return true;
}
}

bool LoadPreviewGeometry(const std::vector<std::string>& SemodelPaths, PreviewGeometry& Out,
                         const std::function<bool()>& Cancelled)
{
    return LoadGeometry(SemodelPaths, Out, Cancelled, false);
}

bool LoadRigGeometry(const std::vector<std::string>& Paths, PreviewGeometry& Out, const std::function<bool()>& Cancelled)
{
    // a placeholder's triangle or two (MW2's angel_flare_rig) shows nothing an anim could move: no rig
    return LoadGeometry(Paths, Out, Cancelled, true) && Out.Idx.size() / 3 >= 4;
}

namespace
{
    // An image embedded in a .glb (another writer's PNG/JPEG; ours never embeds) is decoded by
    // stb_image, then kept at 512 texels at most (box filter), which is all a preview needs.
    bool DecodeGlbImage(tinygltf::Image* Image, const int Index, std::string* Err, std::string* Warn, int ReqW, int ReqH,
                      const unsigned char* Bytes, int Size, void*)
    {
        if (!tinygltf::LoadImageData(Image, Index, Err, Warn, ReqW, ReqH, Bytes, Size, nullptr)) return true;   // no texture, not a failed load
        const int Comp = Image->component, Bits = Image->bits;
        while (Bits == 8 && Comp > 0 && (Image->width > 512 || Image->height > 512) && Image->width > 1 && Image->height > 1)
        {
            const int W = Image->width / 2, H = Image->height / 2;
            std::vector<unsigned char> Half((size_t)W * H * Comp);
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++)
                    for (int c = 0; c < Comp; c++)
                    {
                        const auto At = [&](int X, int Y) { return (int)Image->image[((size_t)Y * Image->width + X) * Comp + c]; };
                        Half[((size_t)y * W + x) * Comp + c] = (unsigned char)((At(2 * x, 2 * y) + At(2 * x + 1, 2 * y) + At(2 * x, 2 * y + 1) + At(2 * x + 1, 2 * y + 1)) / 4);
                    }
            Image->image = std::move(Half);
            Image->width = W;
            Image->height = H;
        }
        return true;
    }

    // A glTF image's file: the smaller copy the map export wrote under textures/ beside the .glb
    // (the map_textures setting) when there is one, else the file its URI names. Empty: neither.
    std::string GlbImageFile(const std::filesystem::path& Dir, const std::string& Uri)
    {
        std::string Decoded;
        if (!tinygltf::URIDecode(Uri, &Decoded, nullptr)) Decoded = Uri;
        const std::filesystem::path Rel(Strings::ToUnicodeString(Decoded));
        if (Rel.empty()) return {};
        std::error_code Ec;
        const std::filesystem::path Copy = Dir / L"textures" / Rel.filename();
        if (std::filesystem::is_regular_file(Copy, Ec)) return Strings::ToNormalString(Copy.wstring());
        const std::filesystem::path File = (Dir / Rel).lexically_normal();
        if (std::filesystem::is_regular_file(File, Ec)) return Strings::ToNormalString(File.wstring());
        return {};
    }

    // Runs Fn(i) for i in [0, Count) on Threads threads (the caller's one of them).
    void ParallelFor(size_t Count, size_t Threads, const std::function<void(size_t)>& Fn, const std::function<bool()>& Stop)
    {
        std::atomic<size_t> Next{ 0 };
        auto Work = [&] { for (size_t k; (k = Next++) < Count && !(Stop && Stop());) Fn(k); };
        Threads = std::max<size_t>(1, std::min(Threads, Count));
        std::vector<std::thread> Pool;
        for (size_t t = 1; t < Threads; t++) Pool.emplace_back(Work);
        Work();
        for (auto& T : Pool) T.join();
    }
}

bool LoadPreviewGeometryGlb(const std::string& GlbPath, PreviewGeometry& Out, const GlbPreviewOptions& O, GlbPreviewStats* Stats)
{
    using Clock = std::chrono::steady_clock;
    const auto Since = [](Clock::time_point T) { return std::chrono::duration<double, std::milli>(Clock::now() - T).count(); };
    GlbPreviewStats Own;
    GlbPreviewStats& St = Stats ? *Stats : Own;
    St = {};
    const auto Cancelled = [&] { return O.Cancelled && O.Cancelled(); };

    auto T0 = Clock::now();
    std::vector<uint8_t> Bytes;
    if (!ReadFileBulk(GlbPath, Bytes) || Bytes.empty() || Bytes.size() > 0xFFFFFFFFull) return false;
    St.ReadMs = Since(T0);
    if (Cancelled()) return false;

    // tinygltf reads no image file: the maps are DDS (MSFT_texture_dds), which it cannot decode, and
    // read below, each once, through DirectXTex - only the levels the preview uses.
    T0 = Clock::now();
    tinygltf::TinyGLTF Loader;
    tinygltf::URICallbacks Uris{ nullptr, [](const std::string& In, std::string* Decoded, void*) {
        const std::string Ext = ExportNaming::Lower(std::filesystem::path(In).extension().string());
        return Ext == ".bin" && tinygltf::URIDecode(In, Decoded, nullptr);
    }, nullptr };
    Loader.SetURICallbacks(Uris);
    Loader.SetImageLoader(&DecodeGlbImage, nullptr);
    tinygltf::Model Gltf;
    std::string Err, Warn;
    const std::filesystem::path Dir = std::filesystem::path(GlbPath).parent_path();
    if (!Loader.LoadBinaryFromMemory(&Gltf, &Err, &Warn, Bytes.data(), (unsigned int)Bytes.size(), Dir.string())) return false;
    std::vector<uint8_t>().swap(Bytes);
    St.ParseMs = Since(T0);
    if (Cancelled()) return false;

    // ---- materials: one preview material per (base colour image, alpha mode)
    const auto ImageOf = [&](int TexIdx) -> int {
        if (TexIdx < 0 || TexIdx >= (int)Gltf.textures.size()) return -1;
        const tinygltf::Texture& T = Gltf.textures[(size_t)TexIdx];
        int Src = T.source;
        if (Src < 0)
            if (const auto It = T.extensions.find("MSFT_texture_dds"); It != T.extensions.end() && It->second.Has("source"))
                Src = It->second.Get("source").GetNumberAsInt();
        return Src >= 0 && Src < (int)Gltf.images.size() ? Src : -1;
    };
    struct Slot { int Image; Materials::AlphaMode Alpha; float Cutoff; };
    std::vector<Slot> Slots;
    std::map<std::pair<int, int>, int> SlotOf;
    std::vector<int32_t> MatSlot(Gltf.materials.size(), -1);
    if (O.MaxTexture > 0)
        for (size_t m = 0; m < Gltf.materials.size(); m++)
        {
            const tinygltf::Material& Mat = Gltf.materials[m];
            const int Img = ImageOf(Mat.pbrMetallicRoughness.baseColorTexture.index);
            if (Img < 0) continue;
            const Materials::AlphaMode Alpha = Mat.alphaMode == "MASK" ? Materials::AlphaMode::Cutout
                                             : Mat.alphaMode == "BLEND" ? Materials::AlphaMode::Blend : Materials::AlphaMode::Opaque;
            const auto [It, New] = SlotOf.emplace(std::make_pair(Img, (int)Alpha), (int)Slots.size());
            if (New) Slots.push_back({ Img, Alpha, (float)Mat.alphaCutoff });
            MatSlot[m] = It->second;
        }

    // ---- geometry
    T0 = Clock::now();
    // An accessor's elements in the .glb's buffer (null when out of bounds)
    const auto View = [&](int AccessorIdx, int& Stride, size_t& Count) -> const uint8_t* {
        if (AccessorIdx < 0 || AccessorIdx >= (int)Gltf.accessors.size()) return nullptr;
        const auto& Acc = Gltf.accessors[AccessorIdx];
        if (Acc.bufferView < 0 || Acc.bufferView >= (int)Gltf.bufferViews.size()) return nullptr;
        const auto& Bv = Gltf.bufferViews[Acc.bufferView];
        if (Bv.buffer < 0 || Bv.buffer >= (int)Gltf.buffers.size()) return nullptr;
        const auto& Buf = Gltf.buffers[Bv.buffer];
        Stride = Acc.ByteStride(Bv);
        Count = Acc.count;
        if (Stride <= 0 || Bv.byteOffset + Acc.byteOffset + (size_t)Stride * Acc.count > Buf.data.size()) return nullptr;
        return Buf.data.data() + Bv.byteOffset + Acc.byteOffset;
    };
    // Floats read in place when packed (our writer's), else copied into Own
    const auto Floats = [&](int AccessorIdx, int Components, size_t Want, std::vector<float>& Own) -> const float* {
        int Stride = 0;
        size_t Count = 0;
        const uint8_t* Base = View(AccessorIdx, Stride, Count);
        if (!Base || Gltf.accessors[(size_t)AccessorIdx].componentType != TINYGLTF_COMPONENT_TYPE_FLOAT || (Want && Count != Want)) return nullptr;
        if (Stride == (int)sizeof(float) * Components) return reinterpret_cast<const float*>(Base);
        Own.resize(Count * (size_t)Components);
        for (size_t v = 0; v < Count; v++) std::memcpy(Own.data() + v * Components, Base + v * Stride, sizeof(float) * (size_t)Components);
        return Own.data();
    };

    // A map's skybox mesh often spans the whole scene: even the outlier-robust framing does not escape
    // it once the camera ends up inside that dome, so anything sky-named is skipped.
    struct Prim
    {
        const float *Pos = nullptr, *Nrm = nullptr, *UV = nullptr;   // in the .glb's buffer, or in the Own copies
        const uint32_t* Idx = nullptr;   // whole triangles, every index within the vertices
        size_t VtxCount = 0, IdxCount = 0;
        std::vector<float> OwnPos, OwnNrm, OwnUV;
        std::vector<uint32_t> OwnIdx;
        int32_t Mat = -1;
        float Lo[3] = { 0, 0, 0 }, Hi[3] = { 0, 0, 0 };
    };
    std::map<int, std::vector<Prim>> Meshes;   // read once, placed by every node that uses it
    const auto MeshPrims = [&](int MeshIdx) -> const std::vector<Prim>& {
        if (const auto It = Meshes.find(MeshIdx); It != Meshes.end()) return It->second;
        std::vector<Prim> List;
        const auto& MeshEntry = Gltf.meshes[(size_t)MeshIdx];
        if (ExportNaming::Lower(MeshEntry.name).find("sky") == std::string::npos)
            for (const auto& P : MeshEntry.primitives)
            {
                if (P.mode != TINYGLTF_MODE_TRIANGLES && P.mode != -1) continue;
                const auto PosIt = P.attributes.find("POSITION");
                if (PosIt == P.attributes.end()) continue;
                List.emplace_back();
                Prim& Pr = List.back();
                Pr.Pos = Floats(PosIt->second, 3, 0, Pr.OwnPos);
                Pr.VtxCount = Pr.Pos ? Gltf.accessors[(size_t)PosIt->second].count : 0;
                if (!Pr.Pos || !Pr.VtxCount) { List.pop_back(); continue; }
                if (const auto It = P.attributes.find("NORMAL"); It != P.attributes.end()) Pr.Nrm = Floats(It->second, 3, Pr.VtxCount, Pr.OwnNrm);
                if (const auto It = P.attributes.find("TEXCOORD_0"); It != P.attributes.end()) Pr.UV = Floats(It->second, 2, Pr.VtxCount, Pr.OwnUV);
                Pr.Mat = (P.material >= 0 && P.material < (int)MatSlot.size()) ? MatSlot[(size_t)P.material] : -1;

                // indices: in place when 32-bit and every one is within the vertices
                int Stride = 0;
                size_t Count = 0;
                const uint8_t* Base = P.indices >= 0 ? View(P.indices, Stride, Count) : nullptr;
                const int Type = Base ? Gltf.accessors[(size_t)P.indices].componentType : 0;
                const uint32_t Max = (uint32_t)Pr.VtxCount;
                bool Valid = false;
                if (Type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT && Stride == 4)
                {
                    const uint32_t* I = reinterpret_cast<const uint32_t*>(Base);
                    const uint32_t* const End = I + Count / 3 * 3;
                    Valid = true;
                    for (const uint32_t* At = I; At != End; ++At) if (*At >= Max) { Valid = false; break; }
                    if (Valid) { Pr.Idx = I; Pr.IdxCount = Count / 3 * 3; }
                }
                if (!Valid)
                {
                    std::vector<uint32_t> All;
                    if (Base && (Type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT || Type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT ||
                                 Type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE))
                    {
                        All.resize(Count);
                        for (size_t i = 0; i < Count; i++)
                        {
                            const uint8_t* Src = Base + i * Stride;
                            uint32_t V = 0;
                            if (Type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) V = Src[0];
                            else if (Type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) { uint16_t V16 = 0; std::memcpy(&V16, Src, 2); V = V16; }
                            else std::memcpy(&V, Src, 4);
                            All[i] = V;
                        }
                    }
                    else if (!Base)
                    {
                        All.resize(Pr.VtxCount);   // unindexed triangle list
                        for (uint32_t v = 0; v < Max; v++) All[v] = v;
                    }
                    Pr.OwnIdx.reserve(All.size());
                    for (size_t t = 0; t + 2 < All.size(); t += 3)
                        if (All[t] < Max && All[t + 1] < Max && All[t + 2] < Max)
                            Pr.OwnIdx.insert(Pr.OwnIdx.end(), { All[t], All[t + 1], All[t + 2] });
                    Pr.Idx = Pr.OwnIdx.data();
                    Pr.IdxCount = Pr.OwnIdx.size();
                }
                if (!Pr.IdxCount) { List.pop_back(); continue; }

                // its box: the accessor's (glTF asks for it), else read
                const auto& PosAcc = Gltf.accessors[(size_t)PosIt->second];
                if (PosAcc.minValues.size() == 3 && PosAcc.maxValues.size() == 3)
                    for (int c = 0; c < 3; c++) { Pr.Lo[c] = (float)PosAcc.minValues[c]; Pr.Hi[c] = (float)PosAcc.maxValues[c]; }
                else
                {
                    for (int c = 0; c < 3; c++) { Pr.Lo[c] = FLT_MAX; Pr.Hi[c] = -FLT_MAX; }
                    const float* V = Pr.Pos;
                    for (size_t v = 0; v < Pr.VtxCount; v++, V += 3)
                        for (int c = 0; c < 3; c++) { if (V[c] < Pr.Lo[c]) Pr.Lo[c] = V[c]; if (V[c] > Pr.Hi[c]) Pr.Hi[c] = V[c]; }
                }
            }
        return Meshes.emplace(MeshIdx, std::move(List)).first->second;
    };

    // Column-major 4x4 matrices. The map writer's scene root ("cod_to_gltf") only converts CoD's
    // Z-up inches to glTF: it is left out, so the preview stays in the Z-up space it frames.
    using Mat4 = std::array<double, 16>;
    const auto Mul = [](const Mat4& A, const Mat4& B) {
        Mat4 R{};
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                for (int k = 0; k < 4; k++) R[c * 4 + r] += A[k * 4 + r] * B[c * 4 + k];
        return R;
    };
    const Mat4 Ident = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    const auto Local = [&](const tinygltf::Node& N) -> Mat4 {
        if (N.name == "cod_to_gltf") return Ident;
        if (N.matrix.size() == 16)
        {
            Mat4 M;
            for (int i = 0; i < 16; i++) M[i] = N.matrix[i];
            return M;
        }
        Mat4 T = Ident, Rm = Ident, S = Ident;
        if (N.translation.size() == 3) { T[12] = N.translation[0]; T[13] = N.translation[1]; T[14] = N.translation[2]; }
        if (N.scale.size() == 3) { S[0] = N.scale[0]; S[5] = N.scale[1]; S[10] = N.scale[2]; }
        if (N.rotation.size() == 4)
        {
            const double x = N.rotation[0], y = N.rotation[1], z = N.rotation[2], w = N.rotation[3];
            Rm = { 1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w), 0,
                   2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w), 0,
                   2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y), 0, 0, 0, 0, 1 };
        }
        return Mul(T, Mul(Rm, S));
    };

    // Every placed piece, its size in the scene (the diagonal of its placed box)
    struct Piece { const Prim* Pr; Mat4 World; double Size; size_t Vtx = 0, Idx = 0; };
    std::vector<Piece> Pieces;
    std::vector<std::pair<int, Mat4>> Stack;
    const int SceneIdx = Gltf.defaultScene >= 0 ? Gltf.defaultScene : 0;
    if (SceneIdx < (int)Gltf.scenes.size())
        for (int N : Gltf.scenes[(size_t)SceneIdx].nodes) Stack.emplace_back(N, Ident);
    else
        for (int N = 0; N < (int)Gltf.nodes.size(); N++) Stack.emplace_back(N, Ident);
    while (!Stack.empty())
    {
        const auto [NodeIdx, Parent] = Stack.back();
        Stack.pop_back();
        if (NodeIdx < 0 || NodeIdx >= (int)Gltf.nodes.size()) continue;
        const auto& N = Gltf.nodes[(size_t)NodeIdx];
        const Mat4 World = Mul(Parent, Local(N));
        if (N.mesh >= 0 && N.mesh < (int)Gltf.meshes.size())
            for (const Prim& Pr : MeshPrims(N.mesh))
            {
                double Lo[3] = { DBL_MAX, DBL_MAX, DBL_MAX }, Hi[3] = { -DBL_MAX, -DBL_MAX, -DBL_MAX };
                for (int k = 0; k < 8; k++)
                {
                    const double P[3] = { (k & 1) ? Pr.Hi[0] : Pr.Lo[0], (k & 2) ? Pr.Hi[1] : Pr.Lo[1], (k & 4) ? Pr.Hi[2] : Pr.Lo[2] };
                    for (int r = 0; r < 3; r++)
                    {
                        const double W = World[r] * P[0] + World[4 + r] * P[1] + World[8 + r] * P[2] + World[12 + r];
                        Lo[r] = std::min(Lo[r], W); Hi[r] = std::max(Hi[r], W);
                    }
                }
                const double D[3] = { Hi[0] - Lo[0], Hi[1] - Lo[1], Hi[2] - Lo[2] };
                Pieces.push_back({ &Pr, World, std::sqrt(D[0] * D[0] + D[1] * D[1] + D[2] * D[2]) });
            }
        for (int C : N.children) Stack.emplace_back(C, World);
        if (Cancelled()) return false;
    }
    St.Pieces = Pieces.size();
    for (const Piece& P : Pieces) St.Triangles += P.Pr->IdxCount / 3;

    // Over the budget, the pieces that cover the most for their triangles are kept (size squared
    // over triangles): the ground, the walls and the vistas before the dense props, a tank's
    // 30 000 triangles in a few metres
    if (O.MaxTriangles > 0 && St.Triangles > O.MaxTriangles)
    {
        const auto Worth = [](const Piece& P) { return P.Size * P.Size / (double)std::max<size_t>(1, P.Pr->IdxCount / 3); };
        std::stable_sort(Pieces.begin(), Pieces.end(), [&](const Piece& A, const Piece& B) { return Worth(A) > Worth(B); });
        std::vector<Piece> Kept;
        size_t Tris = 0;
        for (Piece& P : Pieces)
        {
            const size_t T = P.Pr->IdxCount / 3;
            if (Tris + T > O.MaxTriangles) continue;
            Tris += T;
            Kept.push_back(P);
        }
        Pieces = std::move(Kept);
    }
    St.PiecesKept = Pieces.size();

    // ---- the textures of the pieces kept, on a few threads while the pieces are placed
    std::vector<int> Wanted;
    for (const Piece& P : Pieces)
        if (P.Pr->Mat >= 0) Wanted.push_back(Slots[(size_t)P.Pr->Mat].Image);
    std::sort(Wanted.begin(), Wanted.end());
    Wanted.erase(std::unique(Wanted.begin(), Wanted.end()), Wanted.end());
    std::vector<PreviewImage> Images(Gltf.images.size());
    std::atomic<size_t> Read{ 0 }, Missing{ 0 };
    double TexturesMs = 0;
    std::thread Textures([&] {
        const auto TT = Clock::now();
        const size_t Threads = std::min<size_t>(16, std::max(2u, std::thread::hardware_concurrency() * 2 / 3));
        ParallelFor(Wanted.size(), Threads, [&](size_t k) {
            const HRESULT Com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // WIC decodes PNGs
            const int i = Wanted[k];
            const tinygltf::Image& Img = Gltf.images[(size_t)i];
            if (!Img.image.empty() && Img.width > 0 && Img.height > 0)   // embedded, already decoded
            {
                const int Comp = std::max(1, Img.component);
                std::vector<uint8_t> Rgba((size_t)Img.width * Img.height * 4, 255);
                for (size_t p = 0; p < (size_t)Img.width * Img.height && (p * Comp + Comp) <= Img.image.size(); p++)
                    for (int c = 0; c < std::min(4, Comp); c++) Rgba[p * 4 + c] = Img.image[p * Comp + c];
                Images[(size_t)i] = PreviewImageFromRGBA(Img.width, Img.height, Rgba);
            }
            else if (const std::string File = Img.uri.empty() ? std::string() : GlbImageFile(Dir, Img.uri); File.empty())
                Missing++;
            else if (PreparePreviewImage(File, PreviewTexUse::Color, -1.0f, Images[(size_t)i], nullptr, O.MaxTexture))
                Read++;
            else
                AppLog::Log().info("map preview: {} did not decode", File);
            if (SUCCEEDED(Com)) CoUninitialize();
        }, Cancelled);
        TexturesMs = Since(TT);
    });
    struct JoinTextures { std::thread& T; ~JoinTextures() { if (T.joinable()) T.join(); } } Joiner{ Textures };

    // Each piece written at its own place in the arrays, on a few threads
    size_t Vtx = 0, Idx = 0;
    for (Piece& P : Pieces) { P.Vtx = Vtx; P.Idx = Idx; Vtx += P.Pr->VtxCount; Idx += P.Pr->IdxCount; }
    St.TrianglesKept = Idx / 3;
    Out.Pos.resize(Vtx * 3); Out.Nrm.resize(Vtx * 3); Out.UV.resize(Vtx * 2); Out.VtxMat.resize(Vtx); Out.Idx.resize(Idx);
    float* const OutPos = Out.Pos.data();
    float* const OutNrm = Out.Nrm.data();
    float* const OutUV = Out.UV.data();
    int32_t* const OutMat = Out.VtxMat.data();
    uint32_t* const OutIdx = Out.Idx.data();
    ParallelFor(Pieces.size(), std::max(2u, std::thread::hardware_concurrency()), [&](size_t k) {
        const Piece& P = Pieces[k];
        const Prim& Pr = *P.Pr;
        double M[16];
        std::memcpy(M, P.World.data(), sizeof(M));
        const size_t N = Pr.VtxCount;
        float* Pos = OutPos + P.Vtx * 3;
        const float* In = Pr.Pos;
        for (size_t v = 0; v < N; v++, In += 3, Pos += 3)
        {
            const double x = In[0], y = In[1], z = In[2];
            Pos[0] = (float)(M[0] * x + M[4] * y + M[8] * z + M[12]);
            Pos[1] = (float)(M[1] * x + M[5] * y + M[9] * z + M[13]);
            Pos[2] = (float)(M[2] * x + M[6] * y + M[10] * z + M[14]);
        }
        if (Pr.Nrm)
        {
            float* Nrm = OutNrm + P.Vtx * 3;
            const float* Nin = Pr.Nrm;
            for (size_t v = 0; v < N; v++, Nin += 3, Nrm += 3)
            {
                const double nx = Nin[0], ny = Nin[1], nz = Nin[2];
                const double a = M[0] * nx + M[4] * ny + M[8] * nz, b = M[1] * nx + M[5] * ny + M[9] * nz, c = M[2] * nx + M[6] * ny + M[10] * nz;
                const double L = std::sqrt(a * a + b * b + c * c);
                const double K = L > 1e-12 ? 1.0 / L : 0.0;
                Nrm[0] = (float)(a * K); Nrm[1] = (float)(b * K); Nrm[2] = (float)(c * K);
            }
        }
        if (Pr.UV) std::memcpy(OutUV + P.Vtx * 2, Pr.UV, N * 2 * sizeof(float));
        int32_t* Mat = OutMat + P.Vtx;
        for (size_t v = 0; v < N; v++) Mat[v] = Pr.Mat;
        uint32_t* I = OutIdx + P.Idx;
        const uint32_t Base = (uint32_t)P.Vtx;
        for (size_t i = 0; i < Pr.IdxCount; i++) I[i] = Base + Pr.Idx[i];
    }, Cancelled);
    St.GeometryMs = Since(T0);

    Textures.join();
    St.TexturesMs = TexturesMs;
    St.Images = Wanted.size(); St.ImagesRead = Read; St.ImagesMissing = Missing;
    if (Cancelled()) return false;

    // the materials, parallel to the slots (Materials only sizes them: their maps are the textures)
    Out.Materials.assign(Slots.size(), PreviewTexture{});
    Out.MaterialMaps.resize(Slots.size());
    for (size_t s = 0; s < Slots.size(); s++)
    {
        PreviewMaterial& M = Out.MaterialMaps[s];
        M.Albedo = Images[(size_t)Slots[s].Image];
        M.Alpha = M.Desc.Alpha = Slots[s].Alpha;
        M.Desc.AlphaCutoff = Slots[s].Cutoff;
    }
    return !Out.Idx.empty();
}

bool RenderGlbToPNG(const std::string& GlbPath, const std::string& OutPngPath, int Size)
{
    PreviewGeometry P;
    if (!LoadPreviewGeometryGlb(GlbPath, P) || P.Idx.empty()) return false;

    // Adapt the flat PreviewGeometry (per-VERTEX material) back to the software rasterizer's Geo/Tex
    // shape (per-TRIANGLE material) - safe because every triangle's 3 corners share one VtxMat value,
    // set uniformly per glTF primitive in LoadPreviewGeometryGlb.
    Geo G;
    const size_t VertCount = P.Pos.size() / 3;
    G.Pos.resize(VertCount); G.Nrm.resize(VertCount); G.UV.resize(VertCount);
    for (size_t v = 0; v < VertCount; v++)
    {
        G.Pos[v] = { P.Pos[v*3], P.Pos[v*3+1], P.Pos[v*3+2] };
        G.Nrm[v] = { P.Nrm[v*3], P.Nrm[v*3+1], P.Nrm[v*3+2] };
        G.UV[v]  = { P.UV[v*2],  P.UV[v*2+1] };
    }
    G.Idx = P.Idx;
    G.TriMat.resize(P.Idx.size() / 3);
    for (size_t t = 0; t < G.TriMat.size(); t++)
    {
        const uint32_t V0 = P.Idx[t * 3];
        G.TriMat[t] = (V0 < P.VtxMat.size()) ? P.VtxMat[V0] : -1;
    }

    std::vector<Tex> Textures(P.Materials.size());
    for (size_t i = 0; i < P.Materials.size(); i++)
    {
        Textures[i].W = P.Materials[i].Width; Textures[i].H = P.Materials[i].Height;
        Textures[i].Rgba = P.Materials[i].Rgba;
        if (Textures[i].Rgba.empty() && i < P.MaterialMaps.size())
            PreviewImageToRGBA8(P.MaterialMaps[i].Albedo, Textures[i].W, Textures[i].H, Textures[i].Rgba);
    }

    if (!RasterizeAndSave(G, Textures, OutPngPath, Size)) return false;
    // a map: also its layout from above and its middle closer in, beside the 3/4 view
    const std::filesystem::path Out(OutPngPath);
    for (const auto& [View, Suffix] : { std::pair<int, const char*>{ 1, "_top" }, { 2, "_close" } })
    {
        std::vector<uint8_t> Px;
        if (RasterizeToRGBA(G, Textures, Size, 0.0f, Px, View))
            SavePng(Px, Size, Size, (Out.parent_path() / (Out.stem().string() + Suffix + Out.extension().string())).string());
    }
    return true;
}

bool RenderPreviewGeometryToPNG(const PreviewGeometry& P, const std::string& OutPngPath, int Size)
{
    if (P.Idx.empty() || P.Pos.empty()) return false;

    // Same per-vertex -> per-triangle material adaptation as RenderGlbToPNG (see its comment) - every
    // triangle's 3 corners share one VtxMat value in every PreviewGeometry producer this codebase has.
    Geo G;
    const size_t VertCount = P.Pos.size() / 3;
    G.Pos.resize(VertCount); G.Nrm.resize(VertCount); G.UV.resize(VertCount);
    for (size_t v = 0; v < VertCount; v++)
    {
        G.Pos[v] = { P.Pos[v*3], P.Pos[v*3+1], P.Pos[v*3+2] };
        G.Nrm[v] = { P.Nrm[v*3], P.Nrm[v*3+1], P.Nrm[v*3+2] };
        G.UV[v]  = { P.UV[v*2],  P.UV[v*2+1] };
    }
    G.Idx = P.Idx;
    G.TriMat.resize(P.Idx.size() / 3);
    for (size_t t = 0; t < G.TriMat.size(); t++)
    {
        const uint32_t V0 = P.Idx[t * 3];
        G.TriMat[t] = (V0 < P.VtxMat.size()) ? P.VtxMat[V0] : -1;
    }

    std::vector<Tex> Textures2(P.Materials.size());
    for (size_t i = 0; i < P.Materials.size(); i++)
    {
        Textures2[i].W = P.Materials[i].Width; Textures2[i].H = P.Materials[i].Height;
        Textures2[i].Rgba = P.Materials[i].Rgba;
        if (Textures2[i].Rgba.empty() && i < P.MaterialMaps.size())
            PreviewImageToRGBA8(P.MaterialMaps[i].Albedo, Textures2[i].W, Textures2[i].H, Textures2[i].Rgba);
    }

    return RasterizeAndSave(G, Textures2, OutPngPath, Size);
}
