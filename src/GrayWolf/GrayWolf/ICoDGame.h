#pragma once

// A game's live readers, as the common code sees them: each game plugs its own in from its folder
// (Games/<ENGINE>/LiveGame.cpp, a LiveGameRegistration), and the common code reaches the one being
// read through CoDAssets::Game, never by naming it. Translator-internal switches stay private to
// the translators, which query the behavior-bucket enums below instead. Unhandled cases default to
// nullptr/false/empty/None.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "CoDAssets.h"
#include "WraithModel.h"

// Behavior buckets for translator-internal dispatches.

// CoDXAnimTranslator::TranslateXAnim[Reader]'s viewmodel relative-tag switch.
enum class ViewmodelBoneConvention { None, Standard /*j_gun*/, BO3BO4 /*tag_weapon*/, InfiniteWarfare };
// CoDXAnimTranslator's three "Delta *, per-game" switches (translation/2D rot/3D rot all key
// off the same 32/64-bit grouping).
enum class DeltaBitness { None, Bits32, Bits64 };
// CoDXAnimTranslator's "Stage 11: Notifications" switch.
enum class NotetrackFormat { None, Standard, BO3, BO4, CW, VG };
// CoDXAnimTranslator's "Stage 12: Blendshapes" switch.
enum class BlendShapeFormat { None, BO4, MW };
// CoDXModelTranslator's "Setup weights, game specific code" switch.
enum class VertexWeightsVariant { None, A, B, C };
// CoDXModelTranslator's "Unpack normal (Game specific)" switch.
enum class NormalUnpackVariant { None, A, B, C };

// A package file a game's are recognised by (the `file` mode): its container and version; for a
// sound bank, its entry size too (0: any); for an .iwd, a word of its folder ("": any). A mark
// that names its entry size or folder wins over one that does not.
struct PackageMark
{
    enum class Container { Sab, Xpak, Ipak, Iwd };
    Container Kind;
    uint32_t Version = 0;
    uint32_t EntrySize = 0;
    const char* Folder = "";
};

class ICoDGame
{
public:
    virtual ~ICoDGame() = default;

    // The game's engine, as its Games/<ENGINE> folder and IDiskGame::Engine() name it.
    virtual const char* Engine() const = 0;
    // The file mode's folder under exported_files/. Empty: none.
    virtual const char* GetExportFolderName() const { return ""; }
    // The package files that are this game's (the file mode reads them through its readers).
    virtual std::vector<PackageMark> Packages() const { return {}; }

    // Per-asset reads (LiveExport.h).
    virtual std::unique_ptr<XAnim_t> ReadXAnim(const CoDAnim_t* Animation) const { return nullptr; }
    virtual std::unique_ptr<XModel_t> ReadXModel(const CoDModel_t* Model) const { return nullptr; }
    // Live in-memory image read (Image->IsFileEntry == false).
    virtual std::unique_ptr<XImageDDS> ReadXImage(const CoDImage_t* Image) const { return nullptr; }
    // File-package image read (IsFileEntry == true; dispatches to IWD/IPAK/XPAKSupport).
    virtual std::unique_ptr<XImageDDS> ReadFileImage(const CoDImage_t* Image) const { return nullptr; }
    virtual std::unique_ptr<XSound> ReadXSound(const CoDSound_t* Sound) const { return nullptr; }
    // Its sounds are written as read, never transcoded (the file mode names them .wav).
    virtual bool SoundsAsRead() const { return false; }

    // LiveExport::ExportModel's hitbox switch (via the shared CoDXModelTranslator::TranslateXModelHitbox).
    virtual std::unique_ptr<WraithModel> TranslateHitboxModel(const std::unique_ptr<XModel_t>& GenericModel) const { return nullptr; }

    // CoDXModelTranslator's streamed-model switch. Games whose GameXXX::LoadXModel takes only
    // (ModelLOD, ResultModel) simply ignore the Model parameter.
    virtual void LoadStreamedXModel(const std::unique_ptr<XModel_t>& Model, const XModelLod_t& ModelLOD, const std::unique_ptr<WraithModel>& ResultModel) const {}
    // The model reader builds the skeleton itself: CoDXModelTranslator reads no bone matrices.
    virtual bool ReadsOwnBones() const { return false; }
    // Its model translation (LoadStreamedXModel and all it reads) may run on several threads at once:
    // a disk export then translates the LODs on the pool writing the models' files (ModelFilesOnPool)
    virtual bool ReadsMeshesInParallel() const { return false; }

    // Translator-internal behavior buckets (see enums above).
    virtual ViewmodelBoneConvention GetViewmodelBoneConvention() const { return ViewmodelBoneConvention::None; }
    virtual DeltaBitness GetDeltaBitness() const { return DeltaBitness::None; }
    virtual NotetrackFormat GetNotetrackFormat() const { return NotetrackFormat::None; }
    virtual BlendShapeFormat GetBlendShapeFormat() const { return BlendShapeFormat::None; }
    virtual VertexWeightsVariant GetVertexWeightsVariant() const { return VertexWeightsVariant::None; }
    virtual NormalUnpackVariant GetNormalUnpackVariant() const { return NormalUnpackVariant::None; }
};

// One per game, a static object beside its ICoDGame (Games/<ENGINE>/LiveGame.cpp).
struct LiveGameRegistration
{
    explicit LiveGameRegistration(const ICoDGame& Game);
};

namespace LiveGames
{
    // The game of an engine; throws std::out_of_range when none registered it (a programming error).
    const ICoDGame& Get(const std::string& Engine);
    // The game whose package files these are (Folder: the file's folder, lower case, for an .iwd),
    // nullptr if none.
    const ICoDGame* OwnerOf(PackageMark::Container Kind, uint32_t Version, uint32_t EntrySize = 0, const std::string& Folder = "");
}
