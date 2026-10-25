#pragma once

// One material description for every game: which file is the colour, normal, roughness or gloss,
// occlusion, specular or metalness, emission, transmission, which channel holds each value, and the
// alpha mode.
// Written beside a model as <model>.materials.json.

#include <cstdint>
#include <map>
#include <string>
#include <vector>


namespace Materials
{
    enum class AlphaMode
    {
        Opaque,
        Cutout,  // alpha tested at AlphaCutoff (alpha to coverage under MSAA)
        Blend,   // alpha blended
        Auto,    // cutout when the colour map's alpha is used, else opaque (decided on load)
    };

    // XY: x in r, y in g, z rebuilt. AG: x in a, y in g (DXT5nm). XYZ: all three.
    enum class NormalEncoding { XY, AG, XYZ };

    // A texture file (relative to the model file) and the channel holding the value (0..3 = rgba;
    // colour maps use rgb). Invert: the value is 1 - sample (a gloss map read as roughness).
    struct Map
    {
        std::string Path;
        int Channel = 0;
        bool Invert = false;
        bool Empty() const { return Path.empty(); }
    };

    // A colour gradient looked up by the mesh's first UV set: albedo.rgb *= G(U, v), where
    // v = saturate(uv.y + OffsetScale * (2 * Offset[channel] - 1)) and G is Image.rgb (Rgb) or
    // Image[channel] * Color. Color is linear.
    struct Gradient
    {
        Map Image;
        bool Rgb = true;
        float U = 0.5f;
        Map Offset;
        float OffsetScale = 0.0f;
        float Color[3] = { 1, 1, 1 };
        bool Empty() const { return Image.Empty(); }
    };

    // A colour mixed in by a mask: albedo.rgb *= lerp(1, Color, Mask[channel]). Color is linear.
    struct ColorTint
    {
        Map Mask;
        float Color[3] = { 1, 1, 1 };
        bool Empty() const { return Mask.Empty(); }
    };

    // Light passing through a thin surface from behind (a leaf, a pod's shell): what comes out on
    // the viewer's side is the light reaching the far side times albedo.rgb * Color * Image[channel]
    // (Invert: the map is a thickness, 1 - sample). Color is linear.
    struct Backlight
    {
        Map Image;
        float Color[3] = { 1, 1, 1 };
        bool Empty() const { return Image.Empty(); }
    };

    // A map that sets the alpha in place of the colour map's: alpha = saturate((Image[channel] at
    // uv * UVScale - Low) / (High - Low)) (a reveal map's soft edge).
    struct Opacity
    {
        Map Image;
        float Low = 0.0f, High = 1.0f;
        float UVScale[2] = { 1, 1 };
        bool Empty() const { return Image.Empty(); }
    };

    // One surface layer. Its maps are sampled at uv' = (R(UVRotation) * (uv - 0.5) + 0.5) * UVScale;
    // its colour is tinted by its own alpha (rgb *= lerp(1, Tint, a)); its normal is a detail
    // normal whose xy, times NormalScale, add to the base normal's. A layer after the first covers
    // the result below it by lerp with Mask[channel], sampled at uv * MaskUVScale.
    struct Layer
    {
        Map Albedo, Normal, Roughness, Mask;
        float NormalScale = 1.0f;
        float RoughnessValue = -1.0f;   // < 0: none (the material's own roughness)
        float UVScale[2] = { 1, 1 }, MaskUVScale[2] = { 1, 1 };
        float UVRotation = 0.0f;        // radians
        float Tint[3] = { 1, 1, 1 };    // linear
    };

    struct Description
    {
        std::string Name;
        Gradient Tint;                  // empty: none
        ColorTint Color;                // empty: none (applied before Tint)
        std::vector<Layer> Layers;      // when set, the colour (and roughness) is theirs; Normal is the base
        Map Albedo;                     // sRGB colour, alpha in a
        Map Normal;
        NormalEncoding NormalEnc = NormalEncoding::XY;
        bool NormalFlipY = false;
        Map Roughness;
        Map Metalness;                  // metal workflow
        Map Specular;                   // sRGB F0 colour, used when Metalness is empty
        // A pre-PBR specular map (reflection colour, not F0): F0 = map * SpecularScale, the colour
        // kept whole instead of what the reflection takes from it.
        bool LegacySpecular = false;
        float SpecularScale = 1.0f;
        Map Occlusion;
        Map Emissive;                   // sRGB colour
        float EmissiveColor[3] = { 1, 1, 1 };   // the emission times this (linear)
        Backlight Transmission;         // empty: none
        Opacity AlphaMap;               // empty: the colour map's alpha
        AlphaMode Alpha = AlphaMode::Opaque;
        float AlphaCutoff = 0.5f;
        float RoughnessValue = 0.6f;    // without a roughness map
        float MetalnessValue = 0.0f;    // without a metalness or specular map
    };

