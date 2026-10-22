#pragma once

// Black Ops III's sound names by install: a bank entry's key (the hash of an alias's file spec) to
// the spec's path, e.g. "fr\vox\scripted\zod\vox_zod_crbt_robot_activated_0.SN85.pc.snd". The PC
// banks of the languages hold no name table, and a voice line's sound sits in one zone's bank
// (zm_common.fr.sabs) while its alias sits in a language zone (fr_zm_zod.ff): the names are read
// from the SndBanks of a set of the install's zones, the language zones (<locale>_*.ff for each
// zone/snd/<locale> folder: 132 zones, seconds) or the others (the campaign's common banks are
// named by the maps' aliases: 264 zones, minutes). Built once, the zones opened one after the
// other, and kept in a file (beside the install index, KeptFile) zone by zone: its key hashes the
// format and the reader's tables, each zone's record carries the zone's name, size and time. A
// zone read is appended at once, so a build that stops (cancelled, killed) resumes where it was,
// and a changed zone (or a new one) is the only one read again.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Bo3SoundNames
{
    using Table = std::unordered_map<uint32_t, std::string>;

    // The language zones beside ZoneDir's sound banks (Languages), or every other zone, sorted (full
    // paths, empty files left out)
    std::vector<std::string> Zones(const std::string& ZoneDir, bool Languages);

    struct Kept
    {
        std::shared_ptr<const Table> Names;   // never null
        bool Built = false;                   // some zone read by this call
        double Seconds = 0;                   // building or loading it
        size_t Zones = 0;                     // the zones it is made of
        size_t ZonesRead = 0, ZonesFailed = 0; // read by this call, those that did not open
        size_t Resumed = 0;                   // when built: the zones the file already held
    };

    // The table of these zones: in memory, else the zones' records in File, the zones it lacks read
    // now by Read (each in turn: its specs added, false when it does not open) and appended to File.
    // Before is called before the first zone is read (a zone in use parks its session there), After
    // once the reading is over; Step(Done, Count, Zone) reports it, false to stop (the zones read so
    // far are kept, the table is used as it is and asked again next time). A key in several zones
    // keeps the name of the first in Zones' order.
    Kept For(const std::vector<std::string>& Zones, const std::string& File, const std::string& ReaderKey,
             const std::function<bool(const std::string& Zone, Table& Out)>& Read, const std::function<void()>& Before,
             const std::function<void()>& After, const std::function<bool(size_t, size_t, const std::string&)>& Step);
}
