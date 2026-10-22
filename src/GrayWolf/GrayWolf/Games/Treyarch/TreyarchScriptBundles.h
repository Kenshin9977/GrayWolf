#pragma once

// The bundles kind for Black Ops 4 and Cold War: a scriptbundle (what scripts read with
// getscriptbundle) as JSON. Both games keep the same tree, laid out apart (acts' bo4 and cw
// scriptbundle unlinkers): ScriptBundle {name, settings type, objects}, objects {u64 count, entries,
// u64 sub count, subs}; an entry {key hash, value hash, key's script hash u32, script string u32,
// type u32, int or float}; a sub {its key, u64 count, that many objects}.
// Black Ops 4: XHash is 16 bytes (bundle 0x40, entry 0x30, sub 0x18 keyed by a script string);
// Cold War: 8 (bundle 0x30, entry 0x20, sub 0x20 keyed by a hash and a script hash).

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Treyarch
{
    enum class BundleLayout { Bo4, ColdWar };

    // What the reader reads a zone through.
    struct BundleZone
    {
        const std::vector<uint8_t>* Image = nullptr;
        std::function<int64_t(uint64_t Address)> Off;               // zone offset, -1 outside
        std::function<std::string(uint32_t Index)> ScriptString;     // the zone's script string, ""
        std::function<std::string(uint64_t Hash)> HashName;          // a hash's name, "" unknown
        uint64_t HashMask = ~0ull;
    };

    // The scriptbundle at Pos as JSON {"name", "type", "fields"} and its settings type's name (the
    // folder it is written in); a value written as its kind (a string, a number, "model#<name>",
    // "enum:3"...), a key by its name when an index knows its hash, else #<hash>. Json is left empty
    // for a bundle with no objects (a name-only header); false when its objects are not in the zone.
    // Reads: the bundle's reads (position, size) in walk order: a block whose pointer the zone keeps
    // as -1 (Cold War's subs) is the next read of its size
    bool ReadScriptBundle(const BundleZone& Z, BundleLayout L, int64_t Pos, const std::vector<std::pair<int64_t, int64_t>>& Reads,
                          std::string& Type, std::string& Json);
}
