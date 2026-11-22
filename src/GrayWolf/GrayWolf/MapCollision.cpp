#include "stdafx.h"

#include "MapCollision.h"

#include "ExportNaming.h"
#include "MapRadiantWriter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <vector>

namespace Maps
{
namespace
{
    using ExportNaming::At;

    constexpr double ModelPrismDepth = 2.0;   // a brush model's triangle as a brush, as the Radiant export makes the world's

    class Reader
    {
    public:
        Reader(const ClipSource& Src, const ClipLayout& L, std::map<std::string, size_t>& Counters)
            : Src(Src), Img(*Src.Image), L(L), Counters(Counters)
        {
        }

        uint64_t Pointer(int64_t Pos) const
        {
            return Src.PointerSize == 4 ? (uint64_t)At<uint32_t>(Img, Pos) : At<uint64_t>(Img, Pos);
        }
        int64_t Follow(int64_t Pos) const
        {
            const uint64_t P = Pointer(Pos);
            return P ? Src.Off(P) : -1;
        }

        // One table of brushes (clipMap_t's or a ClipInfo's), Info its start
        struct Table
        {
            int64_t Brushes = -1, Bounds = -1, Contents = -1, Nodes = -1, Planes = -1;
            int64_t Tables = -1;   // where the brushes' tables are (ClipLayout::Tables)
            uint32_t BrushCount = 0, NodeCount = 0, PlaneCount = 0;
            std::vector<std::string> Materials;
            std::vector<std::pair<uint32_t, uint32_t>> MaterialFlags;   // surface, contents
        };
        Table Open(int64_t Info) const
        {
            Table T;
            const ClipInfoLayout& I = L.Info;
            T.Tables = Info;
            if (L.Tables >= 0)
            {
                const int64_t Holder = Follow(Info + L.Tables);
                T.Tables = Holder >= 0 ? Holder + L.TablesAt : -1;
            }
            const int64_t B = T.Tables;
            if (B >= 0 && I.BrushCount >= 0) T.BrushCount = I.BrushCountBytes == 4 ? At<uint32_t>(Img, B + I.BrushCount) : At<uint16_t>(Img, B + I.BrushCount);
            if (B >= 0 && I.Brushes >= 0) T.Brushes = Follow(B + I.Brushes);
            if (B >= 0 && I.BrushBounds >= 0) T.Bounds = Follow(B + I.BrushBounds);
            if (B >= 0 && I.BrushContents >= 0) T.Contents = Follow(B + I.BrushContents);
            if (I.LeafBrushNodes >= 0) T.Nodes = Follow(Info + I.LeafBrushNodes);
            if (I.LeafBrushNodeCount >= 0) T.NodeCount = At<uint32_t>(Img, Info + I.LeafBrushNodeCount);
            if (I.Planes >= 0) T.Planes = Follow(Info + I.Planes);
            if (I.PlaneCount >= 0) T.PlaneCount = At<uint32_t>(Img, Info + I.PlaneCount);
            const int64_t Mats = I.Materials >= 0 ? Follow(Info + I.Materials) : -1;
            const uint32_t MatCount = I.MaterialCount >= 0 ? At<uint32_t>(Img, Info + I.MaterialCount) : 0;
            for (uint32_t K = 0; Mats >= 0 && K < MatCount && K < 65536; K++)
            {
                const int64_t M = Mats + (int64_t)K * L.MaterialSize;
                std::string Name;
                if (L.MaterialNameInline)
                {
                    for (int64_t C = 0; C < 64 && M + L.MaterialName + C < (int64_t)Img.size(); C++)
                    {
                        const char Ch = (char)Img[(size_t)(M + L.MaterialName + C)];
                        if (Ch == 0) break;
                        Name += Ch;
                    }
                }
                else if (L.MaterialName >= 0)
                    Name = Src.String(Pointer(M + L.MaterialName));
                T.Materials.push_back(Name);
                T.MaterialFlags.emplace_back(L.MaterialSurface >= 0 ? At<uint32_t>(Img, M + L.MaterialSurface) : 0,
                                             L.MaterialContents >= 0 ? At<uint32_t>(Img, M + L.MaterialContents) : 0);
            }
            return T;
        }

