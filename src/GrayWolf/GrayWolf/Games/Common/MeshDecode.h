#pragma once

// One mesh decoder for every model reader. What a game's submesh is made of is data: where its
// streams are in the LOD's mesh buffer (SubmeshStreams) and how each one is packed (Encoding: a
// handful of codecs). The codecs are chosen once per submesh, the vertex loop being instantiated
// per combination, so a vertex costs what it did in each game's own loop. Every read is checked
// against the buffer: a submesh whose vertex streams fall outside it is left empty, its faces
// stop at the first one that does, and the caller is told (a game may fail the whole model).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "CoDXModelTranslator.h"   // WeightsData
#include "VectorMath.h"

class WraithModel;
class WraithSubmesh;
struct XModelLod_t;
struct XModelSubmesh_t;

namespace MeshDecode
{
    // Positions: three 21-bit unsigned fields scaled into the submesh's box (8 bytes), or three floats
    enum class PositionCodec : uint8_t { Packed21, Float3 };
    // Normals (4 bytes): a QTangent (CoDQTangent), Modern Warfare 2019's packed quaternion turning
    // up, ten signed bits per axis (Treyarch), or the older games' unit vectors (A: WaW to MW3, B:
    // BO2, C: Ghosts to IW); None leaves the normal and the UVs unread
    enum class NormalCodec : uint8_t { None, QTangent, PackedQuat, Signed10, UnitA, UnitB, UnitC };
    // UVs: two half floats, U then V (or V then U: the games of NormalCodec::UnitA)
    enum class UvCodec : uint8_t { None, Half2, Half2Swapped };
    // Faces: u16 triples, or packed corners indexing the u16 index run through tables of 40 bytes
    // (Modern Warfare 2019 to III), or Black Ops 6 / 7's (FaceTableSize bytes, delta-coded corners)
    enum class FaceCodec : uint8_t { U16, PackedTable40, PackedTableDelta };
    // PackedTableDelta: a corner whose 8-byte word the buffer ends inside reads the bytes there
    // (Partial) or 0 (Zero)
    enum class Edge : uint8_t { Partial, Zero };

    struct Encoding
    {
        PositionCodec Position = PositionCodec::Packed21;
        NormalCodec Normal = NormalCodec::QTangent;
        UvCodec Uv = UvCodec::Half2;
        FaceCodec Faces = FaceCodec::U16;
        uint32_t FaceTableSize = 32;   // PackedTableDelta
        Edge FaceEdge = Edge::Partial;
    };

    constexpr uint64_t NoStream = UINT64_MAX;
    // A per-vertex stream: its first element's offset in the buffer, the bytes from one to the next
    struct Stream
    {
        uint64_t Offset = NoStream;
        uint32_t Stride = 0;
    };
    struct SubmeshStreams
    {
        uint32_t VertexCount = 0, FaceCount = 0;
        Stream Positions, Normals, Uvs, Colors;   // Colors NoStream: white
        uint64_t Faces = NoStream;                // u16 triples, or the packed faces' index run
        uint64_t Tables = NoStream, Packed = NoStream;   // the packed faces' tables and corners
        uint32_t TableCount = 0;
        float Scale = 1.0f;                       // Packed21: the box's half size and centre
        Vector3 Offset;
    };
    // A mesh buffer's bytes as a decoder reads them
    struct Bytes
    {
        const uint8_t* Data = nullptr;
        uint64_t Size = 0;
    };

    // ---- the codecs alone (the world readers decode their vertices with them too)
    Vector3 Unpack21BitVertex(uint64_t Packed, float Scale, const Vector3& Offset);
    Vector3 UnpackPackedQuat(uint32_t Packed);
    Vector3 UnpackSigned10(int32_t Packed);
    // A face out of 40-byte tables (TableCount at Tables, corners at Packed, the index run at
    // Indices): Out left as it was when Face is past the tables
    void UnpackTable40Face(const uint8_t* Tables, size_t TableCount, const uint8_t* Packed, const uint16_t* Indices, size_t Face,
                           uint32_t (&Out)[3]);
    // A face out of delta tables of TableSize bytes, all offsets into Buf: each its face count
    // (byte 19, + 1), first index (u24 20), corner bit width (byte 23), packed offset (u32 24), delta
    // width and flags (byte 28; a 28-byte table has none: every corner in the full width). false
    // when a table or an index is outside Buf, or Face past the tables
    bool UnpackDeltaFace(const uint8_t* Buf, size_t Size, uint64_t Tables, uint32_t TableCount, uint64_t Packed, uint64_t Indices, uint32_t Face,
                         uint32_t (&Out)[3], uint32_t TableSize, Edge End);

    // ---- a submesh
    // Whether every vertex stream S reads is inside B
    bool VerticesFit(const Bytes& B, const SubmeshStreams& S, const Encoding& E);
    // S's vertices appended to Out (their weights from Weights, one per vertex; nullptr: all on
    // bone 0); false (nothing read) when a stream is outside B
    bool DecodeVertices(const Bytes& B, const SubmeshStreams& S, const Encoding& E, const WeightsData* Weights, WraithSubmesh& Out);
    // S's faces appended to Out; false when one was outside B (the ones before it are kept)
    bool DecodeFaces(const Bytes& B, const SubmeshStreams& S, const Encoding& E, WraithSubmesh& Out);
    // Both, Out prepared for them; false when a vertex stream is outside B (Out left empty) or a face
    // is (the vertices and the faces before it kept)
    bool DecodeSubmesh(const Bytes& B, const SubmeshStreams& S, const Encoding& E, const WeightsData* Weights, WraithSubmesh& Out);

