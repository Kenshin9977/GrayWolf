#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Which model an anim plays on (its grid cell, the Viewport), chosen by bone names: the rig holding
// most of the anim's bones, a listed model completed by a second one when short (hands + weapon, a
// body + its head, joined by LoadRigGeometry), each model scored with what completes it. Each game's reference hands and body (zonekit/profiles/preview_rigs.json) are
// exported once from its install into %TEMP%\greyhound_rigs\<game>\ so an anim always has a model;
// an anim its name says is of a kind (a zombie's, a dog's: the table's "kinds") plays on a model of
// that kind, the game's reference of it ("zombie": body + head) or a listed one. Before any of
// that, an anim its game's export gave owners (anims/anim_owners.json, AnimOwners.h) plays on its
// first owner's models found in the browser's models, else exported from the install like a reference.
namespace AnimRigs
{
    // What an anim plays on: one model, or several joined (Paths[0] the skeleton the others join; a
    // reference body with its game's reference head).
    struct Pick
    {
        std::vector<std::string> Paths;   // empty: nothing fits
        float Score = 0;                  // the anim's bones the rig has, 0..1 (of those some model of its game has, and those none has it shapes)
        float Unmodelled = 0;             // of those, the share no model has (the anim shapes them: Index)
        bool OwnModelAbsent = false;      // Unmodelled so high the anim was made for a model not here (its references are asked for)
        bool Manual = false;              // the user's choice (SetManual)
        bool Reference = false;           // a game's reference model is part of it
        std::string Label;                // "hands + weapon"
        std::string Owner;                // the owner the game's data gives it (anim_owners.json: "aitype:zombie_dog"), "" when scored
    };

    // One model as a single rig for an anim, for the user's choice
    struct Candidate
    {
        std::string Path, Name;
        float Score = 0;
        bool Reference = false;
    };

    struct ModelRef
    {
        std::string Path, Name;   // the file, its name without extension
    };

    // Score: the anim's bones the rig has / the anim's bones some model of its game has (a bone
    // no listed or reference model of the game has costs every rig alike when the anim moves it, unless
    // it is optional: the table's optional_joints (a tag, a sleeve's helpers), or a joint of a variant of
    // the rig's skeleton, another game's model of it); a
    // rig of two models pays the 4% the second has to add. Ties: the anim's own game, the kind
    // its name says (vm_, viewmodel: hands; pb_, mp_, body: a body), the bind pose (of the rigs left
    // tied, the one whose body binds the anim's first frame is nearest, by more than 30 degrees), common
    // words, fewer bones left still.
    class Index
    {
    public:
        Index();
        ~Index();
        Index(const Index&) = delete;
        Index& operator=(const Index&) = delete;

        // The browser's models, once listed whole (picks wait until the first call); a new Version
        // has picks made again when next asked
        void SetModels(std::vector<ModelRef> Models, uint64_t Version);
        // Anim's rig when known (true), else false and its pick is queued; never blocks. An older
        // pick is still given while a new one is made.
        bool PickFor(const std::string& AnimPath, Pick& Out);
        // Every model fitting Anim by its own score, best first (false while not known)
        bool Ranked(const std::string& AnimPath, std::vector<Candidate>& Out);
        // The user's model for Anim (empty: back to the automatic pick)
        void SetManual(const std::string& AnimPath, const std::string& ModelPath);
        // Bumped when a pick may have changed: a consumer asks again
        uint64_t Generation() const;
        // Off: no reference model is exported (the ones already kept are used)
        void SetFetchReferences(bool On);
        // Blocks until the queued picks and reference exports are done (anim-rigs)
        void WaitIdle();
        std::string Stats() const;

    private:
        struct Impl;
        std::shared_ptr<Impl> P;
    };

    // A model's bone names (.semodel, .cast, .xmodel_export) without its mesh; an anim's (.seanim, .cast)
    bool ModelBoneNames(const std::string& Path, std::vector<std::string>& Out);
    bool AnimBoneNames(const std::string& Path, std::vector<std::string>& Out);
    // The game an exported file belongs to: the folder above its kind's (<out>/<game>/anims/x.seanim), "" when none
    std::string GameOf(const std::string& Path);
}
