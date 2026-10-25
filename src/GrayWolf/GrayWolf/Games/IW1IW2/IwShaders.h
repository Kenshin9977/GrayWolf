#pragma once

// CoD1's shader scripts (scripts/<file>.shader, the effects' fxshaders/<file>.shader, Quake 3's
// syntax): what a surface, a model skin or an effect named after a shader draws. For the material
// description only the drawing matters: the colour map (the first stage's image, neither the
// lightmap nor an environment map; an animMap's frames), how it blends, and whether the surface is
// drawn at all (nodraw: clip, caulk...) or is the sky.

#include <map>
#include <string>
#include <vector>

namespace IW12
{
    struct Shader
    {
        enum class Blend { Opaque, Cutout, Blend, Add, Multiply };
        std::string ColorMap;     // as the script writes it (often without extension), lower case
        // an animated colour map (animMap <rate> <maps...>, or CoD1's animMap <rate> then a map
        // line a frame): its frames, ColorMap the first, played at FrameRate a second
        std::vector<std::string> Frames;
        float FrameRate = 0;
        Blend Mode = Blend::Opaque;
        bool NoDraw = false, Sky = false, TwoSided = false;
        // the technique-set word CoD2's materials use for the same blending ("replace", "alphatest",
        // "blend", "add", "multiply"): one vocabulary for the adapter
        const char* BlendWord() const;
    };

    // Every shader the scripts define, by lower-case name; the first definition wins, as in the
    // game. Scripts that do not parse are counted in Failed, their shaders up to the error kept.
    struct ShaderScripts
    {
        std::map<std::string, Shader> Shaders;
        size_t Files = 0, Failed = 0;
        void Parse(const std::string& Text, bool& Ok);
    };
}