        bool ReadBrush(const Table& T, uint32_t Index, Brush& Out) const
        {
            if (T.Brushes < 0 || Index >= T.BrushCount) return false;
            const int64_t B = T.Brushes + (int64_t)Index * L.BrushSize;
            if (B + L.BrushSize > (int64_t)Img.size()) return false;
            float Mins[3], Maxs[3];
            for (int K = 0; K < 3; K++)
            {
                if (L.BrushMins >= 0)
                {
                    Mins[K] = At<float>(Img, B + L.BrushMins + K * 4);
                    Maxs[K] = At<float>(Img, B + L.BrushMaxs + K * 4);
                }
                else
                {
                    // Bounds: midPoint, halfSize
                    const int64_t Bo = T.Bounds + (int64_t)Index * 24;
                    const float Mid = At<float>(Img, Bo + K * 4), Half = At<float>(Img, Bo + 12 + K * 4);
                    Mins[K] = Mid - Half;
                    Maxs[K] = Mid + Half;
                }
            }
            if (L.BrushMins < 0 && T.Bounds < 0) return false;
            Out.Contents = L.BrushContents >= 0 ? At<uint32_t>(Img, B + L.BrushContents)
                         : T.Contents >= 0      ? At<uint32_t>(Img, T.Contents + (int64_t)Index * L.BrushContentsSize)
                                                : 0;
            const auto Named = [&T](BrushSide& S, uint32_t Material) {
                if (Material >= T.Materials.size()) return;
                S.Material = T.Materials[Material];
                S.Surface = T.MaterialFlags[Material].first;
                S.Contents = T.MaterialFlags[Material].second;
            };
            // the bounding box's six sides: min then max per axis, as the game keeps their materials
            for (int Side = 0; Side < 2; Side++)
                for (int K = 0; K < 3; K++)
                {
                    BrushSide S;
                    S.Normal[0] = S.Normal[1] = S.Normal[2] = 0;
                    S.Normal[K] = Side ? 1.0f : -1.0f;
                    S.Dist = Side ? Maxs[K] : -Mins[K];
                    if (L.AxialMaterial >= 0) Named(S, (uint32_t)At<int16_t>(Img, B + L.AxialMaterial + (Side * 3 + K) * 2));
                    if (L.AxialContents >= 0) S.Contents = At<uint32_t>(Img, B + L.AxialContents + (Side * 3 + K) * 4);
                    if (L.AxialSurface >= 0) S.Surface = At<uint32_t>(Img, B + L.AxialSurface + (Side * 3 + K) * 4);
                    Out.Sides.push_back(S);
                }
            const uint32_t Count = L.BrushSideCountBytes == 2 ? At<uint16_t>(Img, B + L.BrushSideCount) : At<uint32_t>(Img, B + L.BrushSideCount);
            const int64_t Sides = Count ? Follow(B + L.BrushSides) : -1;
            if (Count && Sides < 0)
            {
                Counters["collision_brush_sides_unread"]++;
                return false;
            }
            for (uint32_t K = 0; K < Count && K < 4096; K++)
            {
                const int64_t S = Sides + (int64_t)K * L.SideSize;
                int64_t Plane = -1;
                if (L.SidePlaneInline)
                    Plane = S + L.SidePlane;
                else if (!L.SidePlaneNumber)
                    Plane = Follow(S + L.SidePlane);
                else if (const uint32_t Number = At<uint32_t>(Img, S + L.SidePlane); T.Planes >= 0 && Number < T.PlaneCount)
                    Plane = T.Planes + (int64_t)Number * 0x14;
                if (Plane < 0 || Plane + 16 > (int64_t)Img.size())
                {
                    Counters["collision_planes_unread"]++;
                    return false;
                }
                BrushSide Out1;
                for (int J = 0; J < 3; J++) Out1.Normal[J] = At<float>(Img, Plane + J * 4);
                Out1.Dist = At<float>(Img, Plane + 12);
                if (L.SideMaterial >= 0)
                    Named(Out1, L.SideMaterialBytes == 2 ? At<uint16_t>(Img, S + L.SideMaterial) : At<uint32_t>(Img, S + L.SideMaterial));
                if (L.SideContents >= 0) Out1.Contents = At<uint32_t>(Img, S + L.SideContents);
                if (L.SideSurface >= 0) Out1.Surface = At<uint32_t>(Img, S + L.SideSurface);
                Out.Sides.push_back(Out1);
            }
            return true;
        }

