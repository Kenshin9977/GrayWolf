#include "stdafx.h"

#include "MaterialDescription.h"
#include "CoDAssets.h"

#include <algorithm>
#include <cmath>
#include <string>

// Black Ops 3 materials (MaterialDescription.h): colour, normal (x,y in r,g), gloss,
// specular colour, occlusion and emission by semantic. 4E07583B/AB07D475/01782DE2 are the
// character shader's colour, normal and gloss. Layered ("micro tile blend") materials: two layers
// mixed by mixMap, their scales, tints and gloss read from the technique's static pixel cbuffer
// at the offsets the t7 profile's shading.txt gives (from the shaders' RDEF).
// Backlit materials: thicknessMap (inverted) and backlightScatterColor as the transmission; the
// alpha mode of transparent technique sets from what their shader writes as alpha (a reveal map's
// soft edge for the reveal ones, the "reveal" recipe). The colour tint (colorTint by the colour map's
// alpha or tintMask, the "color" recipe) as the colour tint, emissiveTint1 as the emission's colour.
namespace
{
    // Texture semantics: the hash of the shader's own texture name (mod tools' techsetdefs).
    constexpr uint32_t ColorMap = 0xA0AB1041, ColorMapDetail2 = 0x80951342, DetailMap1 = 0x55A604DC,
                       DetailMap2 = 0x55A604DF, MixMap = 0x04429A80, GlossMap = 0x6D0A6C98, GlossMapDetail2 = 0x56B697BB,
                       ThicknessMap = 0x2335512A, RevealMap = 0x34D849D5;

    std::string TechsetName(const Materials::Source& M)
    {
        return !M.Techset.empty() && M.Techset[0] == ',' ? M.Techset.substr(1) : M.Techset;
    }

    // The technique set's shading.txt row when its recipe includes Kind ("layers", "backlit").
    const Materials::Recipe* RecipeWith(const Materials::Source& M, const std::string& Kind)
    {
        const Materials::Recipe* R = M.Techset.empty() ? nullptr : Materials::ShadingRecipe("t7", TechsetName(M));
        if (R == nullptr) return nullptr;
        const std::string Kinds = "+" + R->at("recipe") + "+";
        return Kinds.find("+" + Kind + "+") != std::string::npos ? R : nullptr;
    }

    // Backlit technique sets (lit_transparent_backlit_*, lit_vehicle_backlit): the forward shader adds
    // light from behind * colorMap.rgb * backlightScatterColor * (1 - thicknessMap.c).
    void DescribeBacklight(const Materials::Source& M, Materials::Description& D)
    {
        const Materials::Recipe* R = RecipeWith(M, "backlit");
        const Materials::Source::Image* Thickness = M.BySemantic(ThicknessMap);
        if (R == nullptr || Thickness == nullptr) return;
        const size_t Technique = (size_t)std::stoul(R->at("backlit_technique"));
        static const std::vector<uint8_t> None;
        const std::vector<uint8_t>& Cb = Technique < M.Constants.size() ? M.Constants[Technique] : None;
        D.Transmission.Image = { Thickness->Path, (int)std::string("rgba").find(R->count("thickness_channel") ? R->at("thickness_channel")[0] : 'r'), true };
        for (int c = 0; c < 3; c++) D.Transmission.Color[c] = Materials::RecipeFloat(*R, "backlight_color", Cb, c, 1.0f);
    }

