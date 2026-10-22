#pragma once

// The weapons and weaponstats kinds of the games OpenAssetTools reads (CoD4, World at War, MW2, MW3,
// Black Ops, Black Ops II), for their OAT readers (--game <id>-oat) and their zonekit ones alike:
// the zone loaded by OAT (OatInProcess.h), its weapons as OAT's dumpers write them, the mod tools'
// weapon file (an InfoString), read into the table as CoD1's and CoD2's own weapon files are
// (Games/Common/WeaponFiles.h: per-zone facts, the table rebuilt from all of them).

#include "DiskGame.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class OatZoneSession;

namespace OatWeapons
{
    // A zone's weapon files through OAT: a session it opens itself on the fast file the first time
    // they are asked (a zonekit reader's zone), or one it is lent (an OAT reader's)
    class Source
    {
    public:
        explicit Source(std::string FastFile);
        explicit Source(OatZoneSession* Lent);
        ~Source();
        Source(const Source&) = delete;
        Source& operator=(const Source&) = delete;

        // (path in OAT's tree, contents), those of the weapon, attachment, attachmentunique and camo
        // assets the zone defines; Assets their count. False with Error when OAT does not load the zone.
        bool Files(const std::vector<std::pair<std::string, std::string>>*& Out, size_t& Assets, std::string& Error);
        // its localized strings (key, text in the game's bytes) and the zone's code page
        bool Strings(std::vector<std::pair<std::string, std::string>>& Out, unsigned& CodePage, std::string& Error);
        // the string tables of these names (lower case, "mp/attachmentcombos.csv") the zone defines, as CSV text
        bool Tables(const std::vector<std::string>& Names, std::map<std::string, std::string>& Out, std::string& Error);

    private:
        OatZoneSession* Session(std::string& Error);
        std::string Path;
        std::unique_ptr<OatZoneSession> Owned;
        OatZoneSession* Lent = nullptr;
        bool Tried = false, Listed = false;
        std::string OpenError;
        std::vector<std::pair<std::string, std::string>> Cache;
        size_t CachedAssets = 0;
    };

    // The weapons kind: OAT's weapon files under <out>/<game>/weapons/ (weapons/<name>, the mod tools'
    // tree; attachment/, attachmentunique/, camo/, accuracy/ beside them under weapons/), filtered by name
    void ExportWeapons(Source& S, const std::string& Zone, const DiskGame::Context& C, DiskGame::Totals& T);
    // The weaponstats kind: this zone's facts, then the table rebuilt once the run's zones are written
    // (Context::After). Title: the game, for the README.
    void ExportWeaponStats(Source& S, const std::string& Zone, const DiskGame::Context& C, DiskGame::Totals& T, const char* Title);
}
