#include "stdafx.h"

#include "MaterialDescription.h"
#include "Games/IW/IwMaterialAlpha.h"

// Modern Warfare 4 materials: Black Ops 7's material struct (0x78, the same {semantic, image} slots),
// described as Black Ops 7's (Games/BO7/MaterialAdapter.cpp): the first layer's colour 0x47 (sRGB)
// and "nog" 0x48 (gloss r, normal y g, occlusion b, normal x a), its opacity map 0x4a, the alpha mode
// from the sort key. Not yet checked against Modern Warfare 4's shaders.
namespace
{
    constexpr uint32_t Color = 0x47, Nog = 0x48, OpacityMap = 0x4a;

    using Materials::Role;
    class ModernWarfare4Materials final : public Materials::SemanticAdapter
    {
    public:
        ModernWarfare4Materials()
            : SemanticAdapter("mw4", {
                  { Color, Role::Albedo },
                  { Nog, Role::Normal },
                  { Nog, Role::Gloss, 0 },
                  { Nog, Role::Occlusion, 2 },
              }, Unmatched::Names) {}
    protected:
        void Refine(const Materials::Source& M, Materials::Description& D, bool BySemantic) const override
        {
            if (BySemantic && !D.Normal.Empty()) D.NormalEnc = Materials::NormalEncoding::AG;
            IwMaterialAlpha::Apply(M, M.BySemantic(OpacityMap), D);
        }
    };
    const ModernWarfare4Materials Adapter;
    const Materials::AdapterRegistration Registered("MW4", Adapter);
}
