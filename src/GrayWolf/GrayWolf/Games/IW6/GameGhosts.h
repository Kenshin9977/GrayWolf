#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <set>
#include <utility>
#include <vector>

#include "CoDAssetType.h"

class GameGhosts
{
public:
    static std::unique_ptr<XAnim_t> ReadXAnim(const CoDAnim_t* Animation);
    static std::unique_ptr<XModel_t> ReadXModel(const CoDModel_t* Model);

    // Inflates one pak stream entry and translates it at the given size
    static std::unique_ptr<XImageDDS> DecodePakImage(const GhostsGfxImage& Info, const std::string& PakPath, uint64_t Start, uint64_t End, uint32_t Width, uint32_t Height, ImageUsageType Usage);

    // The sounds one alias names (loaded, streamed, primed), each file spec once across calls
    static void AddAliasSounds(const GhostsSoundAlias& Alias, const std::string& SoundName, std::set<uint64_t>& Unique, std::vector<CoDAsset_t*>& Out);

    static const XMaterial_t ReadXMaterial(uint64_t MaterialPointer);
    // A material another zone defines (a name-only header without images, its name without the
    // leading ','): its images as (semantic hash, image name), when the caller knows them. Read
    // by name only: ReadXMaterial gives them the ExportWriters::ImageElsewhere pointer.
    static thread_local std::function<bool(const std::string& Name, std::vector<std::pair<uint32_t, std::string>>& Images)> ImagesElsewhere;
};