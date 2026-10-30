#pragma once

#include "MaterialDescription.h"

// The texture semantics of Treyarch's PBR games (Black Ops 3, Black Ops 4, Black Ops Cold War): the
// hash of the shader's own texture name (mod tools' techsetdefs), so one name means one slot in each.
// Colour BC7 sRGB, normal x,y in r,g (b holds something else: z is rebuilt), gloss and occlusion BC4,
// specular colour BC1. 4E07583B/AB07D475/01782DE2 are the character shader's colour, normal and
// gloss. Cold War's gloss is E9817F0D (the _g map, read off its zm abomination exports), and its
// 34614347 (emissive) is mostly the engine's "$black_color" placeholder.
namespace TreyarchMaterials
{
    constexpr uint32_t ColorMap = 0xA0AB1041, NormalMap = 0x59D30D0F, GlossMap = 0x6D0A6C98, SpecularMap = 0xEC443804,
                       OcclusionMap = 0x07176BF2, EmissiveMap = 0x34614347, ColdWarGlossMap = 0xE9817F0D;

    inline const std::vector<Materials::SemanticRole>& Table()
    {
        using Materials::Role;
        static const std::vector<Materials::SemanticRole> Rows = {
            { ColorMap, Role::Albedo },       { 0x4E07583B, Role::Albedo },
            { NormalMap, Role::Normal },      { 0xAB07D475, Role::Normal },
            { GlossMap, Role::Gloss },        { 0x01782DE2, Role::Gloss },    { ColdWarGlossMap, Role::Gloss },
            { SpecularMap, Role::Specular },  { OcclusionMap, Role::Occlusion },
            { EmissiveMap, Role::Emissive },
        };
        return Rows;
    }
}
