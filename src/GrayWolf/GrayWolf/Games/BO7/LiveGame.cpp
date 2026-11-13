#include "stdafx.h"

// Black Ops 7's readers, as the common code reaches them (ICoDGame.h). Read from its files only
// (Games/BO7/DiskGame.cpp serves a zone to them): no running-game session.

#include "ICoDGame.h"
#include "GameBlackOps7.h"

namespace
{
    class GameBlackOps7Live final : public ICoDGame
    {
    public:
        const char* Engine() const override { return "BO7"; }
        bool ReadsOwnBones() const override { return true; }
        // GameBlackOps7::LoadXModel reads the served zone and the packages only (both shared reads)
        bool ReadsMeshesInParallel() const override { return true; }
        const char* GetExportFolderName() const override { return "black_ops_7"; }
        std::unique_ptr<XModel_t> ReadXModel(const CoDModel_t* Model) const override { return GameBlackOps7::ReadXModel(Model); }
        std::unique_ptr<XImageDDS> ReadXImage(const CoDImage_t* Image) const override { return GameBlackOps7::ReadXImage(Image); }
        std::unique_ptr<XSound> ReadXSound(const CoDSound_t* Sound) const override { return GameBlackOps7::ReadXSound(Sound); }
        std::unique_ptr<XAnim_t> ReadXAnim(const CoDAnim_t* Animation) const override { return GameBlackOps7::ReadXAnim(Animation); }
        void LoadStreamedXModel(const std::unique_ptr<XModel_t>& Model, const XModelLod_t& ModelLOD, const std::unique_ptr<WraithModel>& ResultModel) const override
        {
            GameBlackOps7::LoadXModel(Model, ModelLOD, ResultModel);
        }
        DeltaBitness GetDeltaBitness() const override { return DeltaBitness::Bits64; }
    };

    const GameBlackOps7Live Game;
    const LiveGameRegistration Registered(Game);
}
