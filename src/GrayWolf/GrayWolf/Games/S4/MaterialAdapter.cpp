#include "stdafx.h"

#include "MaterialDescription.h"

#include <filesystem>

// Vanguard materials: a slot's semantic is a u32, eight layers per map (README.md beside this file):
// 0-7 colour, 8-15 "nog", 16-23 emissive, 24-31 opacity, 32-39 reveal. Read off global_stream_mp's
// books 2026-11-20: a weapon's slot 0 is its "_c&_s" (the specular packed in the colour's alpha, not
// an opacity), 8 its "_n&_u&_g", 0x19 "loot_opacity_0_a", 1-2 / 9-10 / 0x19-0x1a the loot layers'.
// The first layer described: the colour, the nog as Modern Warfare 2019's (gloss r, normal y g,
// occlusion b, normal x a: "ag"), the emissive, the opacity map as a cutout. Without these slots: the
// names and the readers' hints.
namespace
{
    constexpr uint32_t Color = 0, Nog = 8, Emissive = 16, OpacityMap = 24;

    // an engine placeholder ("$white", "$black"): none
    bool Placeholder(const std::string& Path)
    {
        const std::string Stem = std::filesystem::path(Path).stem().string();
        return !Stem.empty() && Stem[0] == '$';
    }

    using Materials::Role;
    class VanguardMaterials final : public Materials::SemanticAdapter
    {
    public:
        VanguardMaterials()
            : SemanticAdapter("vanguard", {
                  { Color, Role::Albedo },
                  { Nog, Role::Normal },
                  { Nog, Role::Gloss, 0 },
                  { Nog, Role::Occlusion, 2 },
                  { Emissive, Role::Emissive },
              }, Unmatched::Names) {}
    protected:
        // the colour's semantic is 0, also the one an image without a semantic has: a material
        // whose images all have 0 is read by its names
        std::vector<Materials::SemanticRole> TableFor(const Materials::Source& M) const override
        {
            for (const auto& I : M.Images)
                if (I.Semantic != 0) return SemanticAdapter::TableFor(M);
            return {};
        }
        void Refine(const Materials::Source& M, Materials::Description& D, bool BySemantic) const override
        {
            if (!D.Normal.Empty()) D.NormalEnc = Materials::NormalEncoding::AG;
            if (!BySemantic) return;
            if (!D.Emissive.Empty() && Placeholder(D.Emissive.Path)) D.Emissive = {};
            const auto* Op = M.BySemantic(OpacityMap);
            if (Op && !Placeholder(Op->Path))
            {
                D.AlphaMap.Image = { Op->Path, 0, false };
                D.Alpha = Materials::AlphaMode::Cutout;
            }
            else
                D.Alpha = Materials::AlphaMode::Opaque;
        }
    };
    const VanguardMaterials Adapter;
    const Materials::AdapterRegistration Registered("S4", Adapter);
}
