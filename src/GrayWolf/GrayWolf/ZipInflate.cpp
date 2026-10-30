#include "stdafx.h"

#include "ZipInflate.h"
#include "Compression.h"   // Inflater

#include <libdeflate.h>

#include <cstring>
#include <memory>

namespace
{
    constexpr uint32_t LocalHeaderSignature = 0x04034b50;
    constexpr size_t LocalHeaderSize = 30;   // its name's length at +26, its extra field's at +28
    // encrypted (bit 0), a compressed patch (bit 5), strong encryption (bit 6): miniz refuses them
    constexpr uint16_t RefusedFlags = 0x0001 | 0x0020 | 0x0040;

    // The compressed bytes of the thread's last entry (kept: the next one reuses the room)
    thread_local std::vector<uint8_t> Packed;

    // Nothing to write: miniz returns true before reading anything
    bool Empty(const mz_zip_archive_file_stat& S)
    {
        return S.m_is_directory || S.m_comp_size == 0;
    }

    // What libdeflate reads: stored or deflated, not encrypted, fitting the output
    bool Handled(const mz_zip_archive* Zip, const mz_zip_archive_file_stat& S, size_t OutSize)
    {
        return Zip->m_pRead != nullptr && (S.m_bit_flag & RefusedFlags) == 0 &&
               (S.m_method == 0 || S.m_method == MZ_DEFLATED) && S.m_uncomp_size <= OutSize;
    }

    // Where the entry's data starts (after its local header), within the archive
    bool DataStart(mz_zip_archive* Zip, const mz_zip_archive_file_stat& S, uint64_t& At)
    {
        uint8_t H[LocalHeaderSize];
        if (Zip->m_pRead(Zip->m_pIO_opaque, S.m_local_header_ofs, H, LocalHeaderSize) != LocalHeaderSize) return false;
        uint32_t Signature = 0;
        uint16_t NameLength = 0, ExtraLength = 0;
        std::memcpy(&Signature, H, 4);
        std::memcpy(&NameLength, H + 26, 2);
        std::memcpy(&ExtraLength, H + 28, 2);
        if (Signature != LocalHeaderSignature) return false;
        At = S.m_local_header_ofs + LocalHeaderSize + NameLength + ExtraLength;
        return At + S.m_comp_size <= Zip->m_archive_size;
    }

    // The entry's bytes as the archive holds them: a stored one's straight into Out, a deflated
    // one's into Packed. False: left to miniz.
    bool ReadData(mz_zip_archive* Zip, const mz_zip_archive_file_stat& S, void* Out)
    {
        uint64_t At = 0;
        if (!DataStart(Zip, S, At)) return false;
        if (S.m_method == 0)
            return Zip->m_pRead(Zip->m_pIO_opaque, At, Out, (size_t)S.m_uncomp_size) == S.m_uncomp_size;
        Packed.resize((size_t)S.m_comp_size);
        return Zip->m_pRead(Zip->m_pIO_opaque, At, Packed.data(), Packed.size()) == Packed.size();
    }

    // A deflated entry inflated from Packed into exactly its size, and its CRC-32 checked
    bool Finish(const mz_zip_archive_file_stat& S, void* Out)
    {
        if (S.m_method != 0 &&
            libdeflate_deflate_decompress(Compression::Inflater(), Packed.data(), Packed.size(), Out,
                                          (size_t)S.m_uncomp_size, nullptr) != LIBDEFLATE_SUCCESS)
            return false;
        return libdeflate_crc32(0, Out, (size_t)S.m_uncomp_size) == S.m_crc32;
    }
}

namespace ZipInflate
{
    bool Zlib(const uint8_t* In, size_t InSize, uint8_t* Out, size_t OutAvail, size_t& Got)
    {
        Got = 0;
        return libdeflate_zlib_decompress(Compression::Inflater(), In, InSize, Out, OutAvail, &Got) == LIBDEFLATE_SUCCESS;
    }

    bool Extract(mz_zip_archive* Zip, mz_uint Index, void* Out, size_t OutSize)
    {
        mz_zip_archive_file_stat S;
        if (Zip != nullptr && Zip->m_pState != nullptr && (OutSize == 0 || Out != nullptr) &&
            mz_zip_reader_file_stat(Zip, Index, &S))
        {
            if (Empty(S)) return true;
            if (Handled(Zip, S, OutSize) && ReadData(Zip, S, Out) && Finish(S, Out)) return true;
        }
        // anything else, a failure included: miniz's own answer
        return mz_zip_reader_extract_to_mem(Zip, Index, Out, OutSize, 0) != 0;
    }

    bool Extract(mz_zip_archive* Zip, mz_uint Index, std::vector<uint8_t>& Out, std::mutex& Lock)
    {
        mz_zip_archive_file_stat S;
        {
            std::lock_guard<std::mutex> Guard(Lock);
            if (!mz_zip_reader_file_stat(Zip, Index, &S)) return false;
            Out.resize((size_t)S.m_uncomp_size);
            if (Out.empty() || Empty(S)) return true;
            if (!Handled(Zip, S, Out.size()) || !ReadData(Zip, S, Out.data()))
                return mz_zip_reader_extract_to_mem(Zip, Index, Out.data(), Out.size(), 0) != 0;
        }
        if (Finish(S, Out.data())) return true;
        std::lock_guard<std::mutex> Guard(Lock);
        return mz_zip_reader_extract_to_mem(Zip, Index, Out.data(), Out.size(), 0) != 0;
    }
}
