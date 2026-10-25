#include "stdafx.h"

// Modern Warfare (2019)'s live readers, as the common code reaches them (ICoDGame.h).

#include "ICoDGame.h"
#include "GameModernWarfare4.h"
#include "SABSupport.h"
#include "XPAKSupport.h"

namespace
{
    class GameModernWarfare4Live final : public ICoDGame
    {
    public:
        const char* Engine() const override { return "IW8"; }
        std::vector<PackageMark> Packages() const override { return { { PackageMark::Container::Sab, 0xA }, { PackageMark::Container::Xpak, 0xD } }; }
        const char* GetExportFolderName() const override { return "modern_warfare_4"; }
        std::unique_ptr<XAnim_t> ReadXAnim(const CoDAnim_t* Animation) const override
        {
            return GameModernWarfare4::ReadXAnim(Animation);
        }
        std::unique_ptr<XModel_t> ReadXModel(const CoDModel_t* Model) const override
        {
            return GameModernWarfare4::ReadXModel(Model);
        }
        std::unique_ptr<XImageDDS> ReadXImage(const CoDImage_t* Image) const override
        {
            return GameModernWarfare4::ReadXImage(Image);
        }
        std::unique_ptr<XImageDDS> ReadFileImage(const CoDImage_t* Image) const override
        {
            return XPAKSupport::ReadImageFile(Image);
        }
        std::unique_ptr<XSound> ReadXSound(const CoDSound_t* Sound) const override
        {
            if (Sound->IsFileEntry)
                return SABSupport::LoadOpusSound(Sound, SABSupport::OpusPreSkipPacket);
            return GameModernWarfare4::ReadXSound(Sound);
        }
        void LoadStreamedXModel(const std::unique_ptr<XModel_t>& Model, const XModelLod_t& ModelLOD, const std::unique_ptr<WraithModel>& ResultModel) const override
        {
            GameModernWarfare4::LoadXModel(ModelLOD, ResultModel);
        }
        NotetrackFormat GetNotetrackFormat() const override { return NotetrackFormat::Standard; }
        DeltaBitness GetDeltaBitness() const override { return DeltaBitness::Bits64; }
    };

    const GameModernWarfare4Live Game;
    const LiveGameRegistration Registered(Game);
}
