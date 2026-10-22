#include "stdafx.h"

// Modern Warfare II (2022)'s live readers, as the common code reaches them (ICoDGame.h).

#include "ICoDGame.h"
#include "GameModernWarfare5.h"
#include "SABSupport.h"
#include "XPAKSupport.h"

namespace
{
    class GameModernWarfare5Live final : public ICoDGame
    {
    public:
        const char* Engine() const override { return "IW9"; }
        std::vector<PackageMark> Packages() const override { return { { PackageMark::Container::Sab, 0x15, 56 } }; }
        bool ReadsOwnBones() const override { return true; }
        const char* GetExportFolderName() const override { return "modern_warfare_5"; }
        std::unique_ptr<XAnim_t> ReadXAnim(const CoDAnim_t* Animation) const override
        {
            return GameModernWarfare5::ReadXAnim(Animation);
        }
        std::unique_ptr<XModel_t> ReadXModel(const CoDModel_t* Model) const override
        {
            return GameModernWarfare5::ReadXModel(Model);
        }
        std::unique_ptr<XImageDDS> ReadXImage(const CoDImage_t* Image) const override
        {
            return GameModernWarfare5::ReadXImage(Image);
        }
        std::unique_ptr<XImageDDS> ReadFileImage(const CoDImage_t* Image) const override
        {
            return XPAKSupport::ReadImageFile(Image);
        }
        std::unique_ptr<XSound> ReadXSound(const CoDSound_t* Sound) const override
        {
            if (Sound->IsFileEntry)
                return SABSupport::LoadOpusSound(Sound, SABSupport::OpusPreSkipUnmeasured);
            return GameModernWarfare5::ReadXSound(Sound);
        }
        void LoadStreamedXModel(const std::unique_ptr<XModel_t>& Model, const XModelLod_t& ModelLOD, const std::unique_ptr<WraithModel>& ResultModel) const override
        {
            GameModernWarfare5::LoadXModel(Model, ModelLOD, ResultModel);
        }
        DeltaBitness GetDeltaBitness() const override { return DeltaBitness::Bits64; }
    };

    const GameModernWarfare5Live Game;
    const LiveGameRegistration Registered(Game);
}
