#pragma once

// A Black Ops III usermap around an exported map (the bo3_usermap setting), made as the Mod Tools
// launcher makes one from its "ZM Mod Level" template (rex/templates): usermaps/zm_<map>/ (the
// template's scripts, zone, sound config and images, "template" renamed) and map_source/zm/
// zm_<map>.map, the template's .map without its room (worldspawn's brushes) holding the export's
// map_source/graywolf/<map>.map as a prefab, the template's entities (player spawns, zone volume,
// sun and umbra volumes, ...) moved to the export's player start. Then, when asked, built as the
// launcher's Compile, Light and Link do: gdtdb /update, cod2map64, Radiant's lighting, the linker.

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ModToolsUsermap
{
    // zm_<map>, lower case
    std::string NameOf(const std::string& Map);

    struct Made
    {
        std::string Name;          // zm_<map>
        bool Created = false;      // false: already there, kept as it is (a user may have edited it)
        std::string Error;
    };
    // The usermap of Root's map_source/<Prefab> (graywolf/<map>.map) for Map
    Made Create(const std::filesystem::path& Root, const std::string& Map, const std::string& Prefab);

    struct Step
    {
        std::string Label;         // "compile"
        unsigned long Exit = 0;
        double Seconds = 0;
        size_t Errors = 0;         // its output's lines starting "ERROR"
        std::string Log;           // usermaps/<name>/graywolf_<label>.log
        std::string Assertion;     // the text of the assertion box it put up (dismissed: the step failed)
    };
    // Compile, Light and Link of usermap Name, step after step until one fails; Line gets a line per
    // step; Poll (every half second while a tool runs) stops it when it answers false
    std::vector<Step> Build(const std::filesystem::path& Root, const std::string& Name, const std::function<void(const std::string&)>& Line,
                            const std::function<bool()>& Poll);
}
