#pragma once

#include <cstdint>

struct libdeflate_decompressor;

class Compression
{
public:
    // The thread's libdeflate decompressor (about twice zlib's and miniz's speed): the one every
    // libdeflate inflate of the app uses
    static libdeflate_decompressor* Inflater();

    static uint32_t DecompressLZ4Block(const int8_t* CompressedBuffer, int8_t* DecompressedBuffer, int32_t CompressedSize, int32_t DecompressedSize);
    static uint32_t CompressLZ4Block(const int8_t* Buffer, int8_t* CompressedBuffer, int32_t BufferSize, int32_t CompressedSize);
    static uint32_t DecompressionSizeLZ4();
    static uint32_t CompressionSizeLZ4(uint32_t Size);

    static uint32_t DecompressLZO1XBlock(const int8_t* CompressedBuffer, int8_t* DecompressedBuffer, int32_t CompressedSize, int32_t DecompressedSize);
    static uint32_t DecompressionSizeLZO1X();

    static uint32_t DecompressZLibBlock(const int8_t* CompressedBuffer, int8_t* DecompressedBuffer, int32_t CompressedSize, int32_t DecompressedSize);
    static uint32_t DecompressionSizeZLib();

    static uint32_t DecompressDeflateBlock(const int8_t* CompressedBuffer, int8_t* DecompressedBuffer, int32_t CompressedSize, int32_t DecompressedSize);
    static uint32_t DecompressionSizeDeflate();
};