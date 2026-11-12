#include "stdafx.h"

#include "MaterialDescription.h"

#include <regex>

// Black Ops materials: colour, normal and a pre-PBR specular map (MaterialDescription.h, LegacySpecularAdapter),
// its alpha a roughness, its colour maps gamma 2.0. The technique set's name says how the base layer
// draws, which the names only guess.
namespace
{
    class BlackOpsAdapter final : public Materials::IAdapter
    {
    public:
        const char* Name() const override { return Legacy.Name(); }
        Materials::Description Describe(const Materials::Source& M) const override
        {
            Materials::Description D = Legacy.Describe(M);
            // "<prefix>_unlitdecalblend_multiply" (decal_darkstain_05, decal_rust, com_stain_01: white
            // colour maps with dark stains, alpha full), else the base layer's token in a lit one
            // ("wc_l_sm_r0c0n0s0x0", "l_sm_r0c0n0s0_m1c1"): r replaces (opaque), t alpha tests, b
            // blends, m multiplies; c n s x the layer's colour, normal, specular and extra maps
            static const std::regex Base("(?:^|_)([rtbm])0(?:[a-z]+0)*(?:_|$)");
            std::smatch Op;
            if (M.Techset.find("decalblend_multiply") != std::string::npos)
                D.Multiply = D.Decal = true;
            else if (M.Techset.find("seethru") == std::string::npos && std::regex_search(M.Techset, Op, Base))
                switch (Op[1].str()[0])
                {
                case 'r': D.Alpha = Materials::AlphaMode::Opaque; break;
                case 't': D.Alpha = Materials::AlphaMode::Cutout; break;
                case 'b': D.Alpha = Materials::AlphaMode::Blend; break;
                default: D.Multiply = true; break;
                }
            else
                return D;
            // the decal sort keys again (the legacy adapter judged them by the guessed alpha)
            D.Decal = D.Multiply ? D.Decal || (M.SortKey >= 9 && M.SortKey <= 29)
                                 : M.SortKey >= 9 && M.SortKey <= 29 && D.Alpha != Materials::AlphaMode::Cutout;
            return D;
        }
    private:
        const Materials::IAdapter& Legacy = Materials::LegacySpecularAdapter(true, true);
    };

    const BlackOpsAdapter Adapter;
    const Materials::AdapterRegistration Registered("T5", Adapter);
}