        // The brushes a leaf brush node reaches: a node with brushes lists their numbers, another
        // points at its two children (offsets from itself)
        std::set<uint32_t> NodeBrushes(const Table& T, int32_t First) const
        {
            std::set<uint32_t> Out;
            if (T.Nodes < 0 || First < 0 || (uint32_t)First >= T.NodeCount) return Out;
            std::vector<uint32_t> Stack = { (uint32_t)First };
            std::set<uint32_t> Seen;
            while (!Stack.empty())
            {
                const uint32_t N = Stack.back();
                Stack.pop_back();
                if (N >= T.NodeCount || !Seen.insert(N).second) continue;
                const int64_t P = T.Nodes + (int64_t)N * L.NodeSize;
                const int32_t Count = (L.NodeWide || L.NodeCountBytes == 4) ? At<int32_t>(Img, P + L.NodeCount) : At<int16_t>(Img, P + L.NodeCount);
                if (Count > 0)
                {
                    const int64_t List = Follow(P + L.NodeBrushes);
                    for (int32_t K = 0; List >= 0 && K < Count && K < 65536; K++)
                        Out.insert((L.NodeWide || L.NodeBrushBytes == 4) ? At<uint32_t>(Img, List + K * 4) : At<uint16_t>(Img, List + K * 2));
                    if (List < 0) Counters["collision_node_brushes_unread"]++;
                    continue;
                }
                for (int K = 0; K < 2; K++)
                    if (const uint32_t Off = L.NodeWide ? At<uint32_t>(Img, P + L.NodeChildren + K * 4) : At<uint16_t>(Img, P + L.NodeChildren + K * 2))
                        Stack.push_back(N + Off);
            }
            return Out;
        }

