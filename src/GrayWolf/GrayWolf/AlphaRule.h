#pragma once

// Whether a colour map's alpha masks something, what a material's Auto alpha means (MaterialDescription.h):
// the alphas of its texels (or a sample of them) added one by one, then Masks(). The preview and
// the glTF judge a map so (PreviewTextures.h), the mod tools' GDT too (MaterialGdt.h).

#include <cstddef>
#include <cstdint>

struct AlphaTally
{
    size_t Seen = 0, Clear = 0, Solid = 0;
    void Add(uint8_t A)
    {
        Seen++;
        Clear += A < 26;
        Solid += A > 229;
    }
    // at least 2% of the texels clear and 80% either clear or opaque: a smooth alpha (IW games keep
    // other data there) is not a mask
    bool Masks() const { return Seen && Clear * 50 >= Seen && (Clear + Solid) * 5 >= Seen * 4; }
};
