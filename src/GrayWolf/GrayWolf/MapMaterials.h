#pragma once

// A map scene's materials as the game's adapter describes them, shared by the map writers (the
// .glb's sidecar and alpha modes, the .cast's material slots): one description per material,
// its image files named relative to the directory of the file that refers to them.

#include "MapScene.h"
#include "MaterialDescription.h"

#include <filesystem>
#include <string>
#include <utility>

namespace Maps
{
    // File relative to Dir, with forward slashes (the file itself when it is not below Dir's drive)
    std::string RelativeTo(const std::string& File, const std::filesystem::path& Dir);
    // The scene material Name as a Source (its textures that are files) and the adapter's Description
    std::pair<Materials::Source, Materials::Description> DescribeMaterial(const MapScene& Scene, const std::string& Name,
                                                                          const Materials::IAdapter& Adapter,
                                                                          const std::filesystem::path& Dir);
    // The same for a material not in the scene's list (a blend layer's, MapRadiantLayers.h), its images the scene's
    std::pair<Materials::Source, Materials::Description> DescribeMaterial(const MapScene& Scene, const Material& M,
                                                                          const Materials::IAdapter& Adapter,
                                                                          const std::filesystem::path& Dir);
}
