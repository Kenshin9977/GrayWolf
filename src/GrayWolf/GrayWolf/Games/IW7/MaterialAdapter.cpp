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

    class IW7Materials final : public Materials::IAdapter
    {
    public:
        const char* Name() const override { return "iw"; }
        Materials::Description Describe(const Materials::Source& M) const override
        {
            using Materials::Role;
            const auto* Color = M.BySemantic(ColorMap);
            const auto* Normal = M.BySemantic(NormalMap);
            const auto* SpecOcclusion = M.BySemantic(SpecOcclusionMap);
            const bool PackedColor = Packed(Color, "cs"), Nog = Packed(Normal, "nog"), Ng = Packed(Normal, "ng");
            const auto* Opacity = Packed(SpecOcclusion, "a") ? SpecOcclusion : nullptr;

            std::vector<Materials::SemanticRole> Table = { { ColorMap, Role::Albedo }, { NormalMap, Role::Normal }, { EmissiveMap, Role::Emissive } };
            if (PackedColor) Table.push_back({ ColorMap, Role::Metalness, 3 });
            else Table.push_back({ SpecularMap, Role::Specular });
            if (Nog || Ng) Table.push_back({ NormalMap, Role::Gloss, 0 });
            else Table.push_back({ SpecularMap, Role::Gloss, 3 });
            if (Nog) Table.push_back({ NormalMap, Role::Occlusion, 2 });
            else if (SpecOcclusion && !Opacity) Table.push_back({ SpecOcclusionMap, Role::Occlusion, 0 });
            Materials::Description D = Materials::DescribeBySemantic(M, Table);
            if (!D.Normal.Empty()) D.NormalEnc = Materials::NormalEncoding::AG;
            // a material without these slots: the names and the readers' hints
            if (D.Albedo.Empty() && D.Normal.Empty()) return Materials::DefaultAdapter().Describe(M);

            // the alpha: the packed_a's, else an unpacked colour map's
            const bool HasAlpha = Opacity != nullptr || (Color != nullptr && !PackedColor);
            const bool Tested = M.Techset.find("_atest") != std::string::npos;
            if (IwMaterialAlpha::Apply(M, M.SortKey > 2 || Tested ? Opacity : nullptr, D))
            {
                if (!HasAlpha) D.Alpha = Materials::AlphaMode::Opaque;
                else if (Tested && D.Alpha == Materials::AlphaMode::Opaque) D.Alpha = Materials::AlphaMode::Cutout;
            }
            else if (Tested && HasAlpha)
            {
                if (Opacity) D.AlphaMap.Image = { Opacity->Path, 0, false };
                D.Alpha = Materials::AlphaMode::Cutout;
            }
            else if (Tested || PackedColor)
                D.Alpha = Materials::AlphaMode::Opaque;
            return D;
        }
    };
    const IW7Materials Adapter;
    const Materials::AdapterRegistration Registered("IW7", Adapter);
}
