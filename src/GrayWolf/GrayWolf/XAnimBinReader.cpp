#include "stdafx.h"

#include "XAnimBinReader.h"
#include "Compression.h"
#include "SeAnimReader.h"
#include "XAnimBinExport.h"
#include "XMEImport.h"
#include "MemoryWriter.h"

#include <fstream>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <unordered_map>

namespace
{
    // A forward-only cursor with bounds checks: any over-read trips Bad.
    struct Cursor
    {
        const uint8_t* P; size_t N, O = 0; bool Bad = false;
        Cursor(const uint8_t* p, size_t n) : P(p), N(n) {}
        bool Take(size_t n, std::vector<uint8_t>& Out)
        {
            if (O + n > N) { Bad = true; return false; }
            Out.assign(P + O, P + O + n);
            O += n;
            return true;
        }
        uint16_t U16()
        {
            if (O + 2 > N) { Bad = true; return 0; }
            uint16_t v; std::memcpy(&v, P + O, 2); O += 2; return v;
        }
        // Case 0/0xe/0xf/0x10/0x11's string sub-field: raw non-NUL-terminated chars, then 1-4 zero pad
        // bytes (`4 - (len & 3)`: 4 when already aligned). The first pad byte is the terminator a reader
        // scans for; there is no length field on the wire.
        bool Str(std::string& Out)
        {
            const size_t Start = O;
            Out.clear();
            while (O < N && P[O] != 0) { Out.push_back((char)P[O]); O++; }
            if (O >= N) { Bad = true; return false; } // ran off the end without a pad/terminator
            O++; // consume the terminating zero (always present - pad is 1-4, never 0)
            const size_t Consumed = O - Start;
            const size_t AlignedTo = (Consumed + 3) & ~size_t(3);
            if (O + (AlignedTo - Consumed) > N) { Bad = true; return false; }
            O = Start + AlignedTo;
            return true;
        }
    };

    struct XAnimKeyword
    {
        const char* Name;
        XAnimBinValueType Type;
        uint32_t TableIndex;
    };

