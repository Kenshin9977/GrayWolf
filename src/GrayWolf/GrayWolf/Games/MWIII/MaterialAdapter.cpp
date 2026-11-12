#include "stdafx.h"

#include "MaterialDescription.h"

#include <filesystem>

// Modern Warfare III materials: a slot's semantic is a byte, one per layer (docs/games/MWIII.md):
// 0-3 colour, 4-7 "nog", 8-11 emissive, 12-15 opacity, 16-19 reveal. The first layer described:
// the colour (its alpha the packed "_s", not an opacity), the nog as Black Ops 6's (gloss r, normal
// y g, occlusion b, normal x a: "ag"), the emissive, the opacity map as a cutout. Measured
// 2026-11-12 (nog red mean 114 std 36, blue 244 and not the normal's z). Without these slots: the
// names and the readers' hints.
namespace
{
    constexpr uint32_t Color = 0, Nog = 4, Emissive = 8, OpacityMap = 12;

    // an engine placeholder ("$white"): no opacity
    bool Placeholder(const std::string& Path)
    {
        const std::string Stem = std::filesystem::path(Path).stem().string();
        return !Stem.empty() && Stem[0] == '$';
    }

    using Materials::Role;
    class ModernWarfare3Materials final : public Materials::SemanticAdapter
    {
    public:
        ModernWarfare3Materials()
            : SemanticAdapter("mwiii", {
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
            if (!D.Emissive.Empty() && Placeholder(D.Emissive.Path)) D.Emissive = {};   // "$black": none
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
    const ModernWarfare3Materials Adapter;
    const Materials::AdapterRegistration Registered("MWIII", Adapter);
}
