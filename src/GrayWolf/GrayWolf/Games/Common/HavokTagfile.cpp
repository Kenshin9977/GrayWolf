#include "stdafx.h"

#include "Games/Common/HavokTagfile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace Havok
{
namespace
{
    uint32_t BigEndian32(const uint8_t* P) { return (uint32_t)P[0] << 24 | (uint32_t)P[1] << 16 | (uint32_t)P[2] << 8 | P[3]; }

    // A section: its tag, contents [Begin, End)
    struct Section
    {
        char Tag[5] = {};
        const uint8_t* Begin = nullptr;
        const uint8_t* End = nullptr;
    };

    // The sections between Begin and End, stopping at the first that does not fit
    std::vector<Section> Sections(const uint8_t* Begin, const uint8_t* End)
    {
        std::vector<Section> Out;
        for (const uint8_t* P = Begin; End - P >= 8;)
        {
            const uint32_t Size = BigEndian32(P) & 0x3fffffff;
            if (Size < 8 || Size > (size_t)(End - P)) break;
            Section S;
            std::memcpy(S.Tag, P + 4, 4);
            S.Begin = P + 8, S.End = P + Size;
            Out.push_back(S);
            P += Size;
        }
        return Out;
    }

    const Section* Find(const std::vector<Section>& Secs, const char* Tag)
    {
        for (const Section& S : Secs)
            if (std::memcmp(S.Tag, Tag, 4) == 0) return &S;
        return nullptr;
    }

    // TNA1 / TBDY's packed integers
    class Packed
    {
    public:
        Packed(const uint8_t* Begin, const uint8_t* End) : P(Begin), End(End) {}
        bool More() const { return P < End; }
        bool Ok() const { return Good; }
        uint64_t Next()
        {
            if (P >= End) return Fail();
            const uint8_t B = *P;
            size_t N = 1;
            uint64_t V = 0;
            if (B < 0x80) V = B;
            else if (B < 0xc0) N = 2, V = B & 0x3f;
            else if (B < 0xe0) N = 3, V = B & 0x1f;
            else if (B < 0xe8) N = 4, V = B & 0x07;
            else if (B < 0xf0) N = 5, V = B & 0x07;
            else if (B == 0xf8) N = 9, V = 0;
            else return Fail();
            if ((size_t)(End - P) < N) return Fail();
            for (size_t K = 1; K < N; K++) V = V << 8 | P[K];
            P += N;
            return V;
        }

    private:
        uint64_t Fail()
        {
            Good = false;
            P = End;
            return 0;
        }
        const uint8_t* P;
        const uint8_t* End;
        bool Good = true;
    };

    std::vector<std::string> Strings(const Section* S)
    {
        std::vector<std::string> Out;
        if (S == nullptr) return Out;
        for (const uint8_t* P = S->Begin; P < S->End;)
        {
            const uint8_t* Z = std::find(P, S->End, (uint8_t)0);
            if (Z == S->End) break;
            Out.emplace_back((const char*)P, (size_t)(Z - P));
            P = Z + 1;
        }
        return Out;
    }

    // ---- the shapes: everything bounds-checked against DATA
    class Shapes
    {
    public:
        using M34 = std::array<std::array<double, 4>, 3>;   // rows: the linear part, then the translation

        Shapes(const uint8_t* Data, size_t Size, const Layout& L, float Scale, Geometry& Out, std::map<std::string, size_t>& Counters)
            : Data(Data), Size(Size), L(L), Scale(Scale), Out(Out), Counters(Counters)
        {
        }

        template <class T>
        bool Get(int64_t At, T& V) const
        {
            if (At < 0 || (uint64_t)At + sizeof(T) > Size) return false;
            std::memcpy(&V, Data + At, sizeof(T));
            return true;
        }
        template <class T>
        T Get(int64_t At) const
        {
            T V{};
            Get(At, V);
            return V;
        }
        // an hkRelArrayView at At: (start, count), start -1 when its Count records of Stride do not fit
        std::pair<int64_t, int32_t> View(int64_t At, size_t Stride) const
        {
            const int32_t Offset = Get<int32_t>(At), Count = Get<int32_t>(At + 4);
            const int64_t Start = At + Offset;
            if (Count <= 0 || Start < 0 || (uint64_t)Start + (uint64_t)Count * Stride > Size) return { -1, 0 };
            return { Start, Count };
        }

        static M34 Matrix(const Transform& T)
        {
            const double X = T.Rotation[0], Y = T.Rotation[1], Z = T.Rotation[2], W = T.Rotation[3];
            const double R[3][3] = { { 1 - 2 * (Y * Y + Z * Z), 2 * (X * Y - Z * W), 2 * (X * Z + Y * W) },
                                     { 2 * (X * Y + Z * W), 1 - 2 * (X * X + Z * Z), 2 * (Y * Z - X * W) },
                                     { 2 * (X * Z - Y * W), 2 * (Y * Z + X * W), 1 - 2 * (X * X + Y * Y) } };
            M34 M{};
            for (int I = 0; I < 3; I++)
            {
                for (int J = 0; J < 3; J++) M[I][J] = R[I][J] * T.Scale[J];
                M[I][3] = T.Translation[I];
            }
            return M;
        }
        static M34 Combine(const M34& A, const M34& B)   // A after B
        {
            M34 M{};
            for (int I = 0; I < 3; I++)
                for (int J = 0; J < 4; J++)
                {
                    double S = J == 3 ? A[I][3] : 0.0;
                    for (int K = 0; K < 3; K++) S += A[I][K] * B[K][J];
                    M[I][J] = S;
                }
            return M;
        }
        static std::array<double, 3> Apply(const M34& M, double X, double Y, double Z)
        {
            std::array<double, 3> P;
            for (int I = 0; I < 3; I++) P[I] = M[I][0] * X + M[I][1] * Y + M[I][2] * Z + M[I][3];
            return P;
        }

        bool Read(int64_t At, const M34& Place, uint16_t Tag, int Depth)
        {
            uint8_t Kind = 0;
            if (!Get(At + 0x18, Kind)) return Unread("havok_shapes_outside");
            if (Kind == L.Convex || Kind == L.Box) return Convex(At, Place, Tag);
            if (Kind == L.Mesh) return Mesh(At, Place, Tag);
            if (Kind == L.Compound) return Depth < 4 ? Compound(At, Place, Tag, Depth) : Unread("havok_compounds_too_deep");
            return Unread("havok_shape_type_" + std::to_string(Kind));
        }

    private:
        bool Unread(const std::string& Why)
        {
            Counters[Why]++;
            return false;
        }

        // hknpConvexShape / hknpBoxShape: the hull's (L.Hull) planes (normal, -distance) at 8, its
        // vertices (hkFloat3) at 0; a plane's side through the placed hull's farthest vertex
        bool Convex(int64_t At, const M34& Place, uint16_t Tag)
        {
            const auto [Vertices, VertexCount] = View(At + L.Hull, 12);
            const auto [Planes, PlaneCount] = View(At + L.Hull + 8, 16);
            if (Vertices < 0 || Planes < 0 || PlaneCount < 4 || PlaneCount > 256 || VertexCount > 4096) return Unread("havok_convex_unread");
            if (Get<float>(At + L.Radius) != 0.0f) Counters["havok_convex_with_radius"]++;
            std::vector<std::array<double, 3>> Pts;
            for (int32_t K = 0; K < VertexCount; K++)
            {
                float P[3];
                std::memcpy(P, Data + Vertices + (int64_t)K * 12, 12);
                Pts.push_back(Apply(Place, P[0], P[1], P[2]));
            }
            // normals go by the inverse transpose of the linear part: its cofactors
            const auto& L = Place;
            const double C[3][3] = { { L[1][1] * L[2][2] - L[1][2] * L[2][1], L[1][2] * L[2][0] - L[1][0] * L[2][2], L[1][0] * L[2][1] - L[1][1] * L[2][0] },
                                     { L[0][2] * L[2][1] - L[0][1] * L[2][2], L[0][0] * L[2][2] - L[0][2] * L[2][0], L[0][1] * L[2][0] - L[0][0] * L[2][1] },
                                     { L[0][1] * L[1][2] - L[0][2] * L[1][1], L[0][2] * L[1][0] - L[0][0] * L[1][2], L[0][0] * L[1][1] - L[0][1] * L[1][0] } };
            const double Det = L[0][0] * C[0][0] + L[0][1] * C[0][1] + L[0][2] * C[0][2];
            Maps::Brush B;
            for (int32_t K = 0; K < PlaneCount; K++)
            {
                float Pl[4];
                std::memcpy(Pl, Data + Planes + (int64_t)K * 16, 16);
                double N[3];
                for (int I = 0; I < 3; I++) N[I] = (C[I][0] * Pl[0] + C[I][1] * Pl[1] + C[I][2] * Pl[2]) * (Det < 0 ? -1.0 : 1.0);
                const double Len = std::sqrt(N[0] * N[0] + N[1] * N[1] + N[2] * N[2]);
                if (Len < 1e-12) continue;
                double Far = -1e300;
                for (const auto& P : Pts) Far = (std::max)(Far, (N[0] * P[0] + N[1] * P[1] + N[2] * P[2]) / Len);
                Maps::BrushSide S;
                for (int I = 0; I < 3; I++) S.Normal[I] = (float)(N[I] / Len);
                S.Dist = (float)(Far * Scale);
                B.Sides.push_back(S);
            }
            if (B.Sides.size() < 4) return Unread("havok_convex_flat");
            Out.Convex.push_back(std::move(B));
            Out.ConvexTags.push_back(Tag);
            return true;
        }

        // hknpMeshShape: its geometry sections' vertices and primitives, each triangle's tag from
        // the shape tag table (the last entry whose primitive key is not above the triangle's)
        bool Mesh(int64_t At, const M34& Place, uint16_t Tag)
        {
            float Inv[3];
            if (!Get(At + L.MeshInverse, Inv)) return Unread("havok_meshes_unread");
            std::vector<uint32_t> Keys;
            std::vector<uint16_t> Tags;
            if (const auto [Table, Entries] = View(At + L.MeshTags, 8); Table >= 0)
                for (int32_t K = 0; K < Entries; K++) Keys.push_back(Get<uint32_t>(Table + K * 8)), Tags.push_back(Get<uint16_t>(Table + K * 8 + 4));
            const auto [Sections, SectionCount] = View(At + L.MeshSections, 0x40);
            if (Sections < 0) return Unread("havok_meshes_unread");
            const auto TagOf = [&](uint32_t Key) {
                const auto It = std::upper_bound(Keys.begin(), Keys.end(), Key);
                const uint16_t T = It == Keys.begin() ? (uint16_t)0xffff : Tags[(size_t)(It - Keys.begin()) - 1];
                return T == 0xffff ? Tag : T;
            };
            for (int32_t S = 0; S < SectionCount; S++)
            {
                const int64_t Sec = Sections + (int64_t)S * 0x40;
                int32_t Offset[3];
                Get(Sec + 0x20, Offset);
                const bool Floats = Offset[0] == 0x7fffffff && Offset[1] == 0x7fffffff && Offset[2] == 0x7fffffff;
                const auto [Buffer, BufferSize] = View(Sec + 0x10, L.VertexBytes ? 1 : 6);
                const auto [Prims, PrimCount] = View(Sec + 8, 4);
                if (Buffer < 0 || Prims < 0)
                {
                    Counters["havok_mesh_sections_unread"]++;
                    continue;
                }
                if (Floats && !L.VertexBytes)
                {
                    Counters["havok_mesh_sections_unread"]++;
                    continue;
                }
                const int32_t Count = L.VertexBytes ? BufferSize / (Floats ? 12 : 6) : BufferSize;
                const uint32_t First = (uint32_t)(Out.Vertices.size() / 3);
                for (int32_t V = 0; V < Count; V++)
                {
                    double P[3];
                    if (Floats)
                    {
                        float F[3];
                        std::memcpy(F, Data + Buffer + (int64_t)V * 12, 12);
                        P[0] = F[0], P[1] = F[1], P[2] = F[2];
                    }
                    else
                    {
                        uint16_t Q[3];
                        std::memcpy(Q, Data + Buffer + (int64_t)V * 6, 6);
                        for (int A = 0; A < 3; A++) P[A] = ((double)Offset[A] + Q[A]) * Inv[A];
                    }
                    const auto W = Apply(Place, P[0], P[1], P[2]);
                    for (int A = 0; A < 3; A++) Out.Vertices.push_back((float)(W[A] * Scale));
                }
                for (int32_t K = 0; K < PrimCount && K < 256; K++)
                {
                    const uint8_t* Pr = Data + Prims + (int64_t)K * 4;
                    if (Pr[0] >= Count || Pr[1] >= Count || Pr[2] >= Count || Pr[3] >= Count)
                    {
                        Counters["havok_mesh_primitives_outside"]++;
                        continue;
                    }
                    const uint32_t Key = (uint32_t)S << 9 | (uint32_t)K << 1;
                    Out.Triangles.insert(Out.Triangles.end(), { First + Pr[0], First + Pr[1], First + Pr[2] });
                    Out.TriangleTags.push_back(TagOf(Key));
                    if (Pr[2] == Pr[3]) continue;
                    Out.Triangles.insert(Out.Triangles.end(), { First + Pr[0], First + Pr[2], First + Pr[3] });
                    Out.TriangleTags.push_back(TagOf(Key | 1));
                }
            }
            return true;
        }

        // hknpCompoundShape: its instances, each its shape placed by its transform
        bool Compound(int64_t At, const M34& Place, uint16_t Tag, int Depth)
        {
            const int64_t Field = At + L.Instances;
            const int64_t Start = Field + Get<int64_t>(Field);
            const int32_t Count = Get<int32_t>(Field + 8);
            if (Count < 0 || Start < 0 || (uint64_t)Start + (uint64_t)Count * 0x40 > Size) return Unread("havok_compounds_unread");
            if (Get<int32_t>(Field + 0x10) != -1) Counters["havok_compounds_with_free_slots"]++;
            for (int32_t K = 0; K < Count; K++)
            {
                const int64_t I = Start + (int64_t)K * 0x40;
                Transform T;
                Get(I, T.Rotation);
                Get(I + 0x10, T.Translation);
                Get(I + 0x20, T.Scale);
                const uint16_t Own = Get<uint16_t>(I + 0x2c);
                const int64_t Shape = I + 0x30 + Get<int64_t>(I + 0x30);
                Read(Shape, Combine(Place, Matrix(T)), Own == 0xffff ? Tag : Own, Depth + 1);
            }
            return true;
        }

        const uint8_t* Data;
        size_t Size;
        const Layout& L;
        float Scale;
        Geometry& Out;
        std::map<std::string, size_t>& Counters;
    };
}

bool Tagfile::Open(const uint8_t* Bytes, size_t Avail)
{
    *this = Tagfile();
    if (Bytes == nullptr || Avail < 8 || std::memcmp(Bytes + 4, "TAG0", 4) != 0) return false;
    Size = BigEndian32(Bytes) & 0x3fffffff;
    if (Size < 8 || Size > Avail) return false;
    const auto Top = Sections(Bytes + 8, Bytes + Size);
    if (const Section* V = Find(Top, "SDKV")) Sdk.assign((const char*)V->Begin, (size_t)(V->End - V->Begin));
    const Section* D = Find(Top, "DATA");
    if (D == nullptr) return false;
    Data = D->Begin, DataSize = (size_t)(D->End - D->Begin);
    if (const Section* T = Find(Top, "TYPE"); T != nullptr && !ReadTypes(T->Begin, T->End)) Types.clear();
    if (const Section* I = Find(Top, "INDX"); I != nullptr && !ReadIndex(I->Begin, I->End)) Items.clear();
    return true;
}

bool Tagfile::ReadTypes(const uint8_t* Begin, const uint8_t* End)
{
    const auto Sub = Sections(Begin, End);
    const auto TypeNames = Strings(Find(Sub, "TST1")), FieldNames = Strings(Find(Sub, "FST1"));
    const Section* Na = Find(Sub, "TNA1");
    const Section* Bd = Find(Sub, "TBDY");
    if (Na == nullptr || Bd == nullptr) return false;
    Packed N(Na->Begin, Na->End);
    const uint64_t Count = N.Next();
    if (Count == 0 || Count > 0x10000) return false;
    Types.assign((size_t)Count, Type());
    for (uint64_t K = 1; K < Count && N.Ok(); K++)
    {
        const uint64_t Name = N.Next();
        if (Name < TypeNames.size()) Types[K].Name = TypeNames[Name];
        const uint64_t Templates = N.Next();
        for (uint64_t T = 0; T < Templates && N.Ok(); T++) N.Next(), N.Next();
    }
    Packed B(Bd->Begin, Bd->End);
    while (B.More() && B.Ok())
    {
        const uint64_t K = B.Next();
        if (K == 0) continue;
        if (K >= Count) return false;
        Type& T = Types[K];
        T.Parent = (uint32_t)B.Next();
        const uint64_t F = T.Flags = (uint32_t)B.Next();
        uint64_t SubType = 0;
        if (F & 1) SubType = B.Next();
        if ((F & 2) && (SubType & 0xf)) B.Next();
        if (F & 4) B.Next();
        if (F & 8) T.Size = (uint32_t)B.Next(), B.Next();
        if (F & 0x10) B.Next();
        if (F & 0x20)
        {
            const uint64_t Members = B.Next() & 0xffff;
            for (uint64_t M = 0; M < Members && B.Ok(); M++)
            {
                Member Mb;
                const uint64_t Name = B.Next();
                Mb.Name = Name < FieldNames.size() ? FieldNames[Name] : std::string();
                Mb.Flags = (uint32_t)B.Next(), Mb.Offset = (uint32_t)B.Next(), Mb.Type = (uint32_t)B.Next();
                T.Members.push_back(std::move(Mb));
            }
        }
        if (F & 0x40)
        {
            const uint64_t Interfaces = B.Next();
            for (uint64_t I = 0; I < Interfaces && B.Ok(); I++) B.Next(), B.Next();
        }
        if (F & 0x80) B.Next();
    }
    return B.Ok();
}

bool Tagfile::ReadIndex(const uint8_t* Begin, const uint8_t* End)
{
    // (the sections held while It points into them: a temporary's, freed at the end of the
    // statement, was read after its heap block had gone to another thread, a few items or none)
    const auto Sub = Sections(Begin, End);
    const Section* It = Find(Sub, "ITEM");
    if (It == nullptr) return false;
    for (const uint8_t* P = It->Begin; It->End - P >= 12; P += 12)
    {
        uint32_t W[3];
        std::memcpy(W, P, 12);
        Item I;
        I.Type = W[0] & 0xffffff, I.Flags = W[0] >> 24, I.Offset = W[1], I.Count = W[2];
        Items.push_back(I);
    }
    return true;
}

uint32_t Tagfile::TypeNamed(const std::string& Name) const
{
    for (size_t K = 1; K < Types.size(); K++)
        if (Types[K].Name == Name) return (uint32_t)K;
    return 0;
}

int64_t Tagfile::MemberOffset(uint32_t K, const std::string& Name) const
{
    for (int Depth = 0; K != 0 && K < Types.size() && Depth < 32; Depth++, K = Types[K].Parent)
        for (const Member& M : Types[K].Members)
            if (M.Name == Name) return M.Offset;
    return -1;
}

int64_t Tagfile::ObjectOf(const std::string& Name) const
{
    const uint32_t T = TypeNamed(Name);
    for (const Item& I : Items)
        if (T != 0 && I.Type == T && (I.Flags & 0x10) && I.Offset < DataSize) return I.Offset;
    return -1;
}

const Layout* LayoutFor(const std::string& Sdk)
{
    // 2024.2 (Modern Warfare 4, Black Ops 7): the defaults; 2021.1 (Modern Warfare III, Black Ops
    // 6): hknpShape 0x40 (convex radius 0x20), hknpCompositeShape 0x48, the types numbered anew
    static const Layout Sdk2024;
    static const Layout Sdk2021 = [] {
        Layout L;
        L.Mesh = 8, L.Compound = 12;
        L.Radius = 0x20, L.Hull = 0x40;
        L.MeshInverse = 0x60, L.MeshTags = 0x70, L.MeshSections = 0x90, L.VertexBytes = false;
        L.Instances = 0x48;
        return L;
    }();
    if (Sdk.rfind("2024", 0) == 0) return &Sdk2024;
    if (Sdk.rfind("2021", 0) == 0) return &Sdk2021;
    return nullptr;
}

bool ReadShape(const uint8_t* Data, size_t DataSize, size_t At, const Layout& L, const Transform& Place, uint16_t Tag, float Scale, Geometry& Out,
               std::map<std::string, size_t>& Counters)
{
    Shapes S(Data, DataSize, L, Scale, Out, Counters);
    return S.Read((int64_t)At, Shapes::Matrix(Place), Tag, 0);
}

bool ShapeTagContents(const Tagfile& List, std::vector<uint32_t>& Out)
{
    // HavokPhysicsShapeList.shapeTagData: an hkArray (m_data: an item number; m_size 0 in the file,
    // the item's count is the size) of HavokPhysicsShapeList::ShapeTagData (collisionFilterInfo)
    const int64_t Root = List.ObjectOf("HavokPhysicsShapeList");
    const int64_t Field = Root < 0 ? -1 : List.MemberOffset(List.TypeNamed("HavokPhysicsShapeList"), "shapeTagData");
    const uint32_t Record = List.TypeNamed("HavokPhysicsShapeList::ShapeTagData");
    const int64_t Filter = List.MemberOffset(Record, "collisionFilterInfo");
    if (Field < 0 || Filter < 0 || List.Types[Record].Size == 0 || (uint64_t)(Root + Field) + 16 > List.DataSize) return false;
    uint64_t ItemNumber = 0;
    std::memcpy(&ItemNumber, List.Data + Root + Field, 8);
    Out.clear();
    if (ItemNumber == 0) return true;
    if (ItemNumber >= List.Items.size()) return false;
    const Item& I = List.Items[(size_t)ItemNumber];
    const uint64_t Stride = List.Types[Record].Size, Count = I.Count;
    if (I.Type != Record || (uint64_t)I.Offset + Stride * Count > List.DataSize) return false;
    for (uint64_t K = 0; K < Count; K++)
    {
        uint32_t V = 0;
        std::memcpy(&V, List.Data + I.Offset + Stride * (uint64_t)K + (uint64_t)Filter, 4);
        Out.push_back(V);
    }
    return true;
}

void AddLevelCollision(Maps::Collision& Out, const std::vector<uint32_t>& TagContents, const std::vector<std::vector<uint8_t>>& Tiles, float Scale,
                       std::map<std::string, size_t>& Counters)
{
    std::map<uint32_t, uint32_t> MaterialOf;   // content flags -> Out.Materials
    const auto Material = [&](uint16_t Tag) {
        if (Tag >= TagContents.size())
        {
            Counters["havok_tags_unknown"]++;
            return Maps::Collision::NoMaterial;
        }
        const uint32_t Flags = TagContents[Tag];
        const auto [It, New] = MaterialOf.try_emplace(Flags, (uint32_t)Out.Materials.size());
        if (New) Out.Materials.emplace_back("", Flags);
        return It->second;
    };
    for (size_t K = 0; K < Out.Materials.size(); K++) MaterialOf.try_emplace(Out.Materials[K].second, (uint32_t)K);
    // the triangles already there keep theirs; without materials they had none
    const bool Had = Out.Triangles.empty() || Out.TriangleMaterials.size() * 3 == Out.Triangles.size();
    if (!Had) Out.TriangleMaterials.assign(Out.Triangles.size() / 3, Maps::Collision::NoMaterial);
    for (const auto& Bytes : Tiles)
    {
        Tagfile T;
        if (!T.Open(Bytes.data(), Bytes.size()))
        {
            Counters["havok_tagfiles_unread"]++;
            continue;
        }
        Counters["havok_tagfiles"]++;
        const Layout* L = LayoutFor(T.Sdk);
        if (L == nullptr)
        {
            Counters["havok_sdk_unknown"]++;
            continue;
        }
        Geometry G;
        if (!ReadShape(T.Data, T.DataSize, 0, *L, Transform(), 0xffff, Scale, G, Counters)) continue;
        const uint32_t First = (uint32_t)(Out.Vertices.size() / 3);
        Out.Vertices.insert(Out.Vertices.end(), G.Vertices.begin(), G.Vertices.end());
        for (const uint32_t V : G.Triangles) Out.Triangles.push_back(First + V);
        for (const uint16_t Tag : G.TriangleTags) Out.TriangleMaterials.push_back(Material(Tag));
        Counters["havok_triangles"] += G.TriangleTags.size();
        for (size_t K = 0; K < G.Convex.size(); K++)
        {
            Maps::Brush& B = G.Convex[K];
            const uint32_t M = Material(G.ConvexTags[K]);
            B.Contents = M < Out.Materials.size() ? Out.Materials[M].second : 0;
            // a volume that stops nothing (foliage 0x2, 0x2000000, 0x10000000: MW4 shipment's 112,
            // 81, 125) would be a clip or hint brush: left out as such triangles are
            if (!Maps::Collision::Stops(B.Contents))
            {
                Counters["havok_convex_noncolliding"]++;
                continue;
            }
            // a physics volume is no sky surface: the lineage's sky bit (0x800) would make it a sky
            // brush in the play area (MW4 shipment: 39 bullet-blocking boxes, 0x1028c0)
            if (B.Contents & 0x800)
            {
                B.Contents &= ~0x800u;
                Counters["havok_convex_sky_bit_dropped"]++;
            }
            for (Maps::BrushSide& S : B.Sides) S.Contents = B.Contents;
            Out.Brushes.push_back(std::move(B));
            Out.ClipBrushes++;
        }
        Counters["havok_convex_pieces"] += G.Convex.size();
    }
}
}

