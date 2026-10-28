#include "stdafx.h"

#include "ClipMap.h"

#include "ExportNaming.h"
#include "MapLive.h"
#include "MapRadiantWriter.h"

#include <array>
#include <vector>

// The layout, field by field, measured on the zones' bytes.
namespace ColdWar
{
namespace
{
    using ExportNaming::At;

    // clip_map: partitions (count, array), the material table (u64 per material: surface flags,
    // content flags; count at 0x18)
    constexpr int64_t Partitions = 0x30, PartitionCount = 0x1b4, PartitionSize = 0xb8, MaterialTable = 0x38;
    constexpr int64_t MaterialFlags = 0, MaterialCount = 0x18;
    // a partition: its counts, then its arrays
    constexpr int64_t PlaneCount = 0x18, BrushCount = 0x20, VertexCount = 0x28, IndexBytes = 0x2c, ChunkCount = 0x30, BoxCount = 0x38;
    constexpr int64_t Planes = 0x40, SideMaterials = 0x50, FirstPlane = 0x68, FirstSide = 0x78, BrushBounds = 0x80, Vertices = 0x88,
                      Indices = 0x90, Chunks = 0xa8, Boxes = 0xb0;
    constexpr int64_t BrushBoundsSize = 28, ChunkSize = 32, BoxSize = 44;
    constexpr double PrismDepth = 2.0;   // a brush model's triangle as a brush, as the Radiant export makes the world's

    // The game's surface types (bits 20-26 of a material's surface flags) as a side's material
    // name, the clip map keeping none; the climbing ones as Black Ops III's tool materials
    const char* SurfaceName(uint32_t Type)
    {
        static const char* const Names[] = { "", "bark", "brick", "carpet", "cloth", "concrete", "dirt", "flesh", "foliage", "glass",
                                             "grass", "gravel", "ice", "metal", "mud", "paper", "plaster", "rock", "sand", "snow",
                                             "water", "wood", "asphalt", "ceramic", "plastic", "rubber", "cushion", "fruit",
                                             "paintedmetal", "player", "tallgrass", "riotshield", "metalthin", "metalhollow",
                                             "metalcatwalk", "metalcar", "glasscar", "glassbulletproof", "watershallow", "bodyarmor",
                                             "ladder", "mantle_on", "mantle_over", "climbwall", "climbpipe" };
        return Type < std::size(Names) ? Names[Type] : "";
    }

    class Reader
    {
    public:
        Reader(const Maps::ClipSource& Src, std::map<std::string, size_t>& Counters) : Src(Src), Img(*Src.Image), Counters(Counters) {}

        int64_t Follow(int64_t Pos) const
        {
            const uint64_t P = At<uint64_t>(Img, Pos);
            return P ? Src.Off(P) : -1;
        }
        // an array of Count records of Size at the pointer at Pos, -1 when it is not all in the zone
        int64_t Array(int64_t Pos, uint64_t Count, int64_t Size) const
        {
            if (Count == 0) return -1;
            const int64_t A = Follow(Pos);
            return A >= 0 && A + (int64_t)Count * Size <= (int64_t)Img.size() ? A : -1;
        }

        void Materials(int64_t Clip, Maps::Collision& Out)
        {
            const int64_t Table = Follow(Clip + MaterialTable);
            if (Table < 0) return;
            const uint32_t Count = At<uint32_t>(Img, Table + MaterialCount);
            const int64_t Flags = Array(Table + MaterialFlags, Count, 8);
            for (uint32_t K = 0; Flags >= 0 && K < Count; K++)
            {
                const uint32_t Surface = At<uint32_t>(Img, Flags + K * 8), Contents = LineageContents(At<uint32_t>(Img, Flags + K * 8 + 4));
                Mats.push_back({ SurfaceName((Surface >> 20) & 0x7f), Surface, Contents });
                Out.Materials.emplace_back(Mats.back().Name, Contents);
            }
        }

        // One partition's brushes, boxes and triangles; the world's triangles kept as triangles,
        // a brush model's made brushes
        void Partition(int64_t P, bool World, Maps::Collision& Out, std::vector<Maps::Brush>& Into)
        {
            Brushes(P, Out, Into);
            OrientedBoxes(P, Out, Into);
            Triangles(P, World, Out, Into);
        }

    private:
        struct Mat
        {
            std::string Name;
            uint32_t Surface = 0, Contents = 0;
        };

