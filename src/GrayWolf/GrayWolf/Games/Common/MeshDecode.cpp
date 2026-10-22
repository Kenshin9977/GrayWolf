#include "stdafx.h"

#include "Games/Common/MeshDecode.h"

#include "CoDAssets.h"
#include "CoDQTangent.h"
#include "HalfFloats.h"
#include "WraithModel.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <intrin.h>
#include <limits>
#include <numeric>
#include <unordered_map>

namespace MeshDecode
{
namespace
{
    template <class T>
    T At(const uint8_t* P)
    {
        T V;
        std::memcpy(&V, P, sizeof(T));
        return V;
    }

    uint32_t PositionBytes(PositionCodec C) { return C == PositionCodec::Packed21 ? 8 : 12; }

    // a stream of Count elements of Bytes each inside Size
    bool Inside(const Stream& S, uint32_t Count, uint32_t Bytes, uint64_t Size)
    {
        if (S.Offset == NoStream) return false;
        if (Count == 0) return true;
        return S.Offset + (uint64_t)(Count - 1) * S.Stride + Bytes <= Size;
    }

    // up (0, 0, 1) rotated by Q (x, y, z, w)
    Vector3 TransformNormal(Quaternion Q, Vector3 Up)
    {
        const Vector3 A(Q.Y * Up.Z - Q.Z * Up.Y + Up.X * Q.W, Q.Z * Up.X - Q.X * Up.Z + Up.Y * Q.W, Q.X * Up.Y - Q.Y * Up.X + Up.Z * Q.W);
        const Vector3 B(Q.Y * A.Z - Q.Z * A.Y, Q.Z * A.X - Q.X * A.Z, Q.X * A.Y - Q.Y * A.X);
        return Vector3(Up.X + B.X + B.X, Up.Y + B.Y + B.Y, Up.Z + B.Z + B.Z);
    }

    template <NormalCodec N>
    Vector3 Normal(uint32_t Packed)
    {
        if constexpr (N == NormalCodec::QTangent)
        {
            CoDQTangent Q;
            std::memcpy(&Q, &Packed, 4);
            return Q.Unpack(nullptr, nullptr);
        }
        else if constexpr (N == NormalCodec::PackedQuat) return UnpackPackedQuat(Packed);
        else if constexpr (N == NormalCodec::Signed10) return UnpackSigned10((int32_t)Packed);
        else if constexpr (N == NormalCodec::UnitA) return CoDXModelTranslator::UnpackNormalA(Packed);
        else if constexpr (N == NormalCodec::UnitB) return CoDXModelTranslator::UnpackNormalB(Packed);
        else return CoDXModelTranslator::UnpackNormalC(Packed);
    }

    // The vertex loop of one codec combination
    template <PositionCodec P, NormalCodec N, UvCodec U>
    void Vertices(const uint8_t* Buf, const SubmeshStreams& S, bool Colors, const WeightsData* Weights, WraithSubmesh& Out)
    {
        for (uint32_t I = 0; I < S.VertexCount; I++)
        {
            auto& V = Out.AddVertex();
            const uint8_t* Pos = Buf + S.Positions.Offset + (uint64_t)I * S.Positions.Stride;
            if constexpr (P == PositionCodec::Packed21)
                V.Position = Unpack21BitVertex(At<uint64_t>(Pos), S.Scale, S.Offset);
            else
                V.Position = Vector3(At<float>(Pos), At<float>(Pos + 4), At<float>(Pos + 8));
            if constexpr (N != NormalCodec::None)
                V.Normal = Normal<N>(At<uint32_t>(Buf + S.Normals.Offset + (uint64_t)I * S.Normals.Stride));
            if constexpr (U != UvCodec::None)
            {
                const uint8_t* Uv = Buf + S.Uvs.Offset + (uint64_t)I * S.Uvs.Stride;
                const uint16_t First = At<uint16_t>(Uv), Second = At<uint16_t>(Uv + 2);
                if constexpr (U == UvCodec::Half2)
                    V.AddUVLayer(HalfFloats::ToFloat(First), HalfFloats::ToFloat(Second));
                else
                    V.AddUVLayer(HalfFloats::ToFloat(Second), HalfFloats::ToFloat(First));
            }
            if (Colors)
                std::memcpy(V.Color, Buf + S.Colors.Offset + (uint64_t)I * S.Colors.Stride, 4);
            else
                V.Color[0] = V.Color[1] = V.Color[2] = V.Color[3] = 255;
            if (Weights == nullptr)
                V.AddVertexWeight(0, 1.0f);
            else
                for (uint32_t K = 0; K < Weights[I].WeightCount; K++) V.AddVertexWeight(Weights[I].BoneValues[K], Weights[I].WeightValues[K]);
        }
    }