    // What an exporter or a model file knows about one material: the input of an adapter.
    // Thickness: a transmission map read inverted (as Gloss is a roughness map read inverted).
    enum class Role { Unknown, Albedo, Normal, Gloss, Roughness, Specular, Metalness, Occlusion, Emissive, Transmission, Thickness };
    struct Source
    {
        struct Image
        {
            std::string Path;           // as the model file refers to it
            Role Hint = Role::Unknown;  // the usage the game reader gave it
            uint32_t Semantic = 0;      // the game's semantic hash, 0 when unknown
        };
        std::string Name, Techset;
        std::vector<Image> Images;
        std::vector<std::vector<uint8_t>> Constants;   // the game's constant bytes, by its own block index
        int SortKey = -1;                               // the game's sort key, -1 when unknown
        const Image* BySemantic(uint32_t Semantic) const;
    };

    // The row a game's zonekit profile gives a technique set in shading.txt (read off the game's
    // pixel shaders): "recipe" and the constant offsets it names; null when none.
    using Recipe = std::map<std::string, std::string>;
    const Recipe* ShadingRecipe(const std::string& Profile, const std::string& Techset);
    // The float at a recipe field's offset in Block (the field a decimal or 0x offset), else Default.
    float RecipeFloat(const Recipe& R, const std::string& Field, const std::vector<uint8_t>& Block, int Component, float Default);

    // One per game (Games/<game>/MaterialAdapter.cpp): its materials as a Description.
    class IAdapter
    {
    public:
        virtual ~IAdapter() = default;
        virtual const char* Name() const = 0;
        virtual Description Describe(const Source& Material) const = 0;
    };
    struct AdapterRegistration
    {
        AdapterRegistration(const char* Engine, const IAdapter& Adapter);   // the game's Games/<ENGINE>
    };
    const IAdapter& AdapterFor(const std::string& Engine);   // the engine's, else the default
    const IAdapter& DefaultAdapter();                  // texture name suffixes
    // The pre-PBR games' (colorMap, normalMap, specularMap with gloss in its alpha): a game registers
    // it as its own adapter.
    const IAdapter& LegacySpecularAdapter();

    // The role a file name's suffix gives (_c colour, _n normal, _g gloss, _s specular, _o
    // occlusion, _e emissive and their long forms), a "-<hash>" tail ignored.
    Role RoleFromName(const std::string& FileName);
    // Hair, beard, fur, cards, foliage, decals: names of alpha-tested surfaces.
    bool LooksAlphaTested(const std::string& Name);

    // For games whose materials give each texture a semantic hash: the roles those semantics
    // mean (one semantic may feed two roles, e.g. a specular map's alpha read as gloss). Images
    // whose semantic is not in the table are left out; alpha is Cutout for an alpha-tested name
    // (material or colour map), else Auto.
    struct SemanticRole
    {
        uint32_t Semantic;
        Role Use;
        int Channel = 0;
    };
    Description DescribeBySemantic(const Source& Material, const std::vector<SemanticRole>& Table);

    // What a format with a colour factor only (glTF's baseColorFactor) multiplies the colour map by:
    // the colour tint whole, its mask left out (exact where the mask is 1, as on a white colour map),
    // else white. Linear.
    void BaseColorFactor(const Description& D, float Rgb[3]);

    struct Sidecar
    {
        std::string Game, Adapter;
        std::vector<Description> Materials;
        std::vector<Source> Sources;    // the raw inputs, parallel to Materials
    };
    std::string SidecarPath(const std::string& ModelPath);  // <dir>/<stem>.materials.json
    bool WriteSidecar(const std::string& Path, const Sidecar& S);   // a file already holding it is left alone
    bool SidecarHolds(const std::string& Path, const Sidecar& S);   // the file holds exactly what WriteSidecar writes
    bool ReadSidecar(const std::string& Path, Sidecar& S);
}
