#include "stdafx.h"

// Modern Warfare 3's live readers, as the common code reaches them (ICoDGame.h).

#include "ICoDGame.h"
#include "GameWorldWar2.h"
#include "Games/IW4/Iw64Readers.h"
#include "IWDSupport.h"

namespace
{
    class GameModernWarfare3Live final : public ICoDGame
    {
    public:
        const char* Engine() const override { return "IW5"; }
        std::vector<PackageMark> Packages() const override { return { { PackageMark::Container::Iwd, 0, 0, "modern warfare 3" } }; }
        const char* GetExportFolderName() const override { return "modern_warfare_3"; }
        std::unique_ptr<XImageDDS> ReadFileImage(const CoDImage_t* Image) const override
        {
            return IWDSupport::ReadImageFile(Image);
        }
        std::unique_ptr<XSound> ReadXSound(const CoDSound_t* Sound) const override
        {
            return GameWorldWar2::ReadXSound(Sound);
        }
        // the 64-bit rebuild's zones, served from disk (Games/IW4/Iw64Readers.h)
        std::unique_ptr<XAnim_t> ReadXAnim(const CoDAnim_t* Animation) const override
        {
            return Iw64::ReadXAnim(Animation, Iw64::Mw3);
        }
        std::unique_ptr<XModel_t> ReadXModel(const CoDModel_t* Model) const override
        {
            return Iw64::ReadXModel(Model, Iw64::Mw3);
        }
        ViewmodelBoneConvention GetViewmodelBoneConvention() const override { return ViewmodelBoneConvention::Standard; }
        DeltaBitness GetDeltaBitness() const override { return DeltaBitness::Bits64; }
        NotetrackFormat GetNotetrackFormat() const override { return NotetrackFormat::Standard; }
        VertexWeightsVariant GetVertexWeightsVariant() const override { return VertexWeightsVariant::B; }
        NormalUnpackVariant GetNormalUnpackVariant() const override { return NormalUnpackVariant::A; }
    };

    const GameModernWarfare3Live Game;
    const LiveGameRegistration Registered(Game);
}
