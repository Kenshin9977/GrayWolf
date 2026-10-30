#include "stdafx.h"

#include "MaterialDescription.h"
#include "Games/Treyarch/TreyarchMaterials.h"

#include <string>

// Black Ops 4 materials: Treyarch's semantics (Games/Treyarch/TreyarchMaterials.h).
// Hair: the tint gradient (1C3FEDDD), and a colour mixed in by a mask, as the technique set's
// shader reads them: the bo4 profile's shading.txt and the constant blob.
namespace
{
    constexpr uint32_t TintGradient = 0x1C3FEDDD;

    int ChannelOf(const std::string& C) { const size_t I = std::string("rgba").find(C.empty() ? 'r' : C[0]); return I == std::string::npos ? 0 : (int)I; }

    void DescribeTint(const Materials::Source& M, Materials::Description& D)
    {
        if (M.Techset.empty() || M.Constants.empty()) return;
        const Materials::Recipe* R = Materials::ShadingRecipe("bo4", "0x" + M.Techset);
        if (R == nullptr) return;
        const std::vector<uint8_t>& Blob = M.Constants[0];
        const auto Field = [&](const char* K) { const auto It = R->find(K); return It == R->end() ? std::string() : It->second; };
        // a colour mixed in by a mask (color), then the gradient (tint)
        const std::string MaskSemantic = Field("tint_mask");
        if (const Materials::Source::Image* Mask = MaskSemantic.empty() ? nullptr : M.BySemantic((uint32_t)std::stoul(MaskSemantic, nullptr, 16)))
        {
            D.Color.Mask = { Mask->Path, ChannelOf(Field("tint_mask_channel")), false };
            for (int c = 0; c < 3; c++) D.Color.Color[c] = Materials::RecipeFloat(*R, "tint_color", Blob, c, 1.0f);
        }
        const Materials::Source::Image* Gradient = M.BySemantic(TintGradient);
        if (Gradient == nullptr || Field("scale").empty()) return;
        auto& T = D.Tint;
        T.Image.Path = Gradient->Path;
        T.Rgb = Field("channel") == "rgb";
        T.Image.Channel = T.Rgb ? 0 : ChannelOf(Field("channel"));
        T.U = 0.5f;
        const std::string OffsetMap = Field("offset_map");
        if (const Materials::Source::Image* Offset = OffsetMap.empty() ? nullptr : M.BySemantic((uint32_t)std::stoul(OffsetMap, nullptr, 16)))
        {
            T.Offset = { Offset->Path, ChannelOf(Field("offset_channel")), false };
            T.OffsetScale = Materials::RecipeFloat(*R, "scale", Blob, 0, 0.0f);
        }
        if (!T.Rgb)
            for (int c = 0; c < 3; c++) T.Color[c] = Materials::RecipeFloat(*R, "color", Blob, c, 1.0f);
    }

    class T8Materials final : public Materials::SemanticAdapter
    {
    public:
        T8Materials() : SemanticAdapter("bo4", TreyarchMaterials::Table()) {}
    protected:
        void Refine(const Materials::Source& M, Materials::Description& D, bool) const override { DescribeTint(M, D); }
    };
    const T8Materials Adapter;
    const Materials::AdapterRegistration Registered("T8", Adapter);
}