    using VertexLoop = void (*)(const uint8_t*, const SubmeshStreams&, bool, const WeightsData*, WraithSubmesh&);

    template <PositionCodec P, NormalCodec N>
    VertexLoop PickUv(UvCodec U)
    {
        switch (U)
        {
        case UvCodec::Half2: return &Vertices<P, N, UvCodec::Half2>;
        case UvCodec::Half2Swapped: return &Vertices<P, N, UvCodec::Half2Swapped>;
        default: return &Vertices<P, N, UvCodec::None>;
        }
    }

    template <PositionCodec P>
    VertexLoop PickNormal(NormalCodec N, UvCodec U)
    {
        switch (N)
        {
        case NormalCodec::QTangent: return PickUv<P, NormalCodec::QTangent>(U);
        case NormalCodec::PackedQuat: return PickUv<P, NormalCodec::PackedQuat>(U);
        case NormalCodec::Signed10: return PickUv<P, NormalCodec::Signed10>(U);
        case NormalCodec::UnitA: return PickUv<P, NormalCodec::UnitA>(U);
        case NormalCodec::UnitB: return PickUv<P, NormalCodec::UnitB>(U);
        case NormalCodec::UnitC: return PickUv<P, NormalCodec::UnitC>(U);
        default: return PickUv<P, NormalCodec::None>(U);
        }
    }

    VertexLoop Pick(const Encoding& E)
    {
        return E.Position == PositionCodec::Packed21 ? PickNormal<PositionCodec::Packed21>(E.Normal, E.Uv)
                                                     : PickNormal<PositionCodec::Float3>(E.Normal, E.Uv);
    }

    // A corner's index in its table's run (C2M's FindFaceIndexNew): the triangle's corners packed in
    // Bits + 2 * Delta bits (the first absolute, the other two signed deltas from it), or, with flag
    // 0x10, every corner in Bits bits
    uint8_t DeltaCorner(const uint8_t* Buf, size_t Size, size_t Packed, uint32_t Index, uint8_t BitsValue, uint8_t Delta, Edge End)
    {
        unsigned long High = 0;
        const uint8_t Bits = _BitScanReverse(&High, BitsValue) ? (uint8_t)(High + 1) : 0;
        const auto U64At = [&](size_t At) -> uint64_t {
            uint64_t V = 0;
            if (End == Edge::Zero && At + 8 > Size) return V;
            if (At < Size) std::memcpy(&V, Buf + At, std::min<size_t>(8, Size - At));
            return V;
        };
        if (Delta & 0x10)
        {
            const uint64_t Bit = (uint64_t)Index * Bits;
            return (uint8_t)((U64At(Packed + (size_t)(Bit >> 3)) >> (Bit & 7)) & ((1ull << Bits) - 1));
        }
        const uint32_t Width = Bits + 2u * Delta;
        const uint64_t Bit = (uint64_t)(Index / 3) * Width;
        const uint64_t Word = U64At(Packed + (size_t)(Bit >> 3)) >> (Bit & 7);
        uint8_t Value = (uint8_t)(Word & ((1ull << Bits) - 1));
        const uint32_t Corner = Index % 3;
        if (Corner != 0 && Delta != 0)
        {
            const uint32_t Shift = Corner == 1 ? Bits : Bits + Delta;
            uint32_t D = (uint32_t)((Word >> Shift) & ((1ull << Delta) - 1));
            if (D & (1u << (Delta - 1))) D = 0u - (D & ((1u << (Delta - 1)) - 1));
            Value = (uint8_t)(Value + D);
        }
        return Value;
    }

