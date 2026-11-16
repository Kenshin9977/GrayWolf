#include "stdafx.h"

#include "XBin.h"

#include "BinaryWriter.h"
#include "Compression.h"
#include "MemoryWriter.h"

#include <cmath>
#include <memory>

namespace XBin
{
    uint16_t Tag(const char* Keyword, XAnimBinValueType Type)
    {
        uint32_t Crc = (uint32_t)Type;
        for (const char* P = Keyword; *P; P++)
        {
            Crc ^= (uint32_t)(uint8_t)*P << 8;
            for (int Bit = 0; Bit < 8; Bit++)
                Crc = (Crc & 0x8000) ? ((Crc << 1) ^ 0x1021) : (Crc << 1);
        }
        return (uint16_t)Crc;
    }

    bool HasGap(XAnimBinValueType Type)
    {
        // from export2bin's writers, checked on every .xanim_bin and .xmodel_bin the Mod Tools ship
        switch (Type)
        {
        case XAnimBinValueType::String:
        case XAnimBinValueType::Marker:
        case XAnimBinValueType::Raw32:
        case XAnimBinValueType::Raw32_2:
        case XAnimBinValueType::UInt8Nx4:
        case XAnimBinValueType::Raw32_3:
        case XAnimBinValueType::Raw32x2:
        case XAnimBinValueType::Raw32x3:
        case XAnimBinValueType::Raw32x4:
        case XAnimBinValueType::UInt32String:
        case XAnimBinValueType::UInt32x2String:
            return true;
        default:
            return false;
        }
    }

    int16_t Int16N(float Value)
    {
        // a NaN comes out 0, as the clamp of a NaN would not
        if (!(Value == Value)) return 0;
        const float Clamped = Value < -1.0f ? -1.0f : (Value > 1.0f ? 1.0f : Value);
        const float Scaled = Clamped * 32767.0f;
        return (int16_t)(Scaled < 0 ? Scaled - 0.5f : Scaled + 0.5f);
    }

    void WriteStringAligned(MemoryWriter& Writer, const std::string& Value)
    {
        Writer.WriteNullTerminatedString(Value);
        const size_t Written = Value.size() + 1;
        static const uint8_t Zeros[4] = {};
        Writer.Write(Zeros, (uint32_t)(((Written + 3) & ~size_t(3)) - Written));
    }

    void WriteField(MemoryWriter& Writer, const char* Keyword, XAnimBinValueType Type)
    {
        Writer.Write(Tag(Keyword, Type));
        if (HasGap(Type)) Writer.Write<uint16_t>(0);
    }

    bool SaveContainer(const std::string& FileName, const int8_t* Payload, uint32_t Size)
    {
        static const char Magic[5] = { 0x2A, 0x4C, 0x5A, 0x34, 0x2A };   // "*LZ4*"
        const auto Bound = Compression::CompressionSizeLZ4(Size);
        auto Compressed = std::make_unique<int8_t[]>(Bound);
        const auto CompressedSize = Compression::CompressLZ4Block(Payload, Compressed.get(), (int32_t)Size, (int32_t)Bound);
        if (CompressedSize == 0 && Size != 0) return false;

        auto File = BinaryWriter();
        if (!File.Create(FileName)) return false;
        File.Write(Magic);
        File.Write(Size);
        File.Write(Compressed.get(), CompressedSize);
        return true;
    }
}