    // The 60-entry keyword table: name +
    // valueType, extracted from export2bin.exe's table init. Index 0 (";", comment prefix) is omitted;
    // the rest keep their documented index, including the 3 BONE and 2 each of PART/FRAME overloads
    // (same text, different Type: the CRC is seeded with Type, so the tag disambiguates them).
    const XAnimKeyword kKeywords[] = {
        { "//",                XAnimBinValueType::String,        1 },
        { "AMBIENTCOLOR",      XAnimBinValueType::Raw32x4,       2 },
        { "ANIMATION",         XAnimBinValueType::Marker,        3 },
        { "BLINN",             XAnimBinValueType::Raw32x2,       4 },
        { "BONE",              XAnimBinValueType::UInt16,        5 },
        { "BONE",              XAnimBinValueType::UInt16Float,   6 },
        { "BONE",              XAnimBinValueType::UInt32x2String,7 },
        { "BONES",             XAnimBinValueType::UInt16,        8 },
        { "COEFFS",            XAnimBinValueType::Raw32x2,       9 },
        { "COLOR",             XAnimBinValueType::UInt8Nx4,     10 },
        { "FIRSTFRAME",        XAnimBinValueType::UInt16,       11 },
        { "FRAME",             XAnimBinValueType::Raw32,        12 },
        { "FRAME",             XAnimBinValueType::UInt32String, 13 },
        { "FRAMERATE",         XAnimBinValueType::UInt16,       14 },
        { "GLOW",              XAnimBinValueType::Raw32x2,      15 },
        { "INCANDESCENCE",     XAnimBinValueType::Raw32x4,      16 },
        { "MATERIAL",          XAnimBinValueType::UInt16String3,17 },
        { "MODEL",             XAnimBinValueType::Marker,       18 },
        { "NORMAL",            XAnimBinValueType::Int16Nx3,     19 },
        { "NOTETRACK",         XAnimBinValueType::UInt16,       20 },
        { "NOTETRACKS",        XAnimBinValueType::Marker,       21 },
        { "NUMBONES",          XAnimBinValueType::UInt16,       22 },
        { "NUMFACES",          XAnimBinValueType::Raw32_2,      23 },
        { "NUMFRAMES",         XAnimBinValueType::Raw32_2,      24 },
        { "NUMKEYS",           XAnimBinValueType::UInt16,       25 },
        { "NUMMATERIALS",      XAnimBinValueType::UInt16,       26 },
        { "NUMOBJECTS",        XAnimBinValueType::UInt16,       27 },
        { "NUMPARTS",          XAnimBinValueType::UInt16,       28 },
        { "NUMTRACKS",         XAnimBinValueType::UInt16,       29 },
        { "NUMVERTS",          XAnimBinValueType::UInt16,       30 },
        // NUMVERTS32's table index is not in the doc: 0 is a placeholder. Harmless: TableIndex is
        // bookkeeping only, never part of the tag.
        { "NUMVERTS32",        XAnimBinValueType::Raw32_2,       0 },
        { "OBJECT",            XAnimBinValueType::UInt16String, 31 },
        { "OFFSET",            XAnimBinValueType::Raw32x3,      32 },
        { "PART",               XAnimBinValueType::UInt16,       33 },
        { "PART",               XAnimBinValueType::UInt16String, 34 },
        { "PHONG",              XAnimBinValueType::Raw32_3,      35 },
        { "REFLECTIVE",         XAnimBinValueType::Raw32x2,      36 },
        { "REFLECTIVECOLOR",    XAnimBinValueType::Raw32x4,      37 },
        { "REFRACTIVE",         XAnimBinValueType::Raw32x2,      38 },
        { "SCALE",              XAnimBinValueType::Raw32x3,      39 },
        { "SPECULARCOLOR",      XAnimBinValueType::Raw32x4,      40 },
        { "TRANSPARENCY",       XAnimBinValueType::Raw32x4,      41 },
        { "TRI",                XAnimBinValueType::UInt8x2,      42 },
        { "TRI16",              XAnimBinValueType::UInt16x2,     43 },
        { "UV",                 XAnimBinValueType::UVArray,      44 },
        { "VERSION",            XAnimBinValueType::UInt16,       45 },
        { "VERT",               XAnimBinValueType::UInt16,       46 },
        { "VERT32",             XAnimBinValueType::Raw32_2,      47 },
        { "X",                  XAnimBinValueType::Int16Nx3,     48 },
        { "Y",                  XAnimBinValueType::Int16Nx3,     49 },
        { "Z",                  XAnimBinValueType::Int16Nx3,     50 },
        { "NUMSBONES",          XAnimBinValueType::Raw32_2,      51 },
        { "NUMSWEIGHTS",        XAnimBinValueType::Raw32_2,      52 },
        { "QUATERNION",         XAnimBinValueType::Raw32x4,      53 },
        { "NUMIKPITCHLAYERS",   XAnimBinValueType::Raw32_2,      54 },
        { "IKPITCHLAYER",       XAnimBinValueType::Raw32,        55 },
        { "ROTATION",           XAnimBinValueType::Raw32x3,      56 },
        { "NUMCOSMETICBONES",   XAnimBinValueType::Raw32_2,      57 },
        { "EXTRA",              XAnimBinValueType::Raw32x4,      58 },
    };

    const std::unordered_map<uint16_t, const XAnimKeyword*>& TagLookup()
    {
        static const std::unordered_map<uint16_t, const XAnimKeyword*> Map = [] {
            std::unordered_map<uint16_t, const XAnimKeyword*> M;
            for (const auto& K : kKeywords)
            {
                const uint16_t Tag = XBin::Tag(K.Name, K.Type);
                M[Tag] = &K; // no collisions across the 60 entries; a collision would let the second entry win
                             // and show up as a desync a few records later.
            }
            return M;
        }();
        return Map;
    }