    private:
        const ClipSource& Src;
        ByteView Img;
        const ClipLayout& L;
        std::map<std::string, size_t>& Counters;
    };
}

bool ReadCollision(Collision& Out, const ClipSource& Src, int64_t ClipMap, const ClipLayout& L, std::map<std::string, size_t>& Counters)
{
    if (!Src.Image || ClipMap < 0) return false;
    const auto& Img = *Src.Image;
    Reader R(Src, L, Counters);
    const int64_t Info = L.InfoPointer ? R.Follow(ClipMap + L.InfoAt) : ClipMap + L.InfoAt;
    if (Info < 0) return false;
    const Reader::Table World = R.Open(Info);
    if (L.Tables >= 0 && World.Tables < 0) Counters["collision_tables_unread"]++;
    auto Triggers = std::move(Out.Triggers);   // read before, kept
    Out = Collision();
    Out.Triggers = std::move(Triggers);
    Out.ClipBrushes = World.BrushCount;
    for (size_t K = 0; K < World.Materials.size(); K++) Out.Materials.emplace_back(World.Materials[K], World.MaterialFlags[K].second);

    // the brush models: their own tables, or the brushes their leaf's node tree reaches
    const uint32_t ModelCount = L.ModelCount >= 0 ? At<uint32_t>(Img, ClipMap + L.ModelCount) : 0;
    const int64_t Models = L.Models >= 0 && ModelCount ? R.Follow(ClipMap + L.Models) : -1;
    std::set<uint32_t> Owned;   // the world's brushes a model owns (node trees)
    if (Models >= 0) Out.Models.resize(ModelCount);
    for (uint32_t M = 1; Models >= 0 && M < ModelCount && M < 65536; M++)
    {
        const int64_t Rec = Models + (int64_t)M * L.ModelSize;
        std::vector<Brush>& Into = Out.Models[M];
        // a ClipInfo of its own with brushes (Black Ops III; Black Ops II has the field, null; CODOL
        // one naming the world's brushes), else the brushes its leaf's node tree reaches in the world's
        const int64_t Own = L.ModelInfo >= 0 ? R.Follow(Rec + L.ModelInfo) : -1;
        if (L.ModelNeedsInfo && Own < 0) continue;
        if (const Reader::Table T = Own >= 0 ? R.Open(Own) : Reader::Table(); T.BrushCount && T.Brushes != World.Brushes)
        {
            Out.ClipBrushes += T.BrushCount;
            for (uint32_t B = 0; B < T.BrushCount; B++)
            {
                Brush Br;
                if (R.ReadBrush(T, B, Br)) Into.push_back(std::move(Br));
                else Counters["collision_brushes_unread"]++;
            }
            continue;
        }
        if (L.ModelLeafBrushNode < 0) continue;
        for (const uint32_t B : R.NodeBrushes(World, At<int32_t>(Img, Rec + L.ModelLeafBrushNode)))
        {
            Brush Br;
            if (!R.ReadBrush(World, B, Br))
            {
                Counters["collision_brushes_unread"]++;
                continue;
            }
            Owned.insert(B);
            Into.push_back(std::move(Br));
        }
    }
    for (uint32_t B = 0; B < World.BrushCount; B++)
    {
        if (Owned.count(B)) continue;
        Brush Br;
        if (R.ReadBrush(World, B, Br)) Out.Brushes.push_back(std::move(Br));
        else Counters["collision_brushes_unread"]++;
    }
    // the collision triangles, in clipMap_t or in the asset holding the brushes' tables
    const int64_t Holder = L.Tables < 0 ? ClipMap : World.Tables >= 0 ? World.Tables - L.TablesAt : -1;
    const uint32_t Vertices = Holder >= 0 && L.VertexCount >= 0 ? At<uint32_t>(Img, Holder + L.VertexCount) : 0;
    const uint32_t Triangles = Holder >= 0 && L.TriangleCount >= 0 ? At<uint32_t>(Img, Holder + L.TriangleCount) : 0;
    const int64_t V = Vertices ? R.Follow(Holder + L.Vertices) : -1, T = Triangles ? R.Follow(Holder + L.Triangles) : -1;
    // each triangle's vertex base, from the partition holding it
    std::vector<uint32_t> Base;
    struct Partition
    {
        uint32_t First, N, Material;   // Material: UINT32_MAX when the partition has none
    };
    std::vector<Partition> Partitions;
    std::vector<uint32_t> Written;   // a stored triangle's number in Out.Triangles (UINT32_MAX: left out)
    if (L.PartitionCount >= 0 && Triangles)
    {
        Base.assign(Triangles, 0);
        const uint32_t Count = At<uint32_t>(Img, Holder + L.PartitionCount);
        int64_t Parts = Count ? R.Follow(Holder + L.Partitions) : -1;
        if (Parts >= 0 && Parts + (int64_t)Count * L.PartitionSize > (int64_t)Img.size()) Parts = -1;
        for (uint32_t P = 0; Parts >= 0 && P < Count; P++)
        {
            const int64_t Rec = Parts + (int64_t)P * L.PartitionSize;
            const uint32_t First = At<uint32_t>(Img, Rec + L.PartitionFirstTri), N = At<uint8_t>(Img, Rec + L.PartitionTris);
            const uint32_t Segment = At<uint8_t>(Img, Rec + L.PartitionSegment) * (uint32_t)L.SegmentVertices;
            for (uint32_t K = First; K < First + N && K < Triangles; K++) Base[K] = Segment;
            Partitions.push_back({ First, N, L.PartitionMaterial >= 0 ? At<uint32_t>(Img, Rec + L.PartitionMaterial) : UINT32_MAX });
        }
        if (Parts < 0) Counters["collision_partitions_unread"]++;
    }
    // the AABB tree (CollisionAabbTree): its leaves name a partition and its material (below)
    const uint32_t AabbNodes = L.AabbCount >= 0 && Holder >= 0 ? At<uint32_t>(Img, Holder + L.AabbCount) : 0;
    int64_t Aabbs = AabbNodes ? R.Follow(Holder + L.Aabbs) : -1;
    if (Aabbs >= 0 && Aabbs + (int64_t)AabbNodes * L.AabbSize > (int64_t)Img.size()) Aabbs = -1;
    // the triangles a brush model's leaf reaches through its AABB roots (cLeaf_t firstCollAabbIndex,
    // collAabbCount), in the model's own space: its entity's, not the world's (BO1's mp_nuked's
    // heli_height_lock, *203: a bowl over the whole map 1505 units below where it stands, written
    // into the world an invisible hill round the play area). Each stored triangle's model (0: the
    // world's) and its leaf's material.
    std::vector<uint32_t> ModelOf, LeafMaterial;
    if (L.ModelAabbs >= 0 && Models >= 0 && Aabbs >= 0 && !Partitions.empty())
    {
        ModelOf.assign(Triangles, 0);
        LeafMaterial.assign(Triangles, Collision::NoMaterial);
        for (uint32_t M = 1; M < ModelCount && M < 65536; M++)
        {
            const int64_t Rec = Models + (int64_t)M * L.ModelSize;
            const uint32_t First = At<uint16_t>(Img, Rec + L.ModelAabbs), N = At<uint16_t>(Img, Rec + L.ModelAabbs + 2);
            std::vector<uint32_t> Stack;
            for (uint32_t K = 0; K < N; K++) Stack.push_back(First + K);
            std::set<uint32_t> Seen;
            while (!Stack.empty())
            {
                const uint32_t K = Stack.back();
                Stack.pop_back();
                if (K >= AabbNodes || !Seen.insert(K).second) continue;
                const int64_t Node = Aabbs + (int64_t)K * L.AabbSize;
                const uint32_t Children = At<uint16_t>(Img, Node + L.AabbChildCount), U = At<uint32_t>(Img, Node + L.AabbPartition);
                if (Children)
                {
                    for (uint32_t C = 0; C < Children; C++) Stack.push_back(U + C);
                    continue;
                }
                if (U >= Partitions.size()) continue;
                for (uint32_t Stored = Partitions[U].First; Stored < Partitions[U].First + Partitions[U].N && Stored < Triangles; Stored++)
                {
                    ModelOf[Stored] = M;
                    LeafMaterial[Stored] = At<uint16_t>(Img, Node + L.AabbMaterial);
                }
            }
        }
    }
    if (V >= 0 && T >= 0 && V + (int64_t)Vertices * 12 <= (int64_t)Img.size() &&
        T + (int64_t)Triangles * 3 * L.TriangleIndexBytes <= (int64_t)Img.size())
    {
        Out.Vertices.resize((size_t)Vertices * 3);
        Written.assign(Triangles, UINT32_MAX);
        for (size_t K = 0; K < Out.Vertices.size(); K++) Out.Vertices[K] = At<float>(Img, V + (int64_t)K * 4);
        // stored clockwise seen from outside, as Direct3D draws (every game measured, CoD4 to
        // Black Ops 4: against the drawn surface's normals where one lies on the triangle):
        // turned round to face out along (b - a) x (c - a)
        for (uint32_t K = 0; K < Triangles; K++)
        {
            uint32_t I[3];
            for (uint32_t J = 0; J < 3; J++)
                I[J] = (L.TriangleIndexBytes == 2 ? At<uint16_t>(Img, T + (K * 3 + J) * 2) : At<uint32_t>(Img, T + (K * 3 + J) * 4)) +
                       (Base.empty() ? 0 : Base[K]);
            if (I[0] >= Vertices || I[1] >= Vertices || I[2] >= Vertices)
            {
                // that one left out, the others kept (AW's mp_blackbox: one in 1.2 million)
                Counters["collision_triangles_unread"]++;
                continue;
            }
            if (!ModelOf.empty() && ModelOf[K] != 0 && ModelOf[K] < Out.Models.size())
            {
                // a brush model's: a prism among its brushes, as the world's triangles are written
                // (the proof: within the model's bounds, cmodel_t mins 0 and maxs 0xc)
                std::array<double, 3> P[3];
                for (int J = 0; J < 3; J++)
                    for (int A = 0; A < 3; A++) P[J][A] = Out.Vertices[(size_t)I[J ? 3 - J : 0] * 3 + A];
                const int64_t Rec = Models + (int64_t)ModelOf[K] * L.ModelSize;
                bool Inside = true;
                for (int J = 0; J < 3; J++)
                    for (int A = 0; A < 3; A++)
                        Inside = Inside && P[J][A] >= At<float>(Img, Rec + A * 4) - 1.0 && P[J][A] <= At<float>(Img, Rec + 0xc + A * 4) + 1.0;
                if (!Inside) Counters["collision_model_triangles_outside_bounds"]++;
                Brush Br;
                if (!TrianglePrism(P, ModelPrismDepth, Br))
                {
                    Counters["collision_model_triangles_dropped"]++;
                    continue;
                }
                if (const uint32_t Mat = LeafMaterial[K]; Mat < Out.Materials.size())
                {
                    Br.Contents = Out.Materials[Mat].second;
                    for (BrushSide& S : Br.Sides) S.Material = Out.Materials[Mat].first, S.Contents = Br.Contents;
                }
                Out.Models[ModelOf[K]].push_back(std::move(Br));
                Counters["collision_model_triangles"]++;
                continue;
            }
            Written[K] = (uint32_t)(Out.Triangles.size() / 3);
            Out.Triangles.insert(Out.Triangles.end(), { I[0], I[2], I[1] });
        }
    }
    else if (Triangles)
        Counters["collision_triangles_unread"] += Triangles;
    // each triangle's material: its partition's own (WWII), else the one the AABB tree's leaf naming
    // its partition (a run of triangles) collides with (a drawn terrain's, solid; a detail mesh's
    // that stops only bullets, weaponClip)
    const size_t Read = Out.Triangles.size() / 3;
    if (L.PartitionMaterial >= 0 && !Partitions.empty() && Read)
    {
        Out.TriangleMaterials.assign(Read, Collision::NoMaterial);
        for (const Partition& P : Partitions)
        {
            if (P.Material >= Out.Materials.size())
            {
                Counters["collision_partitions_material_unread"]++;
                continue;
            }
            for (uint32_t Stored = P.First; Stored < P.First + P.N && Stored < Written.size(); Stored++)
                if (Written[Stored] < Read) Out.TriangleMaterials[Written[Stored]] = P.Material;
        }
    }
    if (L.AabbCount >= 0 && (!Partitions.empty() || L.AabbFirstTri >= 0) && Read)
    {
        const int64_t Tree = L.AabbInInfo ? Info : Holder;
        const uint32_t Count = At<uint32_t>(Img, Tree + L.AabbCount);
        int64_t Nodes = Count ? R.Follow(Tree + L.Aabbs) : -1;
        if (Nodes >= 0 && Nodes + (int64_t)Count * L.AabbSize > (int64_t)Img.size()) Nodes = -1;
        if (Nodes >= 0)
        {
            const bool FromLeaves = L.PartitionMaterial < 0;
            if (FromLeaves) Out.TriangleMaterials.assign(Read, Collision::NoMaterial);
            for (uint32_t K = 0; K < Count; K++)
            {
                const int64_t Rec = Nodes + (int64_t)K * L.AabbSize;
                if (L.AabbChildCount >= 0 &&
                    (L.AabbChildCountBytes == 1 ? At<uint8_t>(Img, Rec + L.AabbChildCount) : At<uint16_t>(Img, Rec + L.AabbChildCount)) != 0)
                    continue;
                if (L.AabbZone >= 0 && At<uint8_t>(Img, Rec + L.AabbZone) != 0)
                {
                    Counters["collision_aabb_leaves_other_zone"]++;
                    continue;
                }
                const uint32_t Material = FromLeaves ? At<uint16_t>(Img, Rec + L.AabbMaterial) : 0;
                const uint32_t P = L.AabbFirstTri >= 0 ? 0 : At<uint32_t>(Img, Rec + L.AabbPartition);
                if ((L.AabbFirstTri < 0 && P >= Partitions.size()) || Material >= Out.Materials.size())
                {
                    Counters["collision_aabb_leaves_unread"]++;
                    continue;
                }
                const Partition Part = L.AabbFirstTri >= 0
                                         ? Partition{ At<uint32_t>(Img, Rec + L.AabbFirstTri), At<uint8_t>(Img, Rec + L.AabbTriCount), UINT32_MAX }
                                         : Partitions[P];
                // the proof of the layout: a leaf's box (centre, half size) holds its triangles
                float Mid[3], Half[3];
                for (int A = 0; A < 3; A++)
                {
                    const float U = At<float>(Img, Rec + L.AabbOrigin + A * 4), W = At<float>(Img, Rec + L.AabbHalfSize + A * 4);
                    Mid[A] = L.AabbMinMax ? (U + W) / 2 : U;
                    Half[A] = L.AabbMinMax ? (W - U) / 2 : W;
                }
                bool Holds = true;
                for (uint32_t Stored = Part.First; Stored < Part.First + Part.N && Stored < Written.size(); Stored++)
                {
                    const uint32_t T = Written[Stored];
                    if (T >= Read) continue;
                    if (FromLeaves) Out.TriangleMaterials[T] = Material;
                    for (int J = 0; J < 3; J++)
                        for (int A = 0; A < 3; A++)
                            Holds = Holds && std::fabs(Out.Vertices[(size_t)Out.Triangles[(size_t)T * 3 + J] * 3 + A] - Mid[A]) <= Half[A] + 1.0f;
                }
                Counters["collision_aabb_leaves"]++;
                if (!Holds) Counters["collision_aabb_leaves_outside_box"]++;
            }
        }
        else
            Counters["collision_aabb_unread"]++;
    }
    if (!Out.TriangleMaterials.empty())
        Counters["collision_triangles_without_material"] +=
            (size_t)std::count(Out.TriangleMaterials.begin(), Out.TriangleMaterials.end(), Collision::NoMaterial);
    return !Out.Empty();
}
}