namespace Havok
{
void ReadLevel(Maps::Collision& Out, const LevelSource& Src, float Scale, std::map<std::string, size_t>& Counters)
{
    const auto U32 = [&](uint64_t A) {
        uint32_t V = 0;
        if (const uint8_t* P = Src.Locate(A, 4)) std::memcpy(&V, P, 4);
        return V;
    };
    const auto U64 = [&](uint64_t A) {
        uint64_t V = 0;
        if (const uint8_t* P = Src.Locate(A, 8)) std::memcpy(&V, P, 8);
        return V;
    };
    Tagfile List;
    std::vector<uint32_t> TagContents;
    const uint32_t ListSize = U32(Src.ClipMap + (uint64_t)Src.ShapeListSize);
    const uint8_t* ListBytes = ListSize ? Src.Locate(U64(Src.ClipMap + (uint64_t)Src.ShapeList), ListSize) : nullptr;
    if (ListBytes == nullptr)
        Counters["havok_shape_list_outside"]++;
    else if (!List.Open(ListBytes, ListSize))
        Counters["havok_shape_list_unread"]++;
    else if (!ShapeTagContents(List, TagContents))
        Counters["havok_shape_list_without_tags"]++;
    std::vector<std::vector<uint8_t>> Tiles;
    for (const uint64_t Tile : Src.Tiles)
    {
        Counters["havok_tiles"]++;
        for (const auto& [Pointer, SizeAt] : Src.TileShapes)
        {
            const uint32_t N = U32(Tile + (uint64_t)SizeAt);
            const uint64_t P = U64(Tile + (uint64_t)Pointer);
            if (N == 0 || P == 0) continue;
            if (const uint8_t* B = Src.Locate(P, N))
                Tiles.emplace_back(B, B + N);
            else
                Counters["havok_tile_tagfiles_outside"]++;
        }
    }
    if (!TagContents.empty()) AddLevelCollision(Out, TagContents, Tiles, Scale, Counters);
}
}
