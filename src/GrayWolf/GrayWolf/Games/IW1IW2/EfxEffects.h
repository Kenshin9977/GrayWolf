#pragma once

// CoD1's and CoD2's effects: fx/<name>.efx, text the game parses at load. Elements (Particle, Tail,
// FxRunner...) each a block of "key values" lines (a pair: min, max), "key [ names ]" lists and
// "key { ... }" graphs over the life: CoD1's start/end, CoD2's curves times a scale (start/end too).
// Written as the fx kind's JSON in Black Ops' element layout (TreyarchEffects.h), graphs sampled
// into velocity and visual samples, so the effects preview reads it unchanged; the units the files
// leave open are listed in the JSON's "notes".

#include <nlohmann/json_fwd.hpp>

#include <functional>
#include <string>

namespace IW12::Efx
{
    // The effect a reference names: "/fx/explosions/grenade_flash", "fx/explosions/ground.efx" ->
    // "explosions/grenade_flash", lower case (the archives' paths)
    std::string EffectName(const std::string& Ref);

    // What a shader draws, added to its visual ({"material": <shader>}): techset, images
    // (usage, name), an atlas [rows, columns]. Unset: the name only.
    using MaterialFields = std::function<void(const std::string& Shader, nlohmann::ordered_json& Visual)>;

    // The .efx text of the effect Name as the fx kind's JSON (Out); false with Error when it does not parse.
    bool ToJson(const std::string& Text, const std::string& Name, const std::string& Game, const MaterialFields& Material,
                std::string& Out, std::string& Error);
}
