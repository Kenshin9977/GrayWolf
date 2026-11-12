#pragma once

#include <cstdint>
#include <string>
#include <vector>

// IWI images of the "version 6" layout (CoD4, WaW; CoD2's version 5 is the same) as a DDS file in
// memory: every format of that version, the wavelet-compressed ones included (CoD2: 25 images).
// Header "IWi", version, u8 format, u8 flags, u16 width height depth, u32 fileSizeForPicmip[4];
// then the mips smallest first, each holding all its faces. Each size the header gives is
// checked; a wavelet stream must decode within the file. The DDS lists the largest mip first;
// a cube map's faces each with all their mips, as DDS readers expect.
namespace CoDIWIDecoder
{
    bool ToDds(const uint8_t* Data, size_t Size, std::string& Dds, std::string& Error);

    // The largest mip of an uncompressed or wavelet IWI as 8-bit RGBA: versions 5, 6, 8 (MW2, MW3),
    // 9 (CoD Online) and 13 (Black Ops), whose wavelet codec is the same (OpenAssetTools'
    // IwiLoader). A cube map's six faces one after the other. Luminance is copied into R, G and B,
    // alpha alone comes out black with its alpha (as an A8 image converts). False for a
    // block-compressed format or an image that does not decode within the file.
    struct Rgba
    {
        unsigned Width = 0, Height = 0;
        bool Cube = false;
        std::vector<uint8_t> Pixels;
    };
    bool LargestRgba(const uint8_t* Data, size_t Size, Rgba& Image, std::string& Error);
}