    bool ReadRecord(Cursor& C, XAnimBinRecord& Rec)
    {
        const uint16_t Tag = C.U16();
        if (C.Bad) return false;

        const auto& Lookup = TagLookup();
        const auto It = Lookup.find(Tag);
        if (It == Lookup.end()) return false; // real desync - no resync point in this format

        const XAnimKeyword& K = *It->second;
        Rec.Keyword = K.Name;
        Rec.Type = K.Type;
        Rec.TableIndex = K.TableIndex;
        Rec.Raw.clear();
        Rec.Strings.clear();
        Rec.UVs.clear();

        // Gap types (XBin::HasGap): 2 literal zero bytes right after the tag, before the values (case 0's
        // string-alignment pad is separate and additional). 0x13 (TRI16) is never exercised by a shipped file.
        if (XBin::HasGap(K.Type))
        {
            std::vector<uint8_t> Pad2;
            if (!C.Take(2, Pad2) || Pad2[0] != 0 || Pad2[1] != 0) return false;
        }

        switch (K.Type)
        {
        case XAnimBinValueType::String:
        {
            std::string S;
            if (!C.Str(S)) return false;
            Rec.Strings.push_back(std::move(S));
            return true;
        }
        case XAnimBinValueType::Marker:
            return true;
        case XAnimBinValueType::Int16:
        case XAnimBinValueType::UInt16:
            return C.Take(2, Rec.Raw);
        case XAnimBinValueType::Raw32:
        case XAnimBinValueType::Raw32_2:
        case XAnimBinValueType::Raw32_3:
        case XAnimBinValueType::UInt8Nx4:
            return C.Take(4, Rec.Raw);
        case XAnimBinValueType::Int16Nx3:
            return C.Take(6, Rec.Raw);
        case XAnimBinValueType::Raw32x2:
            return C.Take(8, Rec.Raw);
        case XAnimBinValueType::Raw32x3:
            return C.Take(12, Rec.Raw);
        case XAnimBinValueType::Raw32x4:
            return C.Take(16, Rec.Raw);
        case XAnimBinValueType::UInt16Float:
            return C.Take(6, Rec.Raw); // uint16 + float, concatenated wire order
        case XAnimBinValueType::UInt8x2:
            return C.Take(2, Rec.Raw);
        case XAnimBinValueType::UInt16x2:
            return C.Take(4, Rec.Raw);
        case XAnimBinValueType::UVArray:
        {
            const uint16_t Count = C.U16();
            if (C.Bad) return false;
            Rec.UVs.reserve(Count);
            for (uint16_t i = 0; i < Count; i++)
            {
                std::vector<uint8_t> Pair;
                if (!C.Take(8, Pair)) return false;
                float U, V;
                std::memcpy(&U, Pair.data(), 4);
                std::memcpy(&V, Pair.data() + 4, 4);
                Rec.UVs.emplace_back(U, V);
            }
            return true;
        }
        case XAnimBinValueType::UInt16String:
        {
            if (!C.Take(2, Rec.Raw)) return false;
            std::string S;
            if (!C.Str(S)) return false;
            Rec.Strings.push_back(std::move(S));
            return true;
        }
        case XAnimBinValueType::UInt16String3:
        {
            if (!C.Take(2, Rec.Raw)) return false;
            for (int i = 0; i < 3; i++)
            {
                std::string S;
                if (!C.Str(S)) return false;
                Rec.Strings.push_back(std::move(S));
            }
            return true;
        }
        case XAnimBinValueType::UInt32String:
        {
            if (!C.Take(4, Rec.Raw)) return false;
            std::string S;
            if (!C.Str(S)) return false;
            Rec.Strings.push_back(std::move(S));
            return true;
        }
        case XAnimBinValueType::UInt32x2String:
        {
            if (!C.Take(8, Rec.Raw)) return false;
            std::string S;
            if (!C.Str(S)) return false;
            Rec.Strings.push_back(std::move(S));
            return true;
        }
        }
        return false; // unreachable - every XAnimBinValueType is handled above
    }
}

