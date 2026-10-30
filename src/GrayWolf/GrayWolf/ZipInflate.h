#pragma once

// Inflating with libdeflate (about twice miniz's and zlib's speed) where the output size is known:
// zip entries (.iwd, .pk3) and the zlib streams of the archives and packages read whole.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include <miniz/miniz.h>

namespace ZipInflate
{
    // One zlib stream at In inflated into Out, at most OutAvail bytes (Got: how many); false when
    // it does not inflate or needs more room (what uncompress refuses: bad data, a bad Adler-32)
    bool Zlib(const uint8_t* In, size_t InSize, uint8_t* Out, size_t OutAvail, size_t& Got);

    // mz_zip_reader_extract_to_mem's bytes for entry Index (Out holds OutSize >= its size): a stored
    // or deflated entry read through the archive's own read callback and inflated by libdeflate,
    // its CRC-32 checked; anything else (encrypted, other methods, a failure) left to miniz.
    // The caller holds whatever guards the archive's reads.
    bool Extract(mz_zip_archive* Zip, mz_uint Index, void* Out, size_t OutSize);

    // The same, Out resized to the entry's size (empty: a directory or an empty file), Lock held
    // only while the archive is read: the inflating runs beside the other threads' reads
    bool Extract(mz_zip_archive* Zip, mz_uint Index, std::vector<uint8_t>& Out, std::mutex& Lock);
}
