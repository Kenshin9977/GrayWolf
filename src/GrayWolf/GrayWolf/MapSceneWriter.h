#pragma once

// Port: a MapScene written in one format. The map exporter (Maps::WriteMapFiles) asks each
// enabled writer in turn - the .glb always, the Cast scene with map_cast, the Radiant source with map_radiant - with one
// failure and counter handling; another format is one more implementation of this.

#include "MapScene.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Materials { class IAdapter; }

namespace Maps
{
    struct SceneWriteResult
    {
        std::vector<std::pair<std::string, size_t>> Counters;   // report counters, by name
        // work deferred until the run's dependencies are written (the .glb's auto alpha): it adds
        // its result to the counter LaterCounter
        std::function<size_t()> Later;
        std::string LaterCounter;
        // the same, for work that measures several things: it adds them to the counters it is given
        std::function<void(std::map<std::string, size_t>& Counters)> LaterCounters;
    };

    class ISceneWriter
    {
    public:
        virtual ~ISceneWriter() = default;
        virtual const char* Extension() const = 0;   // ".glb"
        virtual bool Write(const MapScene& Scene, const std::string& Path, const Materials::IAdapter& Adapter,
                           SceneWriteResult& Result, std::string& Error) const = 0;
    };

    const ISceneWriter& GlbWriter();    // MapGlbWriter.cpp: the Blender scene, with its import script
    const ISceneWriter& CastWriter();   // MapCastWriter.cpp: the Cast scene (Maya, the Cast plugins)
    // MapRadiantWriter.cpp: the Radiant source (the mod tools); Materials: with the material GDT
    // entries of its models' materials (the material_gdt setting); Tree: written into a Black Ops III
    // Mod Tools tree (the mod_tools setting, ModToolsTree.h), "" beside the map; Patches: the drawn
    // world as a patch per triangle rather than models (the map_surfaces setting); Game: the source
    // game's id
    std::unique_ptr<ISceneWriter> RadiantWriter(bool Materials, const std::string& Tree, bool Patches, const std::string& Game);
    const ISceneWriter& CollisionWriter();   // MapCollisionWriter.cpp: the clip map's collision as JSON
}