bool ReadXAnimBinRecords(const std::string& Path, std::vector<XAnimBinRecord>& Out)
{
    std::ifstream In(Path, std::ios::binary);
    if (!In) return false;
    std::vector<uint8_t> Bytes((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());

    // Container: "*LZ4*" (5 bytes, no NUL) + uint32 uncompressed_size (LE) + raw (unframed) LZ4
    // block.
    if (Bytes.size() < 9 || std::memcmp(Bytes.data(), "*LZ4*", 5) != 0) return false;

    uint32_t UncompressedSize = 0;
    std::memcpy(&UncompressedSize, Bytes.data() + 5, 4);
    if (UncompressedSize == 0 || UncompressedSize > (256u * 1024 * 1024)) return false; // sanity cap

    std::vector<uint8_t> Decompressed(UncompressedSize);
    const int32_t CompressedSize = (int32_t)(Bytes.size() - 9);
    const uint32_t Produced = Compression::DecompressLZ4Block(
        (const int8_t*)(Bytes.data() + 9), (int8_t*)Decompressed.data(), CompressedSize, (int32_t)UncompressedSize);
    if (Produced != UncompressedSize) return false;

    Out.clear();
    Cursor C(Decompressed.data(), Decompressed.size());
    while (C.O < C.N)
    {
        XAnimBinRecord Rec;
        if (!ReadRecord(C, Rec)) return false;
        Out.push_back(std::move(Rec));
    }
    return !C.Bad;
}

namespace
{
    // Helpers over an XAnimBinRecord's raw payload bytes: Layer 1 hands back wire bytes
    // uninterpreted; Layer 2 knows what each keyword's Raw bytes mean.
    uint16_t RawU16(const XAnimBinRecord& R, size_t Off = 0)
    {
        uint16_t V = 0; if (Off + 2 <= R.Raw.size()) std::memcpy(&V, R.Raw.data() + Off, 2); return V;
    }
    uint32_t RawU32(const XAnimBinRecord& R, size_t Off = 0)
    {
        uint32_t V = 0; if (Off + 4 <= R.Raw.size()) std::memcpy(&V, R.Raw.data() + Off, 4); return V;
    }
    float RawF32(const XAnimBinRecord& R, size_t Off)
    {
        float V = 0; if (Off + 4 <= R.Raw.size()) std::memcpy(&V, R.Raw.data() + Off, 4); return V;
    }
    SeAnimVec3 RawVec3(const XAnimBinRecord& R)
    {
        return SeAnimVec3{ RawF32(R, 0), RawF32(R, 4), RawF32(R, 8) };
    }

    // Reverses case 7's quantization: int16(value*32767 rounded half away from zero), input clamped
    // [-1,1], stored as plain two's complement (the writer's 0x80000000 XOR only gives the 0.5
    // rounding offset the value's sign). Checked on 164 Mod Tools and exported bins (1.17M matrices):
    // decoded this way every X/Y/Z triple is orthonormal to 1e-4; read as sign+magnitude, up to 2.0 off.
    float DequantInt16N(uint16_t Raw)
    {
        return (float)(int16_t)Raw / 32767.0f;
    }

    // X/Y/Z (case 7, Int16Nx3) are read as 3 quantized rows of the bone's local rotation matrix (an
    // inference: export2bin.exe only writes this format). Row-major, right-handed, Shepperd's-method
    // conversion to a quaternion; that X/Y/Z map to matrix ROWS in that order is unverified.
    SeAnimQuat MatrixRowsToQuat(const SeAnimVec3& X, const SeAnimVec3& Y, const SeAnimVec3& Z)
    {
        const float m[3][3] = {
            { X.x, X.y, X.z },
            { Y.x, Y.y, Y.z },
            { Z.x, Z.y, Z.z },
        };
        const float Trace = m[0][0] + m[1][1] + m[2][2];
        SeAnimQuat Q;
        if (Trace > 0.0f)
        {
            const float S = std::sqrt(Trace + 1.0f) * 2.0f;
            Q.w = 0.25f * S;
            Q.x = (m[2][1] - m[1][2]) / S;
            Q.y = (m[0][2] - m[2][0]) / S;
            Q.z = (m[1][0] - m[0][1]) / S;
        }
        else if (m[0][0] > m[1][1] && m[0][0] > m[2][2])
        {
            const float S = std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
            Q.w = (m[2][1] - m[1][2]) / S;
            Q.x = 0.25f * S;
            Q.y = (m[0][1] + m[1][0]) / S;
            Q.z = (m[0][2] + m[2][0]) / S;
        }
        else if (m[1][1] > m[2][2])
        {
            const float S = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
            Q.w = (m[0][2] - m[2][0]) / S;
            Q.x = (m[0][1] + m[1][0]) / S;
            Q.y = 0.25f * S;
            Q.z = (m[1][2] + m[2][1]) / S;
        }
        else
        {
            const float S = std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
            Q.w = (m[1][0] - m[0][1]) / S;
            Q.x = (m[0][2] + m[2][0]) / S;
            Q.y = (m[1][2] + m[2][1]) / S;
            Q.z = 0.25f * S;
        }
        return Q;
    }
}

bool ReadXAnimBinClip(const std::string& Path, SeAnimClip& Out)
{
    std::vector<XAnimBinRecord> Records;
    if (!ReadXAnimBinRecords(Path, Records)) return false;

    Out = SeAnimClip{};
    size_t I = 0;
    const size_t N = Records.size();
    auto Next = [&]() -> const XAnimBinRecord* { return I < N ? &Records[I] : nullptr; };

    // Leading "//" build-metadata comments (export filename/source filename/export time/frame
    // range - see the doc) - an arbitrary count, skip all of them.
    while (Next() && Next()->Keyword == "//") I++;

    if (!Next() || Next()->Keyword != "ANIMATION") return false;
    I++;

    if (!Next() || Next()->Keyword != "VERSION") return false;
    I++; // version number itself isn't needed for assembly

    if (!Next() || Next()->Keyword != "NUMPARTS") return false;
    const uint16_t NumParts = RawU16(*Next());
    I++;

    Out.Bones.resize(NumParts);
    for (uint16_t P = 0; P < NumParts; P++)
    {
        if (!Next() || Next()->Keyword != "PART" || Next()->Type != XAnimBinValueType::UInt16String)
            return false;
        if (Next()->Strings.empty()) return false;
        Out.Bones[P].Name = Next()->Strings[0];
        I++;
    }

    if (!Next() || Next()->Keyword != "FRAMERATE") return false;
    Out.FrameRate = (float)RawU16(*Next());
    I++;

    if (!Next() || Next()->Keyword != "NUMFRAMES") return false;
    Out.FrameCount = RawU32(*Next());
    I++;

    // Per-frame data: FRAME(marker w/ a uint32 frame index) then, for each of NumParts bones (every
    // bone re-declared in every frame), PART(select-by-index) + OFFSET (+ optional SCALE) + X/Y/Z
    // (the rotation matrix rows) + optional EXTRA.
    uint32_t FramesRead = 0;
    while (Next() && Next()->Keyword == "FRAME" && Next()->Type == XAnimBinValueType::Raw32)
    {
        const uint32_t FrameIdx = RawU32(*Next());
        I++;

        for (uint16_t P = 0; P < NumParts; P++)
        {
            if (!Next() || Next()->Keyword != "PART" || Next()->Type != XAnimBinValueType::UInt16)
                return false;
            const uint16_t BoneIdx = RawU16(*Next());
            I++;
            if (BoneIdx >= Out.Bones.size()) return false;

            if (!Next() || Next()->Keyword != "OFFSET") return false;
            const SeAnimVec3 Offset = RawVec3(*Next());
            I++;
            Out.Bones[BoneIdx].PosKeys.push_back(SeAnimKeyVec3{ FrameIdx, Offset });

            if (Next() && Next()->Keyword == "SCALE") // not seen in the real corpus, handled anyway
            {
                const SeAnimVec3 Scale = RawVec3(*Next());
                I++;
                Out.Bones[BoneIdx].ScaleKeys.push_back(SeAnimKeyVec3{ FrameIdx, Scale });
            }

            if (!Next() || Next()->Keyword != "X") return false;
            const SeAnimVec3 RowX{ DequantInt16N(RawU16(*Next(), 0)), DequantInt16N(RawU16(*Next(), 2)), DequantInt16N(RawU16(*Next(), 4)) };
            I++;
            if (!Next() || Next()->Keyword != "Y") return false;
            const SeAnimVec3 RowY{ DequantInt16N(RawU16(*Next(), 0)), DequantInt16N(RawU16(*Next(), 2)), DequantInt16N(RawU16(*Next(), 4)) };
            I++;
            if (!Next() || Next()->Keyword != "Z") return false;
            const SeAnimVec3 RowZ{ DequantInt16N(RawU16(*Next(), 0)), DequantInt16N(RawU16(*Next(), 2)), DequantInt16N(RawU16(*Next(), 4)) };
            I++;

            // EXTRA (case 0xb, 4 raw floats/uint32s) follows Z for every bone in about a quarter of the
            // corpus (all zero where inspected): consumed and discarded.
            if (Next() && Next()->Keyword == "EXTRA") I++;

            Out.Bones[BoneIdx].RotKeys.push_back(SeAnimKeyQuat{ FrameIdx, MatrixRowsToQuat(RowX, RowY, RowZ) });
        }
        FramesRead++;
    }
    if (FramesRead != Out.FrameCount) return false; // declared count must match what was assembled

    // Notifications come in two trailing shapes (likely from the source's notetrack syntax):
    //   flat:     NUMKEYS<n> + n x FRAME(0x10)<frame><name>                     (no bone grouping)
    //   per-bone: NOTETRACKS(marker), then for each of NumParts bones:
    //               PART<boneIdx> + NUMTRACKS<trackCount>, then for each track:
    //                 NOTETRACK<trackIdx> + NUMKEYS<keyCount> + keyCount x FRAME(0x10) pairs
    // SeAnimClip has no per-bone notify concept (matches SEAnim's own flat notify block), so both
    // shapes fold into the same Out.Notifications map - which bone/track a key was attached to
    // isn't kept.
    auto ReadKeyList = [&](uint16_t Count) -> bool
    {
        uint16_t Read = 0;
        while (Next() && Next()->Keyword == "FRAME" && Next()->Type == XAnimBinValueType::UInt32String)
        {
            const uint32_t FrameIdx = RawU32(*Next());
            if (Next()->Strings.empty()) return false;
            Out.Notifications[Next()->Strings[0]].push_back(FrameIdx);
            I++;
            Read++;
        }
        return Read == Count;
    };

    if (Next() && Next()->Keyword == "NOTETRACKS" && Next()->Type == XAnimBinValueType::Marker)
    {
        I++;
        while (Next() && Next()->Keyword == "PART" && Next()->Type == XAnimBinValueType::UInt16)
        {
            I++; // bone index itself isn't needed - see the comment above
            if (!Next() || Next()->Keyword != "NUMTRACKS") return false;
            const uint16_t TrackCount = RawU16(*Next());
            I++;
            for (uint16_t T = 0; T < TrackCount; T++)
            {
                if (!Next() || Next()->Keyword != "NOTETRACK") return false;
                I++;
                if (!Next() || Next()->Keyword != "NUMKEYS") return false;
                const uint16_t KeyCount = RawU16(*Next());
                I++;
                if (!ReadKeyList(KeyCount)) return false;
            }
        }
    }
    else if (Next() && Next()->Keyword == "NUMKEYS")
    {
        const uint16_t NumKeys = RawU16(*Next());
        I++;
        if (!ReadKeyList(NumKeys)) return false;
    }
    // else: no trailing notify section at all in this file - nothing to do.

    if (I != N) return false; // every record must be accounted for - no trailing unknown section

    Out.Type = SeAnimBoneType::Absolute; // every frame stores a full pose directly (not additive/
                                          // relative-to-previous).
    Out.Looping = false; // not encoded in this format
    return true;
}

bool WriteXAnimBinRecords(const std::vector<XAnimBinRecord>& Records, const std::string& Path)
{
    MemoryWriter W(0x100000);
    for (const auto& R : Records)
    {
        XBin::WriteField(W, R.Keyword.c_str(), R.Type);
        const auto Raw = [&] { if (!R.Raw.empty()) W.Write(R.Raw.data(), (uint32_t)R.Raw.size()); };
        const auto Str = [&](size_t I) { XBin::WriteStringAligned(W, I < R.Strings.size() ? R.Strings[I] : std::string()); };
        switch (R.Type)
        {
        case XAnimBinValueType::String: Str(0); break;
        case XAnimBinValueType::Marker: break;
        case XAnimBinValueType::UVArray:
            W.Write<uint16_t>((uint16_t)R.UVs.size());
            for (const auto& [U, V] : R.UVs) { W.Write(U); W.Write(V); }
            break;
        case XAnimBinValueType::UInt16String:
        case XAnimBinValueType::UInt32String:
        case XAnimBinValueType::UInt32x2String: Raw(); Str(0); break;
        case XAnimBinValueType::UInt16String3: Raw(); Str(0); Str(1); Str(2); break;
        default: Raw(); break;   // the fixed shapes: their bytes as read
        }
    }
    return XBin::SaveContainer(Path, W.GetCurrentStream(), (uint32_t)W.GetPosition());
}

void XAnimBinAxesToQuat(const double X[3], const double Y[3], const double Z[3], double Q[4])
{
    // m[row][column], columns X, Y, Z
    const double m[3][3] = { { X[0], Y[0], Z[0] }, { X[1], Y[1], Z[1] }, { X[2], Y[2], Z[2] } };
    const double Trace = m[0][0] + m[1][1] + m[2][2];
    if (Trace > 0)
    {
        const double S = std::sqrt(Trace + 1.0) * 2;
        Q[3] = 0.25 * S; Q[0] = (m[2][1] - m[1][2]) / S; Q[1] = (m[0][2] - m[2][0]) / S; Q[2] = (m[1][0] - m[0][1]) / S;
    }
    else if (m[0][0] > m[1][1] && m[0][0] > m[2][2])
    {
        const double S = std::sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2;
        Q[3] = (m[2][1] - m[1][2]) / S; Q[0] = 0.25 * S; Q[1] = (m[0][1] + m[1][0]) / S; Q[2] = (m[0][2] + m[2][0]) / S;
    }
    else if (m[1][1] > m[2][2])
    {
        const double S = std::sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2;
        Q[3] = (m[0][2] - m[2][0]) / S; Q[0] = (m[0][1] + m[1][0]) / S; Q[1] = 0.25 * S; Q[2] = (m[1][2] + m[2][1]) / S;
    }
    else
    {
        const double S = std::sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2;
        Q[3] = (m[1][0] - m[0][1]) / S; Q[0] = (m[0][2] + m[2][0]) / S; Q[1] = (m[1][2] + m[2][1]) / S; Q[2] = 0.25 * S;
    }
    const double L = std::sqrt(Q[0] * Q[0] + Q[1] * Q[1] + Q[2] * Q[2] + Q[3] * Q[3]);
    for (int I = 0; I < 4; I++) Q[I] = L > 0 ? Q[I] / L : (I == 3 ? 1 : 0);
}

namespace
{
    bool CheckRig(const XAnimBinRig& Rig, std::string& Error)
    {
        if (Rig.Bones.empty()) { Error = "the model has no bone"; return false; }
        for (size_t I = 0; I < Rig.Bones.size(); I++)
        {
            const int32_t P = Rig.Bones[I].Parent;
            if (P >= (int32_t)I) { Error = "bone " + Rig.Bones[I].Name + "'s parent comes after it"; return false; }
            if (Rig.Bones[I].Name.empty()) { Error = "bone " + std::to_string(I) + " has no name"; return false; }
        }
        return true;
    }
}

bool ReadXAnimBinRig(const std::string& Path, XAnimBinRig& Out, std::string& Error)
{
    Out = XAnimBinRig{};
    Out.Source = std::filesystem::path(Path).filename().string();
    std::string Ext = std::filesystem::path(Path).extension().string();
    for (auto& C : Ext) C = (char)std::tolower((unsigned char)C);

    if (Ext == ".xmodel_export")
    {
        XMEModel Model;
        if (!CodXMEReader::ReadFile(Path, Model, Error)) return false;
        for (const auto& B : Model.Bones)
        {
            XAnimBinRig::Bone R;
            R.Name = B.Name;
            R.Parent = B.Parent;
            for (int I = 0; I < 3; I++) R.Position[I] = B.Offset[I];
            const double X[3] = { B.Axes[0][0], B.Axes[0][1], B.Axes[0][2] };
            const double Y[3] = { B.Axes[1][0], B.Axes[1][1], B.Axes[1][2] };
            const double Z[3] = { B.Axes[2][0], B.Axes[2][1], B.Axes[2][2] };
            XAnimBinAxesToQuat(X, Y, Z, R.Rotation);
            Out.Bones.push_back(std::move(R));
        }
        return CheckRig(Out, Error);
    }

    std::vector<XAnimBinRecord> Records;
    if (!ReadXAnimBinRecords(Path, Records)) { Error = "not an XMODEL_BIN (or XMODEL_EXPORT) model"; return false; }
    struct Axes { double X[3] = { 1, 0, 0 }, Y[3] = { 0, 1, 0 }, Z[3] = { 0, 0, 1 }; };
    std::vector<Axes> Rows;
    int64_t Current = -1;
    for (const auto& R : Records)
    {
        if (R.Keyword == "NUMVERTS" || R.Keyword == "NUMVERTS32") break;
        if (R.Keyword == "NUMBONES") { Out.Bones.resize(RawU16(R)); Rows.resize(Out.Bones.size()); continue; }
        if (R.Keyword == "BONE" && R.Type == XAnimBinValueType::UInt32x2String)
        {
            const uint32_t Index = RawU32(R, 0), Parent = RawU32(R, 4);
            if (Index >= Out.Bones.size() || R.Strings.empty()) { Error = "a bone outside NUMBONES"; return false; }
            Out.Bones[Index].Name = R.Strings[0];
            Out.Bones[Index].Parent = (int32_t)Parent;   // 0xFFFFFFFF: -1, a root
            continue;
        }
        if (R.Keyword == "BONE" && R.Type == XAnimBinValueType::UInt16)
        {
            Current = RawU16(R);
            if (Current >= (int64_t)Out.Bones.size()) { Error = "a bone outside NUMBONES"; return false; }
            continue;
        }
        if (Current < 0) continue;
        auto& B = Out.Bones[(size_t)Current];
        if (R.Keyword == "OFFSET")
            for (int I = 0; I < 3; I++) B.Position[I] = RawF32(R, 4 * I);
        else if (R.Keyword == "X" || R.Keyword == "Y" || R.Keyword == "Z")
        {
            double* Row = R.Keyword == "X" ? Rows[(size_t)Current].X : (R.Keyword == "Y" ? Rows[(size_t)Current].Y : Rows[(size_t)Current].Z);
            for (int I = 0; I < 3; I++) Row[I] = DequantInt16N(RawU16(R, 2 * I));
        }
    }
    for (size_t I = 0; I < Out.Bones.size(); I++) XAnimBinAxesToQuat(Rows[I].X, Rows[I].Y, Rows[I].Z, Out.Bones[I].Rotation);
    return CheckRig(Out, Error);
}
