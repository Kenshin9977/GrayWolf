#pragma once

// The anim owners (AnimOwners.h) of the Infinity Ward zones Ghosts to Infinite Warfare, from the
// zones' own assets: weapon:<w> (hands, view model; its xanims, its attachments', IW's anim
// packages'), animtree:<t> (an .atr on its tree's models, Layout::Trees), character:<script> (AW's
// compiled player characters), animclass:<n> (its states' script strings), playeranim:<n> (IW).
// A zone gives its facts as documents (Documents); Fill reads the run's zones and the zones the game
// loads with each (Layout::Shared*), specific owners first, MaxOwners at most per anim.

#include "DiskGame.h"
#include "ZoneTableWalker.h"

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace AnimOwners { struct Book; }

namespace IwZoneAnimOwners
{
    constexpr size_t MaxOwners = 8;

    // What a zone gives the documents reader (its protected accessors, from the game's zone class)
    struct ZoneView
    {
        const std::vector<uint8_t>* Image = nullptr;
        const ZoneTables::ZoneResult* Walk = nullptr;
        std::function<int64_t(uint64_t)> Off;                              // a relocated pointer's zone offset, -1 elsewhere
        std::function<std::string(int64_t)> Text;                          // the string pointer field at a zone offset
        std::function<std::string(const std::string&, int64_t)> Named;     // an asset's name by type and position (NamedAsset)
        std::function<bool(int64_t, std::string&, std::vector<uint8_t>&)> Rawfile;   // a rawfile asset's name and bytes
        std::function<bool(uint64_t, uint64_t, std::vector<uint8_t>&)> Bytes;   // N bytes at a relocated address
        const std::vector<std::string>* Slots = nullptr;                   // the zone's script strings by slot
    };

    struct Layout
    {
        std::string AnimType = "xanim";      // Infinite Warfare: xanimparts
        std::string ModelType = "xmodel";
        // from the weapon asset to its view model (the models array's first) and its hands: each
        // offset but the last a pointer followed, the last the model pointer's field
        std::vector<int64_t> Gun, Hands;
        // a composite model (WWII's: its flag at CompositeFlagAt, -1 none) given as its parts: Count (u8)
        // entries of Entry bytes at the pointer at List, each its part model at 0
        int64_t CompositeFlagAt = -1, CompositeList = 8, CompositeCount = 0x2a, CompositeEntry = 0x30;
        uint32_t CompositeFlag = 0;
        // a tree's models, body first, and where the game's data says so
        struct Tree
        {
            std::vector<std::string> Models;
            std::string Source;
        };
        std::map<std::string, Tree> Trees;
        // the trees (and IW's player anim sets) the player's body plays
        std::vector<std::string> PlayerTrees = { "multiplayer" };
        // the trees a level's AI plays (its AI types are scripts): on the level's own soldier, its first
        // body_* and head_* models (a stand-in for the AI type), character:<body>
        std::vector<std::string> LevelTrees;
        // compiled scripts read as player characters: a small one whose strings hold a model of one
        // of these prefixes (its body) and a head; none: not read
        std::vector<std::string> CharacterBodies;
        // the zones the game loads with a zone, the first overriding: patch_<zone>, then these
        // (multiplayer's for a zone mp_* / *_mp, else single player's)
        std::vector<std::string> SharedMp, SharedSp;
        // more for a zone whose name holds a word (alien: common_alien_mp), before those
        std::vector<std::pair<std::string, std::vector<std::string>>> SharedWhen;
    };

    // The zone's facts as documents ("animowners/<type>/<name>" lines, the animtrees' text)
    void Documents(const Layout& L, const ZoneView& V, std::vector<std::pair<std::string, std::string>>& Out);
    // The documents' name patterns (IDiskGame::AnimOwnerDocuments)
    std::vector<std::string> Patterns();
    // The run's anims' owners (IDiskGame::AnimOwners)
    void Fill(const DiskGame::IDiskGame& Game, const Layout& L, const DiskGame::ExportOptions& Options,
              const std::vector<DiskGame::IDiskGame::AnimZone>& Zones, ::AnimOwners::Book& Out);
}
