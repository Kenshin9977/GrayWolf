#include "stdafx.h"

// Modern Warfare 4's readers, as the common code reaches them (ICoDGame.h). Read from its files only
// (Games/MW4/DiskGame.cpp serves a zone to them): no running-game session. Models, materials and
// images are Black Ops 7's layouts (GameMW4.h), sounds and anims its own.

#include "ICoDGame.h"
#include "Games/BO7/GameBlackOps7.h"
#include "GameMW4.h"

namespace
{
    class GameMW4Live final : public ICoDGame
    {
    public:
        const char* Engine() const override { return "MW4"; }
        bool ReadsOwnBones() const override { return true; }
        bool ReadsMeshesInParallel() const override { return true; }
        // (the engine's own numbering, as MWIII's modern_warfare_6: Modern Warfare 2019's is _4)
        const char* GetExportFolderName() const override { return "modern_warfare_7"; }
        std::unique_ptr<XModel_t> ReadXModel(const CoDModel_t* Model) const override { return GameBlackOps7::ReadXModel(Model); }
        std::unique_ptr<XImageDDS> ReadXImage(const CoDImage_t* Image) const override { return GameBlackOps7::ReadXImage(Image); }
        std::unique_ptr<XSound> ReadXSound(const CoDSound_t* Sound) const override { return GameMW4::ReadXSound(Sound); }
        std::unique_ptr<XAnim_t> ReadXAnim(const CoDAnim_t* Animation) const override { return GameMW4::ReadXAnim(Animation); }
        void LoadStreamedXModel(const std::unique_ptr<XModel_t>& Model, const XModelLod_t& ModelLOD, const std::unique_ptr<WraithModel>& ResultModel) const override
        {
            GameBlackOps7::LoadXModel(Model, ModelLOD, ResultModel);
        }
        DeltaBitness GetDeltaBitness() const override { return DeltaBitness::Bits64; }
    };

    const GameMW4Live Game;
    const LiveGameRegistration Registered(Game);

    // Its campaign (sp26-cod.exe, profile mw4sp): the same readers but the anim header's, which is
    // Black Ops 7's (rate 0x14, last frame 0x26, bone counts 0x2c: the counts' sums hold on 395 of 395
    // xanims of sp_rex_escalation_entities, 335 of 337 of sp_rex_kill_archer_entities; at the
    // multiplayer's offsets on none) in the multiplayer's 0x90 struct (its tables are mw4's)
    class GameMW4CampaignLive final : public ICoDGame
    {
    public:
        const char* Engine() const override { return "MW4SP"; }
        bool ReadsOwnBones() const override { return true; }
        bool ReadsMeshesInParallel() const override { return true; }
        const char* GetExportFolderName() const override { return "modern_warfare_7"; }
        std::unique_ptr<XModel_t> ReadXModel(const CoDModel_t* Model) const override { return GameBlackOps7::ReadXModel(Model); }
        std::unique_ptr<XImageDDS> ReadXImage(const CoDImage_t* Image) const override { return GameBlackOps7::ReadXImage(Image); }
        std::unique_ptr<XSound> ReadXSound(const CoDSound_t* Sound) const override { return GameMW4::ReadXSound(Sound); }
        std::unique_ptr<XAnim_t> ReadXAnim(const CoDAnim_t* Animation) const override { return GameBlackOps7::ReadXAnim(Animation); }
        void LoadStreamedXModel(const std::unique_ptr<XModel_t>& Model, const XModelLod_t& ModelLOD, const std::unique_ptr<WraithModel>& ResultModel) const override
        {
            GameBlackOps7::LoadXModel(Model, ModelLOD, ResultModel);
        }
        DeltaBitness GetDeltaBitness() const override { return DeltaBitness::Bits64; }
    };

    const GameMW4CampaignLive Campaign;
    const LiveGameRegistration RegisteredCampaign(Campaign);
}
