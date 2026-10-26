#pragma once

// Anim owners of Black Ops 4 and Cold War (IDiskGame::AnimOwners, AnimOwners.h), read from the
// zones' own assets: an aitype's characters (body, head, their xmodelalias lists) and its
// animmappingtables (alias -> xanims), a weapon's view model and its two anim tables. Both games
// keep the same tree, laid out apart (Layout). A zone's facts are written as a document while it
// is open (DocumentName, IDiskZone::Documents); the zones loaded beside it (zm_common and the
// zombies maps: zm_common holds Black Ops 4's anims, its maps the AI types) are read once per
// install and kept beside the install index (<game>_<install>.animowners).
// An aitype's character or map defined in another zone (a name-only header here) is found by
// its name hash among the other zones' facts.

#include "DiskGame.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace TreyarchAnimOwners
{
    // Where the fields are. -1: not read.
    struct Layout
    {
        // aitype: its name (a string pointer, else a hash), 8 character pointers, 3 animmappingtables
        int64_t AiText = -1, AiHash = 0, Characters = 0, AnimMaps = 0;
        int CharacterSlots = 8, AnimMapSlots = 3;
        // character: its name hash, the body and head models and their xmodelalias lists
        int64_t CharacterHash = 0, Body = 0, BodyAlias = 0, Head = 0, HeadAlias = 0;
        // xmodelalias: its model pointers
        int64_t AliasModels = 0;
        int AliasSlots = 0;
        // animmappingtable: its name hash, entries (u32 count), each entry's xanims and their count;
        // an xanim a 16-byte name hash (AnimsHashed: Black Ops 4) or a pointer
        int64_t MapHash = 0, MapEntries = 0x10, MapCount = 0x18, EntrySize = 0, EntryAnims = 0, EntryCount = 0;
        bool AnimsHashed = false;
        int64_t AnimStride = 8;
        // weapon: its name hash, its view model (a pointer, or a list whose first is it), its anim tables
        int64_t WeaponHash = 0, GunModel = -1;
        std::vector<std::pair<int64_t, int>> WeaponAnims;
        // its attachments' own (a sight's aim, an extended magazine's reload): their list, and each one's anim tables
        const char* UniqueType = "attachment_unique";
        int64_t Uniques = -1;
        int UniqueSlots = 0;
        std::vector<std::pair<int64_t, int>> UniqueAnims;
    };

    // What the reader reads a zone through (the served, relocated zone).
    struct ZoneView
    {
        const std::vector<uint8_t>* Image = nullptr;
        std::function<int64_t(uint64_t Address)> Off;                    // zone offset, -1 outside
        std::function<std::vector<int64_t>(const char* Type)> Assets;   // the zone-level assets of a type
        std::function<std::string(int64_t Pos)> Model, Anim;             // an asset's name as exported
        std::function<std::string(uint64_t Hash)> AnimHash;              // an xanim's name from its hash
        std::function<std::string(uint64_t Hash, const char* Prefix)> Hashed;   // a name hash's name, <prefix>_<hex> unknown
        std::function<std::string(uint64_t Address)> Text;              // a string pointer's text
        uint64_t HashMask = ~0ull;
        std::string Key;   // what makes kept facts stale (the reader's tables)
    };

    // One zone's facts
    struct Owner
    {
        std::string Id, Source;
        std::vector<std::string> Models, Anims;   // its own (a weapon's)
        std::vector<uint64_t> Characters, Maps;   // by name hash (an aitype's)
    };
    struct Facts
    {
        std::string Key;
        std::vector<Owner> Owners;
        std::map<uint64_t, std::vector<std::string>> Characters;   // name hash -> body, heads
        std::map<uint64_t, std::vector<std::string>> Maps;         // name hash -> xanims
    };

    // the document a zone gives its facts as
    constexpr const char* DocumentName = "anim_owners.facts";

    Facts Read(const ZoneView& Z, const Layout& L);
    std::string Text(const Facts& F);
    Facts Parse(const std::string& Text);

    // The zones whose facts a zone's anims are owned from, beside its own: install zone names (as
    // ListZones gives them, without extension, '*' wildcards, no case; "!" leaves out) per zone name pattern
    using Family = std::vector<std::pair<std::string, std::vector<std::string>>>;

    // Every AnimZone's anims owned from its own facts (its document), then those of its family's
    // zones (opened once, kept per install)
    void Fill(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& Options, const Family& Shared,
              const std::vector<DiskGame::IDiskGame::AnimZone>& Zones, AnimOwners::Book& Out);
}
