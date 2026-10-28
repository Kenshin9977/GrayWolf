#pragma once

#include "ByteView.h"

// The "IWffa100" fast file container of the Infinity Ward family since Modern Warfare (2019):
// a header, optional typed sections, then a stream of Oodle blocks with hash blocks spliced in,
// and the .fp update patch ("IWffd100", VCDIFF over the whole base zone) applied when it sits
// beside the .ff. Each title registers the constants its header version differs by
// (Games/IW8/Container.cpp, Games/MWIII/Container.cpp, Games/IW/Container25.cpp); the decoding
// is one.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace IwffContainer
{
    struct Layout
    {
        // u32 at 8 of the .ff header: the registration key with the magic "IWffa100"
        uint32_t HeaderVersion = 0;
        // The .ff header, and the patched one inside a .fp (a 0x38-byte patch header, the base
        // header, the patched header)
        size_t HeaderSize = 0;
        // The decompressed zone's size, u64 in the header; checked against the stream's when set
        size_t ZoneSizeAt = 0;
        bool CheckZoneSize = false;
        // u32 at 0xc of the header must equal it (0: not checked)
        uint32_t XFileVersion = 0;
        // Typed sections after the header ("TAFF", or "TAFA" with a signature, and a blob a
        // section sizes) before the stream: the stream is the first "\x02IWC" after them
        bool Sections = false;
        // Bytes between the header and "\x02IWC" when there are no sections (Modern Warfare 2019's
        // authentication chunk sits before the stream; later its "IWffs100" chunk follows the magic)
        size_t AuthBeforeStream = 0;
        // Hash blocks of 0x4000 bytes: one after every HashEveryBytes stream bytes (Modern Warfare
        // 2019); 0: one before every 0x200th block of a stream whose "IWffs100" chunk follows its magic
        uint64_t HashEveryBytes = 0;
        // The Oodle libraries tried in order, and what an error says the zones belong to
        std::vector<const wchar_t*> OodleDlls = { L"oo2core_8_win64.dll" };
        const char* Title = "these fast files";
    };

    // Registers Layout's header version with the fast file containers (FastFile.h)
    struct Registration
    {
        explicit Registration(const Layout& L);
    };

    // The typed sections of a header version-25 file at At: where they end, and the blob size one
    // of them gives. False if they do not parse.
    bool SkipSections(ByteView File, size_t At, size_t& End, uint64_t& BlobSize);

    // The section list's magic ("TAFF" 0x46464154, "TAFA" 0x41464154) and its section types in a
    // .ff's header (FilePath, read only to its sections); false if it is not an "IWffa100" file
    // with sections after a header of HeaderSize bytes
    bool ReadSections(const std::string& FilePath, size_t HeaderSize, uint32_t& Magic, std::vector<uint32_t>& Types);
}
