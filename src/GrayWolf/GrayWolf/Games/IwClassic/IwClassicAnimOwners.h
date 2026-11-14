#pragma once

// Anim owners (AnimOwners.h) of the Infinity Ward games up to MW3 (CoD1, CoD2, CoD4, MW2, MW3):
// weapon:<name> (its weapon file's *Anim / *AnimR / *AnimL fields, MW3's animOverrides) on its
// handModel then gunModel (the rig: the hands, the gun joined to them); aitype / mptype through
// the Treyarch helper (GscAnimOwners.h) plus IW's head forms (attach, attachFromArray, attachHead) and CoD1/CoD2's "xmodel/" prefix;
// MW3's compiled multiplayer characters (character:<body>) by their scripts' strings, a
// heuristic chosen over decompiling with gsc-tool on the export path.

#include "DiskGame.h"
#include "AnimOwners.h"
#include "Games/Treyarch/GscAnimOwners.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace IwClassicAnimOwners
{
    struct Facts
    {
        GscAnimOwners::Facts Gsc;
        std::map<std::string, ::AnimOwners::Owner> Weapons;            // weapon:<name> -> hands, gun
        std::map<std::string, std::vector<std::string>> WeaponAnims;   // anim (lower case) -> weapon ids, in file order
        std::map<std::string, std::string> Sources;                    // an owner's source the GSC helper does not say (MW3's compiled characters)
    };

    // the document names read (IDiskGame::AnimOwnerDocuments): GSC source, animtrees, MW3's compiled
    // scripts' strings (<n>.gscsource, as the rawfile kind names them)
    std::vector<std::string> Documents();
    // (name, text) documents -> facts; a weapon file (WEAPONFILE...) is one whatever its name, the
    // weapon named by its file name
    Facts Parse(const std::vector<std::pair<std::string, std::string>>& Docs);
    // Anims' owners into Out, the zone's own facts first, then what loads with it. An anim: the
    // weapons naming it (of the first source holding any; the gun named most like the anim first),
    // then its tree's types; owners with the same models given once, MaxOwners at most.
    void Own(const std::vector<const Facts*>& Sources, const std::vector<std::string>& Anims, ::AnimOwners::Book& Out);
    constexpr size_t MaxOwners = 8;

    // The heads each body wears as Docs' character scripts say (setModel / an xmodelalias of bodies,
    // attachHead / attachFromArray / attach("...head..."), MW3's compiled characters): body -> heads,
    // lower case, as the model kind names them (GscAnimOwners::BodyHeads). The char_heads setting's pairs.
    std::vector<std::pair<std::string, std::vector<std::string>>> BodyHeads(const std::vector<std::pair<std::string, std::string>>& Docs);
    // The same of a zone: its own documents (Own) first, then those of the zones loaded with it (as
    // Fill takes them: patch_<zone>; patch_mp, common_mp; or patch, common: a multiplayer map's
    // characters are common_mp's scripts), each read once per process by Open, Asking parked
    // meanwhile (one session at a time)
    std::vector<std::pair<std::string, std::vector<std::string>>> BodyHeads(
        DiskGame::IDiskZone& Asking, const std::string& Zone, const std::vector<std::pair<std::string, std::string>>& Own,
        const std::function<std::unique_ptr<DiskGame::IDiskZone>(const std::string& Path)>& Open);

    // A zonekit game (CoD4, MW2, MW3): per zone, its documents and weapon files (OatWeapons.h; a
    // zone OwnWeaponFiles says gives them among its documents, MW2's and MW3's original builds',
    // is not handed to OAT), then those of the zones loaded with it (patch_<zone>; patch_mp,
    // common_mp; or patch, common), each read once per process.
    void Fill(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& Options,
              const std::vector<DiskGame::IDiskGame::AnimZone>& Zones, ::AnimOwners::Book& Out,
              const std::function<bool(const std::string& Zone)>& OwnWeaponFiles = {});
}
