#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Which model an anim plays on (its grid cell, the Viewport), chosen by bone names: the listed model
// holding most of the anim's bones, completed by a second one when short (hands + weapon, joined by
// LoadRigGeometry). Each game's reference hands and body (zonekit/profiles/preview_rigs.json) are
// exported once from its install into %TEMP%\greyhound_rigs\<game>\ so an anim always has a model.
namespace AnimRigs
{
    // What an anim plays on: one model, or several joined (Paths[0] the skeleton the others join; a
    // reference body with its game's reference head).
    struct Pick
    {
        std::vector<std::string> Paths;   // empty: nothing fits
        float Score = 0;                  // the anim's bones the rig has, 0..1 (of those some model of its game has)
        bool Manual = false;              // the user's choice (SetManual)
        bool Reference = false;           // a game's reference model is part of it
        std::string Label;                // "hands + weapon"
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

    // Score: the anim's bones the model has / the anim's bones some model of its game has (a bone
    // no listed or reference model of the game has, like a sleeve's helper joints, is not counted). Ties: the anim's own game, the kind
    // its name says (vm_, viewmodel: hands; pb_, mp_, body: a body), common words, fewer bones left still.
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
