#pragma once

// Anim owners (AnimOwners.h) for the modern IW titles (Modern Warfare 2019, Modern Warfare III): a
// zone writes its anim sets, characters and direct owners (a weapon's animation package) into one
// document while it is open; the game's AnimOwners joins every zone's by name. An anim of a set is
// owned by archetype:<set> (the models of the first character playing it); an anim's owners in the
// order Own gives. Keys: a name in lower case, or "#<hash>" where the game keeps hashes.

#include "DiskGame.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace IwAnimOwners
{
    // the document's name (IDiskGame::AnimOwnerDocuments' pattern)
    inline constexpr const char* DocumentName = "anim_owner_facts.tsv";

    struct Set
    {
        std::string Key;                        // the archetype's key
        std::string Source;                     // "aianimset juggernaut_lw"
        std::vector<std::string> Anims;         // their keys
    };
    struct Character
    {
        std::string Archetype, Name;            // its archetype's key, the archetype's name
        std::string Source;                     // "character c_jup_zmb_mangler"
        std::vector<std::string> Models;        // body, then heads (names as exported)
    };
    struct Direct
    {
        std::string Id, Source;                 // "weapon:iw8_ar_mike4"
        std::vector<std::string> Models, Anims; // Anims: their keys
    };
    struct Facts
    {
        std::vector<Set> Sets;
        std::vector<Character> Characters;
        std::vector<Direct> Directs;
        bool Empty() const { return Sets.empty() && Characters.empty() && Directs.empty(); }
    };

    // the facts as the document's text, and back
    std::string Write(const Facts& F);
    Facts Read(const std::string& Text);
    // the facts of the documents named DocumentName among Docs
    Facts FromDocuments(const std::vector<std::pair<std::string, std::string>>& Docs);

    // Anims' owners into Out, the facts of every source joined. Key: an exported anim's name -> the
    // key the facts name it by.
    void Own(const std::vector<const Facts*>& Sources, const std::vector<std::string>& Anims,
             const std::function<std::string(const std::string&)>& Key, AnimOwners::Book& Out);

    // The run's zones' facts (their documents) joined with those of Shared (the zones the game loads
    // with them, fast file paths; read only when an anim is left without an owner, each opened once
    // per process and file) into Out
    void Fill(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& Options,
              const std::vector<DiskGame::IDiskGame::AnimZone>& Zones, const std::function<std::vector<std::string>()>& Shared,
              const std::function<std::string(const std::string&)>& Key, AnimOwners::Book& Out);

    // A name a string cipher table gets some bytes of wrong (a gap in it): the one name Known takes
    // that differs from Text at one byte, or at two bytes that are not [a-z0-9_]; else Text
    std::string Repaired(const std::string& Text, const std::function<bool(const std::string&)>& Known);
    // Text holds only [a-z0-9_] (an asset or archetype name as these games write them)
    bool Plain(const std::string& Text);
}
