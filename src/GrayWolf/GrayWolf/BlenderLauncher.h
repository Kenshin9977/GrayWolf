#pragma once

// Opens a map export in Blender: the first time, Blender runs the map's <map>_import.py and saves
// <map>.blend beside the .glb; later it opens that .blend directly (a re-import runs the script
// again and overwrites it). Used by the GUI's "Open in Blender" buttons and by `open-map`. Only
// files are needed: the export folder, never the game.

#include <string>
#include <vector>

namespace Blender
{
    // Where blender.exe was found, and from what ("setting", "BLENDER", "Program Files", ...);
    // Path is "" when none was.
    struct Exe
    {
        std::string Path;
        std::string From;
    };

    // The blender_path setting when set (empty: detect), else %BLENDER%, else the newest
    // "Blender Foundation\Blender x.y" in Program Files, else Steam's Blender, else the registry
    // (App Paths, the .blend file association), else PATH.
    Exe Find();
    // The same with the setting's value given (from a worker, which does not read the settings)
    Exe Find(const std::string& BlenderPath);

    // One exported map: its .glb, its import script and the .blend saved from it (which may not
    // exist yet), all in one folder.
    struct Map
    {
        std::string Name;     // the .glb's stem
        std::string Glb;
        std::string Script;   // <map>_import.py
        std::string Blend;    // <map>.blend

        bool HasBlend() const;
        bool HasScript() const;
    };

    // A map's .glb, its _import.py or its .blend as the map; a model's .cast or .gltf the same (its
    // <model>_import.py and <model>.blend beside it); false (Error set) for anything else.
    bool MapOf(const std::string& File, Map& Out, std::string& Error);
    // The maps of an export folder: every *_import.py in it and in its maps\ (the export root of a
    // game, or its maps folder), by name.
    std::vector<Map> MapsIn(const std::string& Folder);

    struct LaunchOptions
    {
        bool Reimport = false;     // run the import script even when the .blend exists
        bool Background = false;   // no window: import, save, quit, and wait for it (open-map --background)
        Exe Use;                   // blender.exe already found (the GUI's, off its thread); empty: Find()
        // a map's props on import: "fast" (map cells), "objects" (one object per prop), "" the
        // script's own PROPS; passed to the script as "--props <value>" (no effect on a .blend opened)
        std::string Props;
    };

    // Starts Blender on the map: the .blend when it exists (and no re-import), else the import
    // script, saving the .blend. Detached unless Background. Returns false with Error set when
    // Blender or the script is missing or the process did not start (or, in the background, did
    // not write the .blend); Did says what was done ("opened", "importing").
    bool Open(const Map& M, const LaunchOptions& O, std::string& Did, std::string& Error);
}