namespace Maps
{
bool ReadTriggers(Collision& Out, const ClipSource& Src, int64_t Triggers, const TriggerLayout& L)
{
    if (!Src.Image || Triggers < 0) return false;
    const auto& Img = *Src.Image;
    const int P = Src.PointerSize;
    const auto Follow = [&](int64_t Pos) {
        const uint64_t A = P == 4 ? (uint64_t)At<uint32_t>(Img, Pos) : At<uint64_t>(Img, Pos);
        return A ? Src.Off(A) : -1;
    };
    const uint32_t Count = At<uint32_t>(Img, Triggers), HullCount = At<uint32_t>(Img, Triggers + 2 * P),
                   SlabCount = At<uint32_t>(Img, Triggers + 4 * P);
    const int64_t Models = Follow(Triggers + P), Hulls = Follow(Triggers + 3 * P), Slabs = Follow(Triggers + 5 * P);
    if (Models < 0 || Hulls < 0 || Count > 65536) return false;
    Out.Triggers.assign(Count, {});
    for (uint32_t M = 0; M < Count; M++)
    {
        const int64_t Model = Models + (int64_t)M * L.ModelSize;
        const uint16_t N = At<uint16_t>(Img, Model + L.ModelHullCount), First = At<uint16_t>(Img, Model + L.ModelFirstHull);
        for (uint32_t H = First; H < (uint32_t)First + N && H < HullCount; H++)
        {
            const int64_t Hull = Hulls + (int64_t)H * L.HullSize;
            Brush B;
            B.Contents = At<uint32_t>(Img, Hull + L.HullContents);
            for (int Side = 0; Side < 2; Side++)
                for (int K = 0; K < 3; K++)
                {
                    const float Mid = At<float>(Img, Hull + K * 4), Half = At<float>(Img, Hull + 12 + K * 4);
                    BrushSide S;
                    S.Normal[0] = S.Normal[1] = S.Normal[2] = 0;
                    S.Normal[K] = Side ? 1.0f : -1.0f;
                    S.Dist = Side ? Mid + Half : -(Mid - Half);
                    B.Sides.push_back(S);
                }
            const uint16_t SlabN = At<uint16_t>(Img, Hull + L.HullSlabCount), SlabFirst = At<uint16_t>(Img, Hull + L.HullFirstSlab);
            for (uint32_t S = SlabFirst; Slabs >= 0 && S < (uint32_t)SlabFirst + SlabN && S < SlabCount; S++)
            {
                const int64_t Slab = Slabs + (int64_t)S * 0x14;
                const float Mid = At<float>(Img, Slab + 12), Half = At<float>(Img, Slab + 16);
                BrushSide Up, Down;
                for (int K = 0; K < 3; K++) Up.Normal[K] = At<float>(Img, Slab + K * 4), Down.Normal[K] = -Up.Normal[K];
                Up.Dist = Mid + Half;
                Down.Dist = -(Mid - Half);
                B.Sides.push_back(Up);
                B.Sides.push_back(Down);
            }
            Out.Triggers[M].push_back(std::move(B));
        }
    }
    return true;
}
}