    // What the forward shader writes as alpha (the 17 *transparent* technique sets of zm_island read):
    // the reveal ones (a "reveal" recipe) saturate((revealMap - lo) / (hi - lo)), lo and hi from the
    // vertex alpha (1 here: fully revealed), the soft edge and the ramp, the colour map's alpha unused;
    // lit_*transparent* without reveal colorMap.a times the vertex alpha. Emissive and water ones add
    // to or override the alpha: left as they were.
    void DescribeAlpha(const Materials::Source& M, Materials::Description& D)
    {
        const std::string T = TechsetName(M);
        if (T.find("transparent") == std::string::npos || T.find("emissive") != std::string::npos || T.find("water") != std::string::npos) return;
        if (T.find("reveal") == std::string::npos)
        {
            D.Alpha = Materials::AlphaMode::Blend;
            return;
        }
        const Materials::Recipe* R = RecipeWith(M, "reveal");
        const Materials::Source::Image* Reveal = M.BySemantic(RevealMap);
        if (R == nullptr || Reveal == nullptr) return;
        const size_t Technique = (size_t)std::stoul(R->at("reveal_technique"));
        if (Technique >= M.Constants.size() || M.Constants[Technique].empty()) return;
        const std::vector<uint8_t>& Cb = M.Constants[Technique];
        const float X = 0.999f;   // saturate(0.998 * 1 + 0.001)
        const float Ramp = std::min(std::max(Materials::RecipeFloat(*R, "reveal_ramp", Cb, 0, 1.0f), 0.0f), 1.0f);
        const float Soft = Materials::RecipeFloat(*R, "reveal_soft_edge", Cb, 0, 0.0f);
        const auto Saturate = [](float V) { return std::min(std::max(V, 0.0f), 1.0f); };
        D.AlphaMap.Image = { Reveal->Path, (int)std::string("rgba").find(R->count("reveal_channel") ? R->at("reveal_channel")[0] : 'r'), false };
        D.AlphaMap.Low = Saturate(1.0f - X - Soft * std::pow(X, Ramp));
        D.AlphaMap.High = Saturate(1.0f - X + Soft * std::pow(1.0f - X, Ramp));
        D.AlphaMap.UVScale[0] = Materials::RecipeFloat(*R, "reveal_scale", Cb, 0, 1.0f);
        D.AlphaMap.UVScale[1] = Materials::RecipeFloat(*R, "reveal_scale", Cb, 1, 1.0f);
        D.Alpha = Materials::AlphaMode::Blend;
    }

    // The colour tint of lit technique sets (the "color" recipe): rgb *= lerp(1, colorTint, mask.c), the
    // mask the colour map's alpha (lit, lit_advanced...) or tintMask (lit_alphatest, lit_transparent,
    // decals). A white colour map ($white_diffuse) takes its whole colour from it. A white tint
    // changes nothing: left out.
    void DescribeColor(const Materials::Source& M, Materials::Description& D)
    {
        const Materials::Recipe* R = RecipeWith(M, "color");
        if (R == nullptr) return;
        const size_t Technique = (size_t)std::stoul(R->at("color_technique"));
        const uint32_t MaskSemantic = (uint32_t)std::stoul(R->at("tint_mask"), nullptr, 16);
        // the gbuffer technique (3) writes an opaque surface: a colour map's alpha that is the tint mask
        // is no cutout (the barrels' worn paint, 0 where the tint stops)
        if (MaskSemantic == ColorMap && R->at("tint_mask_channel") == "a" && Technique == 3 && TechsetName(M).find("alphatest") == std::string::npos)
            D.Alpha = Materials::AlphaMode::Opaque;
        if (Technique >= M.Constants.size() || M.Constants[Technique].empty()) return;
        const std::vector<uint8_t>& Cb = M.Constants[Technique];
        float Color[3];
        bool White = true;
        for (int c = 0; c < 3; c++)
        {
            Color[c] = Materials::RecipeFloat(*R, "tint_color", Cb, c, 1.0f);
            White = White && std::abs(Color[c] - 1.0f) < 1e-4f;
        }
        const Materials::Source::Image* Mask = M.BySemantic(MaskSemantic);
        if (White || Mask == nullptr) return;
        D.Color.Mask = { Mask->Path, (int)std::string("rgba").find(R->at("tint_mask_channel")[0]), false };
        for (int c = 0; c < 3; c++) D.Color.Color[c] = Color[c];
    }

    // The emission's colour (lit_emissive*, eye_dni, decal_emissive: the "emissive" recipe):
    // emissiveMap.rgb * emissiveTint1, a white emissive map ($white_diffuse) glowing in that colour.
    void DescribeEmissive(const Materials::Source& M, Materials::Description& D)
    {
        const Materials::Recipe* R = RecipeWith(M, "emissive");
        if (R == nullptr || D.Emissive.Empty()) return;
        const size_t Technique = (size_t)std::stoul(R->at("emissive_technique"));
        if (Technique >= M.Constants.size() || M.Constants[Technique].empty()) return;
        for (int c = 0; c < 3; c++) D.EmissiveColor[c] = Materials::RecipeFloat(*R, "emissive_color", M.Constants[Technique], c, 1.0f);
    }

