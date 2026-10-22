#include "stdafx.h"

// Vanguard's live readers, as the common code reaches them (ICoDGame.h).

#include "ICoDGame.h"
#include "GameVanguard.h"
#include "SABSupport.h"

namespace
{
    class GameVanguardLive final : public ICoDGame
    {
    public:
        const char* Engine() const override { return "S4"; }
        std::vector<PackageMark> Packages() const override { return { { PackageMark::Container::Sab, 0x11 } }; }
        const char* GetExportFolderName() const override { return "vanguard"; }
        std::unique_ptr<XAnim_t> ReadXAnim(const CoDAnim_t* Animation) const override
        {
            return GameVanguard::ReadXAnim(Animation);
        }
        std::unique_ptr<XModel_t> ReadXModel(const CoDModel_t* Model) const override
        {
            return GameVanguard::ReadXModel(Model);
        }
        std::unique_ptr<XSound> ReadXSound(const CoDSound_t* Sound) const override
        {
            if (Sound->IsFileEntry)
                return SABSupport::LoadOpusSound(Sound, SABSupport::OpusPreSkipUnmeasured);
            return GameVanguard::ReadXSound(Sound);
        }
        void LoadStreamedXModel(const std::unique_ptr<XModel_t>& Model, const XModelLod_t& ModelLOD, const std::unique_ptr<WraithModel>& ResultModel) const override
        {
            GameVanguard::LoadXModel(Model, ModelLOD, ResultModel);
        }
        DeltaBitness GetDeltaBitness() const override { return DeltaBitness::Bits64; }
        NotetrackFormat GetNotetrackFormat() const override { return NotetrackFormat::VG; }
        BlendShapeFormat GetBlendShapeFormat() const override { return BlendShapeFormat::MW; }
    };

    const GameVanguardLive Game;
    const LiveGameRegistration Registered(Game);
}