        // A brush: its bounds and flags (content flags in bits 0-25, its plane count above), its
        // first plane and first side; its sides' materials 10 bits each, six to a u64: the six
        // axial sides (-x, -y, -z, +x, +y, +z) then its planes
        void Brushes(int64_t P, Maps::Collision& Out, std::vector<Maps::Brush>& Into)
        {
            const uint32_t PlaneN = At<uint32_t>(Img, P + PlaneCount), BrushN = At<uint32_t>(Img, P + BrushCount);
            const uint64_t SideN = (uint64_t)PlaneN + 6ull * BrushN;
            const int64_t Pl = Array(P + Planes, PlaneN, 16), Sm = Array(P + SideMaterials, (SideN + 5) / 6, 8);
            const int64_t Fp = Array(P + FirstPlane, BrushN, 4), Fs = Array(P + FirstSide, BrushN, 4), Bb = Array(P + BrushBounds, BrushN, BrushBoundsSize);
            Out.ClipBrushes += BrushN;
            if (BrushN && (Fp < 0 || Fs < 0 || Bb < 0 || Sm < 0 || (PlaneN && Pl < 0)))
            {
                Counters["collision_brushes_unread"] += BrushN;
                return;
            }
            for (uint32_t B = 0; B < BrushN; B++)
            {
                const int64_t R = Bb + (int64_t)B * BrushBoundsSize;
                const uint32_t Flags = At<uint32_t>(Img, R + 24), N = Flags >> 26, First = At<uint32_t>(Img, Fp + B * 4),
                               Side = At<uint32_t>(Img, Fs + B * 4);
                if ((uint64_t)First + N > PlaneN || (uint64_t)Side + 6 + N > SideN)
                {
                    Counters["collision_brushes_unread"]++;
                    continue;
                }
                Maps::Brush Br;
                Br.Contents = LineageContents(Flags & 0x3ffffff);
                for (uint32_t K = 0; K < 6 + N; K++)
                {
                    Maps::BrushSide S;
                    if (K < 6)
                    {
                        const int Axis = K % 3;
                        S.Normal[0] = S.Normal[1] = S.Normal[2] = 0;
                        S.Normal[Axis] = K < 3 ? -1.0f : 1.0f;
                        S.Dist = K < 3 ? -At<float>(Img, R + Axis * 4) : At<float>(Img, R + 12 + Axis * 4);
                    }
                    else
                    {
                        const int64_t Plane = Pl + (int64_t)(First + K - 6) * 16;
                        for (int J = 0; J < 3; J++) S.Normal[J] = At<float>(Img, Plane + J * 4);
                        S.Dist = At<float>(Img, Plane + 12);
                    }
                    const uint64_t I = (uint64_t)Side + K;
                    Material(S, (uint32_t)((At<uint64_t>(Img, Sm + (int64_t)(I / 6) * 8) >> (10 * (I % 6))) & 0x3ff));
                    Br.Sides.push_back(S);
                }
                Into.push_back(std::move(Br));
            }
        }

        // An oriented box (quaternion x y z w, centre, half sizes, the material in the low 16 bits
        // of the last u32): a brush of six sides
        void OrientedBoxes(int64_t P, Maps::Collision& Out, std::vector<Maps::Brush>& Into)
        {
            const uint32_t BoxN = At<uint32_t>(Img, P + BoxCount);
            const int64_t Bx = Array(P + Boxes, BoxN, BoxSize);
            Out.ClipBrushes += BoxN;
            if (BoxN && Bx < 0) Counters["collision_boxes_unread"] += BoxN;
            for (uint32_t K = 0; Bx >= 0 && K < BoxN; K++)
            {
                const int64_t R = Bx + (int64_t)K * BoxSize;
                float Q[4], Axis[9];
                for (int J = 0; J < 4; J++) Q[J] = At<float>(Img, R + J * 4);
                Maps::AxisFromQuat(Q, Axis);
                Maps::Brush Br;
                const uint32_t M = At<uint32_t>(Img, R + 40) & 0xffff;
                Br.Contents = M < Mats.size() ? Mats[M].Contents : 0;
                for (int Side = 0; Side < 2; Side++)
                    for (int A = 0; A < 3; A++)
                    {
                        Maps::BrushSide S;
                        float Centre = 0;
                        for (int J = 0; J < 3; J++)
                        {
                            S.Normal[J] = Side ? Axis[A * 3 + J] : -Axis[A * 3 + J];
                            Centre += S.Normal[J] * At<float>(Img, R + 16 + J * 4);
                        }
                        S.Dist = Centre + At<float>(Img, R + 28 + A * 4);
                        Material(S, M);
                        Br.Sides.push_back(S);
                    }
                Into.push_back(std::move(Br));
                Counters["collision_boxes"]++;
            }
        }