    // A corner's index in its 40-byte table's run: Index * the table's width, in bits
    uint8_t Table40Corner(const uint8_t* Packed, size_t Index, size_t Bits)
    {
        unsigned long BitIndex;
        if (!_BitScanReverse64(&BitIndex, Bits))
            BitIndex = 64;
        else
            BitIndex ^= 0x3F;
        const uint16_t Offset = (uint16_t)(Index * static_cast<uint8_t>(64 - BitIndex));
        const uint8_t BitCount = static_cast<uint8_t>(64 - BitIndex);
        const uint8_t* Byte = Packed + (Offset >> 3);
        const uint8_t BitOffset = Offset & 7;
        if (BitOffset == 0)
            return *Byte & ((1 << BitCount) - 1);
        if (8 - BitOffset < BitCount)
            return (*Byte >> BitOffset) & ((1 << (8 - BitOffset)) - 1) |
                   ((Byte[1] & static_cast<uint8_t>((1 << (64 - BitIndex - (8 - BitOffset))) - 1)) << (8 - BitOffset));
        return (*Byte >> BitOffset) & ((1 << BitCount) - 1);
    }
}

Vector3 Unpack21BitVertex(uint64_t Packed, float Scale, const Vector3& Offset)
{
    return Vector3(
        (((((Packed >> 00) & 0x1FFFFF) * ((1.0f / 0x1FFFFF) * 2.0f)) - 1.0f) * Scale) + Offset.X,
        (((((Packed >> 21) & 0x1FFFFF) * ((1.0f / 0x1FFFFF) * 2.0f)) - 1.0f) * Scale) + Offset.Y,
        (((((Packed >> 42) & 0x1FFFFF) * ((1.0f / 0x1FFFFF) * 2.0f)) - 1.0f) * Scale) + Offset.Z);
}

Vector3 UnpackPackedQuat(uint32_t Packed)
{
    // the quaternion's three smallest components (10, 10, 9 bits), the dropped one's index in the
    // top two bits, rebuilt from the unit length
    const auto Up = Vector3(0, 0, 1.0f);
    const auto X = ((((Packed >> 00) & 0x3FF) / 511.5f) - 1.0f) / 1.4142135f;
    const auto Y = ((((Packed >> 10) & 0x3FF) / 511.5f) - 1.0f) / 1.4142135f;
    const auto Z = ((((Packed >> 20) & 0x1FF) / 255.5f) - 1.0f) / 1.4142135f;
    const auto W = sqrtf(1 - X * X - Y * Y - Z * Z);
    switch (Packed >> 30)
    {
    case 0: return TransformNormal(Quaternion(W, X, Y, Z), Up);
    case 1: return TransformNormal(Quaternion(X, W, Y, Z), Up);
    case 2: return TransformNormal(Quaternion(X, Y, W, Z), Up);
    default: return TransformNormal(Quaternion(X, Y, Z, W), Up);
    }
}

Vector3 UnpackSigned10(int32_t Packed)
{
    const int32_t X = ((Packed >> 0) & ((1 << 10) - 1)) - 512;
    const int32_t Y = ((Packed >> 10) & ((1 << 10) - 1)) - 512;
    const int32_t Z = ((Packed >> 20) & ((1 << 10) - 1)) - 512;
    return Vector3((float)X / 511.0f, (float)Y / 511.0f, (float)Z / 511.0f);
}

void UnpackTable40Face(const uint8_t* Tables, size_t TableCount, const uint8_t* Packed, const uint16_t* Indices, size_t Face, uint32_t (&Out)[3])
{
    // a table: its first index (u32 28), corner width + 1 (byte 34), face count (byte 35), corners
    // (u32 36, from Packed)
    for (size_t T = 0; T < TableCount; T++)
    {
        const uint8_t* Table = Tables + T * 40;
        const uint8_t* Corners = Packed + At<uint32_t>(Table + 36);
        const size_t Count = Table[35];
        if (Face < Count)
        {
            const size_t Bits = Table[34] - 1i64;
            const size_t First = At<uint32_t>(Table + 28);
            for (int K = 0; K < 3; K++) Out[K] = Indices[Table40Corner(Corners, Face * 3 + K, Bits) + First];
            return;
        }
        Face -= Count;
    }
}

bool UnpackDeltaFace(const uint8_t* Buf, size_t Size, uint64_t Tables, uint32_t TableCount, uint64_t Packed, uint64_t Indices, uint32_t Face,
                     uint32_t (&Out)[3], uint32_t TableSize, Edge End)
{
    for (uint32_t T = 0; T < TableCount; T++)
    {
        const uint64_t Table = Tables + (uint64_t)T * TableSize;
        if (Table + TableSize > Size) return false;
        const uint8_t* Tb = Buf + Table;
        const uint32_t Count = (uint32_t)Tb[19] + 1;
        if (Face < Count)
        {
            const uint32_t First = At<uint32_t>(Tb + 20) & 0xFFFFFF, PackedAt = At<uint32_t>(Tb + 24);
            const uint8_t Flags = TableSize > 28 ? Tb[28] : (uint8_t)0x10;
            for (uint32_t K = 0; K < 3; K++)
            {
                const uint8_t Local = DeltaCorner(Buf, Size, (size_t)(Packed + PackedAt), Face * 3 + K, Tb[23], Flags, End);
                const uint64_t At16 = Indices + ((uint64_t)Local + First) * 2;
                if (At16 + 2 > Size) return false;
                Out[K] = At<uint16_t>(Buf + At16);
            }
            return true;
        }
        Face -= Count;
    }
    return false;
}

bool VerticesFit(const Bytes& B, const SubmeshStreams& S, const Encoding& E)
{
    return Inside(S.Positions, S.VertexCount, PositionBytes(E.Position), B.Size) &&
           (E.Normal == NormalCodec::None || Inside(S.Normals, S.VertexCount, 4, B.Size)) &&
           (E.Uv == UvCodec::None || Inside(S.Uvs, S.VertexCount, 4, B.Size));
}

bool DecodeVertices(const Bytes& B, const SubmeshStreams& S, const Encoding& E, const WeightsData* Weights, WraithSubmesh& Out)
{
    if (!VerticesFit(B, S, E)) return false;
    Pick(E)(B.Data, S, Inside(S.Colors, S.VertexCount, 4, B.Size), Weights, Out);
    return true;
}

bool DecodeFaces(const Bytes& B, const SubmeshStreams& S, const Encoding& E, WraithSubmesh& Out)
{
    switch (E.Faces)
    {
    case FaceCodec::U16:
        for (uint32_t F = 0; F < S.FaceCount; F++)
        {
            const uint64_t At16 = S.Faces + (uint64_t)F * 6;
            if (S.Faces == NoStream || At16 + 6 > B.Size) return false;
            Out.AddFace(At<uint16_t>(B.Data + At16), At<uint16_t>(B.Data + At16 + 2), At<uint16_t>(B.Data + At16 + 4));
        }
        return true;
    case FaceCodec::PackedTable40:
    {
        if (S.Tables == NoStream || S.Tables + (uint64_t)S.TableCount * 40 > B.Size || S.Faces > B.Size || S.Packed > B.Size) return false;
        // a face past the tables repeats the one before it
        uint32_t X[3]{};
        for (uint32_t F = 0; F < S.FaceCount; F++)
        {
            UnpackTable40Face(B.Data + S.Tables, S.TableCount, B.Data + S.Packed, reinterpret_cast<const uint16_t*>(B.Data + S.Faces), F, X);
            Out.AddFace(X[0], X[1], X[2]);
        }
        return true;
    }
    default:
        for (uint32_t F = 0; F < S.FaceCount; F++)
        {
            uint32_t X[3]{};
            if (!UnpackDeltaFace(B.Data, B.Size, S.Tables, S.TableCount, S.Packed, S.Faces, F, X, E.FaceTableSize, E.FaceEdge)) return false;
            Out.AddFace(X[0], X[1], X[2]);
        }
        return true;
    }
}

bool DecodeSubmesh(const Bytes& B, const SubmeshStreams& S, const Encoding& E, const WeightsData* Weights, WraithSubmesh& Out)
{
    if (!VerticesFit(B, S, E)) return false;
    Out.PrepareMesh(S.VertexCount, S.FaceCount);
    DecodeVertices(B, S, E, Weights, Out);
    return DecodeFaces(B, S, E, Out);
}

uint32_t ReadRigidRuns(const XModelSubmesh_t& Sub, const RigidRuns& R, std::vector<WeightsData>& Weights)
{
    uint32_t Index = 0;
    for (uint32_t L = 0; L < Sub.VertListcount && Sub.RigidWeightsPtr != 0; L++)
    {
        uint8_t Run[32]{};
        CoDAssets::GameInstance->Read(Run, Sub.RigidWeightsPtr + (uint64_t)L * R.Size, std::min<uint32_t>(R.Size, sizeof(Run)));
        const uint32_t Bone = At<uint16_t>(Run) / R.BoneScale, Count = At<uint16_t>(Run + R.CountAt);
        for (uint32_t W = 0; W < Count && Index < Weights.size(); W++) Weights[Index++].BoneValues[0] = Bone;
    }
    return Index;
}

uint64_t SkinnedColumnsSize(const uint16_t (&WeightCounts)[8])
{
    uint64_t Size = 0;
    for (uint32_t G = 0; G < 8; G++) Size += 4ull * (G + 1) * WeightCounts[G];
    return Size;
}

void ReadSkinnedColumns(const Bytes& Data, const uint16_t (&WeightCounts)[8], uint32_t Index, std::vector<WeightsData>& Weights)
{
    uint64_t Pos = 0;
    for (uint32_t G = 0; G < 8; G++)
    {
        for (uint32_t K = 0; K <= G; K++)
        {
            for (uint32_t W = 0, V = Index; W < WeightCounts[G]; W++, V++, Pos += 4)
            {
                if (Pos + 4 > Data.Size) return;
                if (V >= Weights.size()) continue;
                auto& Weight = Weights[V];
                Weight.WeightCount = (uint8_t)(K + 1);
                Weight.BoneValues[K] = At<uint16_t>(Data.Data + Pos);
                if (K > 0)
                {
                    Weight.WeightValues[K] = (float)At<uint16_t>(Data.Data + Pos + 2) / 65536.0f;
                    Weight.WeightValues[0] -= Weight.WeightValues[K];
                }
            }
        }
        Index += WeightCounts[G];
    }
}

bool ReadVertexWeights(const Bytes& B, uint64_t Offset, uint32_t Count, uint32_t BoneMask, uint32_t& MaxBone, uint32_t* Budget,
                       std::vector<WeightsData>& Weights)
{
    const uint32_t Records = Budget != nullptr ? std::min(Count, *Budget) : Count;
    if (Offset + (uint64_t)Records * 12 > B.Size) return false;
    Weights.assign(Count, WeightsData());
    for (uint32_t V = 0; V < Records; V++)
    {
        const uint8_t* R = B.Data + Offset + (uint64_t)V * 12;
        auto& Weight = Weights[V];
        Weight.WeightCount = 0;
        for (uint32_t K = 0; K < 4; K++)
        {
            if (K > 0 && R[K] == 0) continue;
            const uint32_t Bone = At<uint16_t>(R + 4 + K * 2) & BoneMask;
            Weight.BoneValues[Weight.WeightCount] = Bone;
            Weight.WeightValues[Weight.WeightCount++] = R[K] / 255.0f;
            MaxBone = std::max(MaxBone, Bone);
        }
    }
    if (Budget != nullptr) *Budget -= Records;
    return true;
}

void AddWeightBones(WraithModel& Model, uint32_t MaxBone, uint32_t FirstNumber, int32_t Parent, const char* Root)
{
    if (MaxBone <= Model.BoneCount() - 1) return;
    const auto Current = Model.BoneCount();
    for (uint32_t I = 0; I < MaxBone + 1 - Current; I++)
    {
        auto& Bone = Model.AddBone();
        Bone.TagName = std::format("smod_bone{}", I + FirstNumber);
        Bone.BoneParent = Parent;
    }
    Model.Bones[0].TagName = Root;
}

Buffer FromZone(uint64_t Address, uint64_t Size)
{
    Buffer Result;
    if (!CoDAssets::GameInstance->IsRunning()) return Result;
    Result.Data = std::make_unique<uint8_t[]>((size_t)Size);
    Result.Size = CoDAssets::GameInstance->Read(Result.Data.get(), Address, (size_t)Size);
    return Result;
}

Buffer FromPackage(uint64_t Key, int32_t Size)
{
    Buffer Result;
    if (CoDAssets::GamePackageCache == nullptr) return Result;
    uint32_t Got = 0;
    Result.Data = Size < 0 ? CoDAssets::GamePackageCache->ExtractPackageObject(Key, Got)
                           : CoDAssets::GamePackageCache->ExtractPackageObject(Key, Size, Got);
    Result.Size = Got;
    return Result;
}

SubmeshStreams StreamsOf(const XModelSubmesh_t& Sub, bool Colors)
{
    SubmeshStreams S;
    S.VertexCount = Sub.VertexCount, S.FaceCount = Sub.FaceCount;
    S.Positions = { Sub.VertexPtr, 8 };
    S.Normals = { Sub.VertexNormalsPtr, 4 };
    S.Uvs = { Sub.VertexUVsPtr, 4 };
    if (Colors && Sub.VertexColorPtr != 0xFFFFFFFF) S.Colors = { Sub.VertexColorPtr, 4 };
    S.Faces = Sub.FacesPtr, S.Tables = Sub.PackedIndexTablePtr, S.Packed = Sub.PackedIndexBufferPtr;
    S.TableCount = Sub.PackedIndexTableCount;
    S.Scale = Sub.Scale;
    S.Offset = Vector3(Sub.XOffset, Sub.YOffset, Sub.ZOffset);
    return S;
}

SubmeshStreams RegionStreams(const Regions& R, uint64_t FirstVertex, uint32_t VertexCount, uint64_t FirstIndex, uint32_t FaceCount, bool Colors)
{
    SubmeshStreams S;
    S.VertexCount = VertexCount, S.FaceCount = FaceCount;
    S.Positions = { R.Positions + FirstVertex * 12, 12 };
    const uint64_t Record = R.Records + FirstVertex * R.RecordSize;
    if (Colors) S.Colors = { Record, R.RecordSize };
    S.Uvs = { Record + 4, R.RecordSize };
    S.Normals = { Record + 8, R.RecordSize };
    S.Faces = R.Faces + FirstIndex * 2;
    return S;
}

void WeightFromNearest(WraithSubmesh& Mesh, uint32_t Covered)
{
    // A grid of the weighted vertices, about two per cell; each other vertex searches outward ring by ring
    Vector3 Lo = Mesh.Verticies[0].Position, Hi = Lo;
    for (uint32_t V = 1; V < Covered; V++)
    {
        const Vector3& P = Mesh.Verticies[V].Position;
        Lo = { std::min(Lo.X, P.X), std::min(Lo.Y, P.Y), std::min(Lo.Z, P.Z) };
        Hi = { std::max(Hi.X, P.X), std::max(Hi.Y, P.Y), std::max(Hi.Z, P.Z) };
    }
    const float Volume = std::max(Hi.X - Lo.X, 1e-3f) * std::max(Hi.Y - Lo.Y, 1e-3f) * std::max(Hi.Z - Lo.Z, 1e-3f);
    const float Cell = std::max(std::cbrt(Volume * 2.0f / (float)Covered), 1e-3f);
    const auto Key = [&](float C, float L) { return (int32_t)std::floor((C - L) / Cell); };
    const auto Pack = [](int32_t X, int32_t Y, int32_t Z) { return ((uint64_t)(uint32_t)(X & 0x1fffff) << 42) | ((uint64_t)(uint32_t)(Y & 0x1fffff) << 21) | (uint32_t)(Z & 0x1fffff); };
    std::unordered_map<uint64_t, std::vector<uint32_t>> Grid;
    for (uint32_t V = 0; V < Covered; V++)
    {
        const Vector3& P = Mesh.Verticies[V].Position;
        Grid[Pack(Key(P.X, Lo.X), Key(P.Y, Lo.Y), Key(P.Z, Lo.Z))].push_back(V);
    }
    const int32_t MaxRing = Key(Hi.X, Lo.X) + Key(Hi.Y, Lo.Y) + Key(Hi.Z, Lo.Z) + 2;
    for (uint32_t V = Covered; V < (uint32_t)Mesh.Verticies.size(); V++)
    {
        const Vector3& P = Mesh.Verticies[V].Position;
        const int32_t X = Key(P.X, Lo.X), Y = Key(P.Y, Lo.Y), Z = Key(P.Z, Lo.Z);
        uint32_t Best = 0;
        float BestD = std::numeric_limits<float>::max();
        for (int32_t R = 0; R <= MaxRing; R++)
        {
            for (int32_t DX = -R; DX <= R; DX++)
                for (int32_t DY = -R; DY <= R; DY++)
                    for (int32_t DZ = -R; DZ <= R; DZ++)
                    {
                        if (std::max({ std::abs(DX), std::abs(DY), std::abs(DZ) }) != R) continue;
                        const auto It = Grid.find(Pack(X + DX, Y + DY, Z + DZ));
                        if (It == Grid.end()) continue;
                        for (const uint32_t C : It->second)
                        {
                            const Vector3& Q = Mesh.Verticies[C].Position;
                            const float D = (Q.X - P.X) * (Q.X - P.X) + (Q.Y - P.Y) * (Q.Y - P.Y) + (Q.Z - P.Z) * (Q.Z - P.Z);
                            if (D < BestD) BestD = D, Best = C;
                        }
                    }
            // a vertex found in ring R can be beaten only from a ring whose nearest side is closer than it
            if (BestD < std::numeric_limits<float>::max() && (float)R * Cell >= std::sqrt(BestD)) break;
        }
        Mesh.Verticies[V].Weights = Mesh.Verticies[Best].Weights;
    }
}

void DecodeLod(const Bytes& B, const XModelLod_t& Lod, const LodFormat& F, bool Colors, WraithModel& Model)
{
    Model.PrepareSubmeshes((uint32_t)Lod.Submeshes.size());
    std::vector<WeightsData> Weights;
    std::vector<uint8_t> InZone;
    for (const auto& Sub : Lod.Submeshes)
    {
        auto& Mesh = Model.AddSubmesh();
        Mesh.AddMaterial(Sub.MaterialIndex);
        const SubmeshStreams S = StreamsOf(Sub, Colors);
        if (!VerticesFit(B, S, F.Mesh)) continue;
        const WeightsData* W = nullptr;
        if (Model.BoneCount() > 1)
        {
            Weights.assign(Sub.VertexCount, WeightsData());
            const uint32_t Index = ReadRigidRuns(Sub, F.Rigid, Weights);
            const uint64_t Skinned = SkinnedColumnsSize(Sub.WeightCounts);
            if (Skinned > 0 && F.SkinnedInBuffer && Sub.WeightsPtr + Skinned <= B.Size)
                ReadSkinnedColumns({ B.Data + Sub.WeightsPtr, Skinned }, Sub.WeightCounts, Index, Weights);
            else if (Skinned > 0 && !F.SkinnedInBuffer)
            {
                InZone.assign((size_t)Skinned, 0);
                CoDAssets::GameInstance->Read(InZone.data(), Sub.WeightsPtr, (size_t)Skinned);
                ReadSkinnedColumns({ InZone.data(), Skinned }, Sub.WeightCounts, Index, Weights);
            }
            W = Weights.data();
            const uint64_t Covered = Index + std::accumulate(std::begin(Sub.WeightCounts), std::end(Sub.WeightCounts), 0ull);
            if (DecodeSubmesh(B, S, F.Mesh, W, Mesh) && Covered > 0 && Covered < Sub.VertexCount)
                WeightFromNearest(Mesh, (uint32_t)Covered);
            continue;
        }
        DecodeSubmesh(B, S, F.Mesh, W, Mesh);
    }
}
}