    void DescribeLayers(const Materials::Source& M, Materials::Description& D)
    {
        const Materials::Recipe* R = RecipeWith(M, "layers");
        if (R == nullptr) return;
        const size_t Technique = (size_t)std::stoul(R->at("technique"));
        if (Technique >= M.Constants.size() || M.Constants[Technique].empty()) return;
        const std::vector<uint8_t>& Cb = M.Constants[Technique];
        const auto F = [&](const char* Var, int C, float Default) { return Materials::RecipeFloat(*R, Var, Cb, C, Default); };
        const auto Path = [&](uint32_t Semantic) { const auto* I = M.BySemantic(Semantic); return I ? I->Path : std::string(); };

        struct Of { uint32_t Color, Normal, Gloss; const char *Scale, *Rotate, *Tint, *Height, *Range; };
        const Of Layers[2] = { { ColorMap, DetailMap1, GlossMap, "detailScale1", "rotateUVs1", "colorTint", "detailScaleHeight1", "glossRange" },
                               { ColorMapDetail2, DetailMap2, GlossMapDetail2, "detailScale2", "rotateUVs2", "colorTint1", "detailScaleHeight2", "glossRange2" } };
        // the basic blend has no gloss maps: each layer's gloss is its range's top, on the 0..17 scale
        const bool GlossMaps = !Path(GlossMapDetail2).empty();
        for (const Of& O : Layers)
        {
            Materials::Layer L;
            L.Albedo.Path = Path(O.Color);
            L.Normal.Path = Path(O.Normal);
            L.NormalScale = F(O.Height, 0, 1.0f);
            L.UVScale[0] = F(O.Scale, 0, 1.0f);
            L.UVScale[1] = F(O.Scale, 1, 1.0f);
            L.UVRotation = F(O.Rotate, 0, 0.0f);
            for (int c = 0; c < 3; c++) L.Tint[c] = F(O.Tint, c, 1.0f);
            if (GlossMaps && !Path(O.Gloss).empty()) L.Roughness = { Path(O.Gloss), 0, true };
            else L.RoughnessValue = 1.0f - std::min(std::max(F(O.Range, 1, 8.5f) / 17.0f, 0.0f), 1.0f);
            D.Layers.push_back(std::move(L));
        }
        auto& Top = D.Layers[1];
        Top.Mask.Path = Path(MixMap);
        Top.Mask.Channel = (int)std::string("rgba").find(R->count("mask_channel") ? R->at("mask_channel")[0] : 'r');
        Top.MaskUVScale[0] = F("mixScale", 0, 1.0f);
        Top.MaskUVScale[1] = F("mixScale", 1, 1.0f);
        // the gbuffer shader writes an opaque surface: the colour maps' alpha is their tint mask
        D.Alpha = Materials::AlphaMode::Opaque;
    }

    class T7Materials final : public Materials::IAdapter
    {
    public:
        const char* Name() const override { return "bo3"; }
        Materials::Description Describe(const Materials::Source& M) const override
        {
            using Materials::Role;
            Materials::Description D = Materials::DescribeBySemantic(M, {
                { 0xA0AB1041, Role::Albedo },   { 0x4E07583B, Role::Albedo },
                { 0x59D30D0F, Role::Normal },   { 0xAB07D475, Role::Normal },
                { 0x6D0A6C98, Role::Gloss },    { 0x01782DE2, Role::Gloss },
                { 0xEC443804, Role::Specular }, { 0x07176BF2, Role::Occlusion },
                { 0x34614347, Role::Emissive },
            });
            DescribeAlpha(M, D);
            if (M.BySemantic(ColorMapDetail2) && M.BySemantic(MixMap)) DescribeLayers(M, D);
            if (D.Layers.empty()) DescribeColor(M, D);   // the layers carry their own tints
            DescribeEmissive(M, D);
            DescribeBacklight(M, D);
            return D;
        }
    };
    const T7Materials Adapter;
    const Materials::AdapterRegistration Registered("T7", Adapter);
}
