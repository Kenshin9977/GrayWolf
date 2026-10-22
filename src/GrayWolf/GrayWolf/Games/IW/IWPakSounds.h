#pragma once

#include "DiskGame.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

class CoDPackageCache;
class CoDAsset_t;

// The Infinity Ward engine's sounds as its live alias readers find them: a loaded sound's samples
// in the zone, a primed or streamed one's in the install's soundfile<N>.pak (a language folder's
// when localized), read through CoDAssets' package cache.
namespace IWPakSounds
{
    // CoDAssets' package cache, which the live readers stream mesh and sound data from, set to a
    // cache the caller keeps for this object's lifetime
    class PackagesInUse
    {
    public:
        explicit PackagesInUse(CoDPackageCache* Packages);
        ~PackagesInUse();
        PackagesInUse(const PackagesInUse&) = delete;
        PackagesInUse& operator=(const PackagesInUse&) = delete;

    private:
        std::unique_ptr<CoDPackageCache> Saved;
    };

    struct WriteOptions
    {
        bool OnePerName = false;        // a later sound of a name already written (case ignored) is skipped
        bool EmptyFileFails = false;    // a written file of no byte is a failure
        std::string Language;           // the language folder localized sounds are read from (GameWorldWar2::LanguageOfZone)
    };

    // The extension Write gives a sound: the exportsnd setting's (".wav")
    std::string Extension();

    // The zone's counter of a name (sounds_streamed, sounds_loaded: the sounds met of each)
    using Counters = std::function<size_t&(const std::string& Name)>;

    // Writes the sounds the alias readers found (CoDSound_t) with Packages as the package cache,
    // then frees them all, cancelled or not. A loaded sound with no samples is a name-only header
    // another zone holds the samples of: counted as a reference.
    void Write(std::vector<CoDAsset_t*>& Found, CoDPackageCache* Packages, const DiskGame::Context& C, DiskGame::Totals& T,
               const std::string& Zone, const Counters& Counter, const WriteOptions& Options = WriteOptions());
}
