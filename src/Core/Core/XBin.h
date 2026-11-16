#pragma once

#include <cstdint>
#include <string>

class MemoryWriter;

// The Black Ops III Mod Tools' binary export form (.XMODEL_BIN, .XANIM_BIN), what export2bin.exe
// makes of an XMODEL_EXPORT / XANIM_EXPORT text: a "*LZ4*" magic, the uint32 size of the payload,
// then the payload as one raw LZ4 block. The payload is one record per line of the text: a 2-byte
// tag (CRC-16/CCITT of the keyword, seeded with its value type), then the line's values in the
// shape that type gives (graywolf-tools/docs/bo3-xanim-bin-format.md). Shared by the XMODEL_BIN
// writer (XMBExport), the XANIM_BIN writer (XAnimBinExport) and the reader (XAnimBinReader).

// A keyword's value type, export2bin's own numbers. Most types write 2 zero bytes right after the
// tag, before their values (HasGap).
enum class XAnimBinValueType : uint32_t
{
    String = 0,          // raw bytes, then 1 to 4 zero bytes up to a multiple of 4 (never 0: the first is the end)
    Marker = 1,          // no value: the tag is the record (ANIMATION, MODEL, NOTETRACKS)
    Int16 = 2,
    UInt16 = 3,
    Raw32 = 4,
    Raw32_2 = 5,         // the same 4 raw bytes as Raw32, another keyword set
    UInt8Nx4 = 6,        // 4 x uint8: value * 255 + 0.5, value in [0, 1]
    Int16Nx3 = 7,        // 3 x int16, two's complement: value * 32767 rounded half away from zero, value in [-1, 1]
    Raw32_3 = 8,
    Raw32x2 = 9,
    Raw32x3 = 10,
    Raw32x4 = 0xb,
    UInt16Float = 0xc,   // uint16 (a bone) + raw float (its weight)
    UVArray = 0xd,       // uint16 count, then count x (float, float)
    UInt16String = 0xe,  // uint16 + string
    UInt16String3 = 0xf, // uint16 + 3 strings
    UInt32String = 0x10,
    UInt32x2String = 0x11,
    UInt8x2 = 0x12,
    UInt16x2 = 0x13,
};

namespace XBin
{
    // A record's tag: CRC-16/CCITT (polynomial 0x1021) of the keyword, seeded with its value type
    uint16_t Tag(const char* Keyword, XAnimBinValueType Type);

    // The types that write 2 zero bytes between the tag and the values
    bool HasGap(XAnimBinValueType Type);

    // export2bin's case 7: value * 32767 rounded half away from zero, value clamped to [-1, 1]
    int16_t Int16N(float Value);

    // A string value: its bytes, then 1 to 4 zero bytes so that the whole is a multiple of 4
    void WriteStringAligned(MemoryWriter& Writer, const std::string& Value);

    // A record's tag, then its 2 zero bytes when its type has them: its values follow
    void WriteField(MemoryWriter& Writer, const char* Keyword, XAnimBinValueType Type);

    // The container: "*LZ4*", the payload's size, the payload as one LZ4 block. False when the
    // file cannot be written.
    bool SaveContainer(const std::string& FileName, const int8_t* Payload, uint32_t Size);
}
