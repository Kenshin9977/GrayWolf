#pragma once

// The live readers of an original 32-bit MW2 / MW3 zone as the common code reaches them (ICoDGame.h):
// the family's readers (Games/IW4/Iw64Readers.h) with that build's Layout, the 32-bit translator
// variants (Call of Duty Online's: the same engine at the same word size), the title's export folder.
// Each title registers one under its own engine name (Games/IW4/LiveGameOriginal.cpp, IW5's).

#include "ICoDGame.h"
#include "GameWorldWar2.h"
#include "Games/IW4/Iw64Readers.h"
#include "IWDSupport.h"

namespace Iw64
{
    class OriginalLive final : public ICoDGame
    {
    public:
        OriginalLive(const char* Engine, const char* Folder, const Layout& L) : Name(Engine), FolderName(Folder), Readers(L) {}
        const char* Engine() const override { return Name; }
        const char* GetExportFolderName() const override { return FolderName; }
        std::unique_ptr<XImageDDS> ReadFileImage(const CoDImage_t* Image) const override { return IWDSupport::ReadImageFile(Image); }
        std::unique_ptr<XSound> ReadXSound(const CoDSound_t* Sound) const override { return GameWorldWar2::ReadXSound(Sound); }
        std::unique_ptr<XAnim_t> ReadXAnim(const CoDAnim_t* Animation) const override { return Iw64::ReadXAnim(Animation, Readers); }
        std::unique_ptr<XModel_t> ReadXModel(const CoDModel_t* Model) const override { return Iw64::ReadXModel(Model, Readers); }
        ViewmodelBoneConvention GetViewmodelBoneConvention() const override { return ViewmodelBoneConvention::Standard; }
        DeltaBitness GetDeltaBitness() const override { return DeltaBitness::Bits32; }
        NotetrackFormat GetNotetrackFormat() const override { return NotetrackFormat::Standard; }
        VertexWeightsVariant GetVertexWeightsVariant() const override { return VertexWeightsVariant::A; }
        NormalUnpackVariant GetNormalUnpackVariant() const override { return NormalUnpackVariant::A; }

    private:
        const char* Name;
        const char* FolderName;
        const Layout& Readers;
    };
}
