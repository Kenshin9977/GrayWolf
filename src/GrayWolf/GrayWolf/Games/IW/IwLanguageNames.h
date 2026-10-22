#pragma once

// A localized sound's name by its language. Black Ops 6 and 7 hash a sound by its path, codec,
// rate and language ("iw9/op/nicki/dx_op_niki_ping_..._opg2.snn.20.48000.english"), the language
// the zone's: Black Ops 7's operator voices are "english" in fra_global_stream_cfwd_mp (the name
// index's) and "english_safe" in ens_ (not in it). A hash no index knows is tried against every
// index name ending in one of the game's languages (or "all"), its last part replaced by each of
// them; a name is kept only when its hash is the asset's. No table, no cache: the stems (86 286 of
// the 2.1 M names Black Ops 7 loads, ~6 MB) are hashed once a run (~1 s), then 2.2 M FNV-1a tails
// a zone with an unknown sound (~0.3 s).

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Games/IW/IwctxReaders.h"

class IwLanguageNames
{
public:
    // Languages: the game's language names, read from its executable's language table
    explicit IwLanguageNames(std::vector<std::string_view> Languages);

    // The names found for Unknown (63-bit asset hashes, IwctxReaders::AssetHash), by hash
    std::unordered_map<uint64_t, std::string> Name(const std::unordered_set<uint64_t>& Unknown);
    // Names the sounds no index named (Found[k] "xsound_<hex>", its hash Hashes[k], bit 63 ignored);
    // how many it named
    size_t Rename(const std::vector<uint64_t>& Hashes, std::vector<IwctxReaders::SoundEntry>& Found);

private:
    struct Stem
    {
        uint64_t Hash;      // FNV-1a state after the stem ("<path>.<codec>.<rate>.")
        uint32_t At, Length;    // the stem in Pool (copied: CoDAssets::CleanUpGame clears the index)
    };
    std::string Pool;
    std::vector<std::string_view> Suffixes;    // the languages and "all"
    std::vector<Stem> Stems;
    std::once_flag Built;
    void Build();
};
