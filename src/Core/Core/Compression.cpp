#include "stdafx.h"

#include "Compression.h"

// External compression libraries (vcpkg headers). lz4 and lzo1x link from lz4.lib and OAT's minilzo.lib,
// which the app already links for OAT: a second LZO library would define the same symbols twice.
#include <lz4.h>
#include <lzo/lzo1x.h>
#include <miniz/miniz.h>
#include <libdeflate.h>

#include <memory>

libdeflate_decompressor* Compression::Inflater()
{
    thread_local std::unique_ptr<libdeflate_decompressor, decltype(&libdeflate_free_decompressor)> D(libdeflate_alloc_decompressor(),
                                                                                                     libdeflate_free_decompressor);
    return D.get();
}

uint32_t Compression::DecompressLZ4Block(const int8_t* CompressedBuffer, int8_t* DecompressedBuffer, int32_t CompressedSize, int32_t DecompressedSize)
{
    if (DecompressedSize == 0) { DecompressedSize = DecompressionSizeLZ4(); }

    auto Result = LZ4_decompress_safe((const char*)CompressedBuffer, (char*)DecompressedBuffer, CompressedSize, DecompressedSize);

    if (Result > 0) { return (uint32_t)Result; }

    return 0;
}

uint32_t Compression::CompressLZ4Block(const int8_t* Buffer, int8_t* CompressedBuffer, int32_t BufferSize, int32_t CompressedSize)
{
    if (CompressedSize == 0) { CompressedSize = CompressionSizeLZ4(BufferSize); }

    auto Result = LZ4_compress_default((const char*)Buffer, (char*)CompressedBuffer, BufferSize, CompressedSize);

    if (Result > 0) { return (uint32_t)Result; }

    return 0;
}

uint32_t Compression::DecompressionSizeLZ4()
{
    return 0x900000;
}

uint32_t Compression::CompressionSizeLZ4(uint32_t Size)
{
    return LZ4_compressBound((int32_t)Size);
}

uint32_t Compression::DecompressLZO1XBlock(const int8_t* CompressedBuffer, int8_t* DecompressedBuffer, int32_t CompressedSize, int32_t DecompressedSize)
{
    if (DecompressedSize == 0) { DecompressedSize = DecompressionSizeLZO1X(); }

    lzo_uint ResultSize = DecompressedSize;

    auto Result = lzo1x_decompress_safe((const uint8_t*)CompressedBuffer, CompressedSize, (uint8_t*)DecompressedBuffer, &ResultSize, NULL);

    if (Result == LZO_E_OK) { return (uint32_t)ResultSize; }

    return 0;
}

uint32_t Compression::DecompressionSizeLZO1X()
{
    return 0x900000;
}

uint32_t Compression::DecompressZLibBlock(const int8_t* CompressedBuffer, int8_t* DecompressedBuffer, int32_t CompressedSize, int32_t DecompressedSize)
{
    if (DecompressedSize == 0) { DecompressedSize = DecompressionSizeZLib(); }

    // libdeflate: what uncompress accepts, Adler-32 checked
    libdeflate_decompressor* const D = Inflater();
    if (D == nullptr || CompressedSize < 0 || DecompressedSize < 0) { return 0; }

    size_t ResultSize = 0;

    if (libdeflate_zlib_decompress(D, CompressedBuffer, (size_t)CompressedSize, DecompressedBuffer, (size_t)DecompressedSize,
                                   &ResultSize) == LIBDEFLATE_SUCCESS)
    {
        return (uint32_t)ResultSize;
    }

    return 0;
}

uint32_t Compression::DecompressionSizeZLib()
{
    return 0x900000;
}

uint32_t Compression::DecompressDeflateBlock(const int8_t* CompressedBuffer, int8_t* DecompressedBuffer, int32_t CompressedSize, int32_t DecompressedSize)
{
    if (DecompressedSize == 0) { DecompressedSize = DecompressionSizeDeflate(); }

    z_stream InflateStream;
    std::memset(&InflateStream, 0, sizeof(InflateStream));

    if (inflateInit2(&InflateStream, -MZ_DEFAULT_WINDOW_BITS))
    {
        return 0;
    }

    InflateStream.avail_in = CompressedSize;
    InflateStream.avail_out = DecompressedSize;
    InflateStream.next_in = (const uint8_t*)CompressedBuffer;
    InflateStream.next_out = (uint8_t*)DecompressedBuffer;

    auto Result = inflate(&InflateStream, Z_SYNC_FLUSH);

    inflateEnd(&InflateStream);

    if (Result == Z_OK || (InflateStream.total_out == DecompressedSize))
    {
        return (uint32_t)InflateStream.total_out;
    }

    return 0;
}

uint32_t Compression::DecompressionSizeDeflate()
{
    return 0x900000;
}