        // The triangles, in chunks (bounds, then a u64: base vertex bits 0-22, first triangle
        // 23-48, count 49-53, material 54-63); an index is a byte added to the chunk's base vertex.
        // Stored clockwise seen from outside, as the older clip maps (mp_sm_central: 2 269 of 2 525
        // collision triangles lying on a drawn one face against its normal): turned round to face
        // out, as Maps::Collision and TrianglePrism take them
        void Triangles(int64_t P, bool World, Maps::Collision& Out, std::vector<Maps::Brush>& Into)
        {
            const uint32_t VertexN = At<uint32_t>(Img, P + VertexCount), IndexN = At<uint32_t>(Img, P + IndexBytes),
                           ChunkN = At<uint32_t>(Img, P + ChunkCount);
            const int64_t V = Array(P + Vertices, VertexN, 12), Ix = Array(P + Indices, IndexN, 1), Ch = Array(P + Chunks, ChunkN, ChunkSize);
            if (ChunkN == 0) return;
            if (V < 0 || Ix < 0 || Ch < 0)
            {
                Counters["collision_triangles_unread"] += IndexN / 3;
                return;
            }
            const uint32_t Base = (uint32_t)(Out.Vertices.size() / 3);
            if (World)
            {
                Out.Vertices.reserve(Out.Vertices.size() + (size_t)VertexN * 3);
                for (uint32_t K = 0; K < VertexN * 3; K++) Out.Vertices.push_back(At<float>(Img, V + (int64_t)K * 4));
            }
            for (uint32_t C = 0; C < ChunkN; C++)
            {
                const uint64_t W = At<uint64_t>(Img, Ch + (int64_t)C * ChunkSize + 24);
                const uint32_t VertexBase = (uint32_t)(W & 0x7fffff), First = (uint32_t)((W >> 23) & 0x3ffffff),
                               Count = (uint32_t)((W >> 49) & 0x1f), M = (uint32_t)(W >> 54);
                for (uint32_t T = First; T < First + Count; T++)
                {
                    uint32_t Tri[3];
                    bool Ok = (uint64_t)T * 3 + 3 <= IndexN;
                    for (int J = 0; Ok && J < 3; J++)
                    {
                        Tri[J] = VertexBase + Img[(size_t)(Ix + (int64_t)T * 3 + J)];
                        Ok = Tri[J] < VertexN;
                    }
                    if (!Ok)
                    {
                        Counters["collision_triangles_unread"]++;
                        continue;
                    }
                    if (World)
                    {
                        Out.Triangles.insert(Out.Triangles.end(), { Base + Tri[0], Base + Tri[2], Base + Tri[1] });
                        continue;
                    }
                    std::array<double, 3> Pt[3];
                    for (int J = 0; J < 3; J++)
                        for (int A = 0; A < 3; A++) Pt[J][A] = At<float>(Img, V + (int64_t)Tri[J ? 3 - J : 0] * 12 + A * 4);
                    Maps::Brush Br;
                    if (!Maps::TrianglePrism(Pt, PrismDepth, Br))
                    {
                        Counters["collision_model_triangles_dropped"]++;
                        continue;
                    }
                    Br.Contents = M < Mats.size() ? Mats[M].Contents : 0;
                    for (auto& S : Br.Sides) Material(S, M);
                    Into.push_back(std::move(Br));
                    Counters["collision_model_triangles"]++;
                }
            }
        }

        void Material(Maps::BrushSide& S, uint32_t M)
        {
            if (M >= Mats.size())
            {
                Counters["collision_side_materials_unknown"]++;
                return;
            }
            S.Material = Mats[M].Name;
            S.Surface = Mats[M].Surface;
            S.Contents = Mats[M].Contents;
        }

        const Maps::ClipSource& Src;
        ByteView Img;
        std::map<std::string, size_t>& Counters;
        std::vector<Mat> Mats;
    };
}

uint32_t LineageContents(uint32_t Contents)
{
    constexpr uint32_t Trigger = 0x2000000, DepthPrime = 0x40000000, Transparent = 0x80000000, LineageTrigger = 0x40000000;
    return (Contents & ~(Trigger | DepthPrime | Transparent)) | (Contents & Trigger ? LineageTrigger : 0);
}

bool ReadClipMap(Maps::Collision& Out, const Maps::ClipSource& Src, int64_t Clip, std::map<std::string, size_t>& Counters)
{
    if (!Src.Image || Clip < 0) return false;
    Reader R(Src, Counters);
    auto Triggers = std::move(Out.Triggers);
    Out = Maps::Collision();
    Out.Triggers = std::move(Triggers);
    R.Materials(Clip, Out);
    const uint32_t Count = At<uint32_t>(*Src.Image, Clip + PartitionCount);
    const int64_t Parts = R.Array(Clip + Partitions, Count, PartitionSize);
    if (Parts < 0) return false;
    if (Count > 1) Out.Models.resize(Count);
    for (uint32_t K = 0; K < Count; K++)
        R.Partition(Parts + (int64_t)K * PartitionSize, K == 0, Out, K == 0 ? Out.Brushes : Out.Models[K]);
    if (Out.Brushes.empty() && Out.Triangles.empty()) Counters["collision_world_empty"]++;
    return !Out.Empty();
}
}