    // ---- weights
    // Rigid runs: Sub.VertListcount records of Size bytes at Sub.RigidWeightsPtr (in the zone),
    // each a bone (u16 0, divided by BoneScale) and a count of vertices on it (u16 CountAt). Returns
    // the first vertex after them.
    struct RigidRuns
    {
        uint32_t Size = 8, CountAt = 2, BoneScale = 1;
    };
    uint32_t ReadRigidRuns(const XModelSubmesh_t& Sub, const RigidRuns& R, std::vector<WeightsData>& Weights);
    // The skinned vertices after them, column by column: WeightCounts[g] vertices on g + 1 bones,
    // every vertex's first bone (and 2 bytes of padding), then every vertex's (bone, u16 weight) for
    // the second influence, and so on. Its size in bytes:
    uint64_t SkinnedColumnsSize(const uint16_t (&WeightCounts)[8]);
    void ReadSkinnedColumns(const Bytes& Data, const uint16_t (&WeightCounts)[8], uint32_t Index, std::vector<WeightsData>& Weights);
    // A weight per vertex (12 bytes: four u8 weights, four u16 bones; the first always, the others
    // when not 0), bones masked with BoneMask; MaxBone raised to the highest bone read. Budget (when
    // set) the vertices left with a record: the others on bone 0. false when the records are
    // outside B.
    bool ReadVertexWeights(const Bytes& B, uint64_t Offset, uint32_t Count, uint32_t BoneMask, uint32_t& MaxBone, uint32_t* Budget,
                           std::vector<WeightsData>& Weights);
    // The bones a model's weights name past its own (up to MaxBone): "smod_bone<n>" added (n from
    // FirstNumber) under Parent, the model's first bone renamed Root
    void AddWeightBones(WraithModel& Model, uint32_t MaxBone, uint32_t FirstNumber, int32_t Parent, const char* Root);

    // ---- a LOD
    // Where a LOD's mesh is: Size bytes at Address in the zone (what could be read), or a package
    // object by its key (Size -1: sized by the package index). Data nullptr when none could be read.
    struct Buffer
    {
        std::unique_ptr<uint8_t[]> Data;
        uint64_t Size = 0;
        Bytes View() const { return { Data.get(), Size }; }
    };
    Buffer FromZone(uint64_t Address, uint64_t Size);
    Buffer FromPackage(uint64_t Key, int32_t Size = -1);

    // A LOD whose submeshes (XModelSubmesh_t) name their streams by offset in one buffer: 21-bit
    // positions (8 bytes), normals, UVs, colours (4 bytes each), rigid runs then skinned columns
    // (in the buffer at WeightsPtr, or in the zone)
    struct LodFormat
    {
        Encoding Mesh;
        RigidRuns Rigid;
        bool SkinnedInBuffer = true;
    };
    SubmeshStreams StreamsOf(const XModelSubmesh_t& Sub, bool Colors);
    // Every submesh of Lod out of B into Model (one submesh each, its material set; empty when its
    // streams are outside B). Weights are read when the model has more than one bone.
    void DecodeLod(const Bytes& B, const XModelLod_t& Lod, const LodFormat& F, bool Colors, WraithModel& Model);

    // Black Ops III to Cold War's mesh buffer: a region per stream, a vertex's position three
    // floats, its record (RecordSize bytes) a colour, half UVs and a ten-bit normal, its weights 12
    // bytes (ReadVertexWeights), faces u16 triples; a submesh's first vertex and first index count
    // in each region
    struct Regions
    {
        uint64_t Positions = 0, Records = 0, Weights = 0, Faces = 0;
        uint32_t RecordSize = 16;
    };
    constexpr Encoding RegionEncoding{ PositionCodec::Float3, NormalCodec::Signed10, UvCodec::Half2, FaceCodec::U16 };
    SubmeshStreams RegionStreams(const Regions& R, uint64_t FirstVertex, uint32_t VertexCount, uint64_t FirstIndex, uint32_t FaceCount, bool Colors);
    // A Black Ops 4 or Cold War LOD's mesh info (TInfo: its StatusFlag and its regions' offsets):
    // its regions, the records 8 bytes longer when its status says extended (bit 6)
    template <class TInfo>
    Regions RegionsOf(const TInfo& Info)
    {
        return { Info.VertexOffset, Info.UVOffset, Info.WeightsOffset, Info.FacesOffset, (Info.StatusFlag & 64) != 0 ? 24u : 16u };
    }
    // ... and its buffer: Size bytes at Address in the zone when its status says loaded (8 in its
    // low six bits), else the package object Key (PackageSize as FromPackage's); throws when it is
    // in the packages and none is open
    Buffer StreamedMesh(uint32_t StatusFlag, uint64_t Address, uint64_t Size, uint64_t Key, int32_t PackageSize = -1);

    // The decoder's checks on byte arrays from real surfaces; the failures appended to Failures
    int SelfTest(std::string& Failures);
}
