#include "stdafx.h"

#include "MaterialDescription.h"
#include "Games/IW/IwMaterialAlpha.h"

#include <string_view>

// Infinite Warfare materials (MaterialDescription.h; measured in graywolf-tools docs/games/IW7.md). The
// packing is in the images' names: <c>_<sg>_packed_cs (rgb colour, a metalness), <n>_packed_nog (r gloss,
// g normal y, b occlusion, a normal x), _packed_ng (no occlusion), <c>_packed_a (the colour's alpha in r,
// in the specOcclusionMap slot); unpacked ones are IW6's (specular rgb, gloss in its a; _o occlusion).
// The second layer's "...Map1" slots (camo) left out. The alpha mode from the sort key
// (Games/IW/IwMaterialAlpha.h) and an "_atest" techset; a packed colour map's alpha is never an opacity.
namespace
{
    constexpr uint32_t ColorMap = 0xA0AB1041, NormalMap = 0x59D30D0F, SpecularMap = 0x34ECCCB3, SpecOcclusionMap = 0xA52C26B4,
                       EmissiveMap = 0x34614347;

    // the image's file name (extension left out) ends with _packed_<Packing>
    bool Packed(const Materials::Source::Image* Image, std::string_view Packing)
    {
        if (Image == nullptr) return false;
        const std::string Suffix = "_packed_" + std::string(Packing);
        const std::string& P = Image->Path;
        const size_t Dot = P.find_last_of('.'), Slash = P.find_last_of("/\\");
        const size_t End = Dot == std::string::npos || (Slash != std::string::npos && Dot < Slash) ? P.size() : Dot;
        return End >= Suffix.size() && P.compare(End - Suffix.size(), Suffix.size(), Suffix) == 0;
    }

    // how a material's maps are packed, from their names
    struct Packing
    {
        const Materials::Source::Image *Color, *SpecOcclusion, *Opacity;
        bool PackedColor, Nog, Ng;
        explicit Packing(const Materials::Source& M)
            : Color(M.BySemantic(ColorMap)), SpecOcclusion(M.BySemantic(SpecOcclusionMap))
        {
            const auto* Normal = M.BySemantic(NormalMap);
            PackedColor = Packed(Color, "cs");
            Nog = Packed(Normal, "nog");
            Ng = Packed(Normal, "ng");
            Opacity = Packed(SpecOcclusion, "a") ? SpecOcclusion : nullptr;
        }
    };

    class IW7Materials final : public Materials::SemanticAdapter
    {
    public:
        // the table built per material (TableFor); one without these slots: the names and the readers' hints
        IW7Materials() : SemanticAdapter("iw", {}, Unmatched::Names) {}
    protected:
        std::vector<Materials::SemanticRole> TableFor(const Materials::Source& M) const override
        {
            using Materials::Role;
            const Packing P(M);
            std::vector<Materials::SemanticRole> Table = { { ColorMap, Role::Albedo }, { NormalMap, Role::Normal }, { EmissiveMap, Role::Emissive } };
            if (P.PackedColor) Table.push_back({ ColorMap, Role::Metalness, 3 });
            else Table.push_back({ SpecularMap, Role::Specular });
            if (P.Nog || P.Ng) Table.push_back({ NormalMap, Role::Gloss, 0 });
            else Table.push_back({ SpecularMap, Role::Gloss, 3 });
            if (P.Nog) Table.push_back({ NormalMap, Role::Occlusion, 2 });
            else if (P.SpecOcclusion && !P.Opacity) Table.push_back({ SpecOcclusionMap, Role::Occlusion, 0 });
            return Table;
        }
        void Refine(const Materials::Source& M, Materials::Description& D, bool BySemantic) const override
        {
            if (!BySemantic) return;
            if (!D.Normal.Empty()) D.NormalEnc = Materials::NormalEncoding::AG;
            // the alpha: the packed_a's, else an unpacked colour map's
            const Packing P(M);
            const bool HasAlpha = P.Opacity != nullptr || (P.Color != nullptr && !P.PackedColor);
            const bool Tested = M.Techset.find("_atest") != std::string::npos;
            if (IwMaterialAlpha::Apply(M, M.SortKey > 2 || Tested ? P.Opacity : nullptr, D))
            {
                if (!HasAlpha) D.Alpha = Materials::AlphaMode::Opaque;
                else if (Tested && D.Alpha == Materials::AlphaMode::Opaque) D.Alpha = Materials::AlphaMode::Cutout;
            }
            else if (Tested && HasAlpha)
            {
                if (P.Opacity) D.AlphaMap.Image = { P.Opacity->Path, 0, false };
                D.Alpha = Materials::AlphaMode::Cutout;
            }
            else if (Tested || P.PackedColor)
                D.Alpha = Materials::AlphaMode::Opaque;
        }
    };
    const IW7Materials Adapter;
    const Materials::AdapterRegistration Registered("IW7", Adapter);
}
