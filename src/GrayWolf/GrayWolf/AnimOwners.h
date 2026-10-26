#pragma once

// Which models an exported anim plays on, as the game's own data says (an AI type and its
// character, an animtree, an archetype): <out>/<game>/anims/anim_owners.json, format
// "graywolf-anim-owners" 1 { game, owners: { id: { models, source } }, anims: { anim: [ids] } }.
// A game fills a Book through IDiskGame::AnimOwners (DiskGame.h); the anim preview
// (AssetGrid/AnimRigs.cpp) reads it before it guesses by bones. One writer for every game.

#include <map>
#include <string>
#include <vector>

namespace AnimOwners
{
    struct Owner
    {
        // as the model kind names them (their folder), the body first, then the head(s) it wears
        std::vector<std::string> Models;
        std::string Source;                // "aitype/zombie_dog.gsc -> character/character_sp_zombie_dog.gsc"
    };

    struct Book
    {
        // by owner id: "aitype:<name>", "mptype:<name>", "animtree:<name>", "archetype:<name>"...
        std::map<std::string, Owner> Owners;
        std::map<std::string, std::vector<std::string>> Anims;     // anim name as exported -> owner ids, in the game's order

        // Anim is played by Id (added once, after the owners already given)
        void Own(const std::string& Anim, const std::string& Id);
        bool Empty() const { return Anims.empty(); }
        // Only the anims Keep holds (compared without case), and the owners they name
        void KeepOnly(const std::vector<std::string>& Keep);
    };

    // anim_owners.json in an anims folder
    std::string FileIn(const std::string& AnimsDir);

    // The file's book (its anims' keys as written); false when it does not read. Game: its "game".
    bool Read(const std::string& File, Book& Out, std::string* Game = nullptr);

    // New merged into AnimsDir's file: an anim it gives replaces the earlier entry; an owner it gives
    // again keeps its earlier models and adds the new ones (union, earlier first), its source
    // replaced; the others stay; owners no anim names are dropped. Written to a temporary file, then moved over
    // the old one. False and Why when it is not written.
    bool Merge(const std::string& AnimsDir, const std::string& Game, const Book& New, std::string* Why = nullptr);
}
