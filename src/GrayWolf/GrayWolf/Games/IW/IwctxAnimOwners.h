#pragma once

// Anim owners (AnimOwners.h) for Black Ops 6 and 7 (iwctx zones): anim sets (their name an
// archetype's), the characters naming that archetype (body, head as script strings), scene
// bundles' objects and map entities. Owner ids "archetype:<archetype>/<character>",
// "scene:<bundle>/<object>", "mapent:<model>"; an anim's owners most specific first (scenes and
// entities, then the smallest set's characters, the zone's own first). Detail:
// graywolf-tools/docs/games/BO7.md "Anim owners".

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "DiskGame.h"
#include "ZoneTableWalker.h"

namespace AnimOwners { struct Book; }

namespace IwctxAnimOwners
{
    using Json = nlohmann::ordered_json;

    // the document a zone gives (IDiskZone::Documents, IDiskGame::AnimOwnerDocuments)
    inline constexpr const char* DocumentName = "anim_owners/facts.json";

    // a walked zone as served: its relocated bytes at Base (the address of its byte 0)
    struct Served
    {
        std::string Name;
        const std::vector<uint8_t>* Bytes = nullptr;
        uint64_t Base = 0;
        const ZoneTables::ZoneResult* R = nullptr;
    };

    struct Layout
    {
        std::vector<std::string> Sets;            // the anim set types (their reads name xanims)
        std::vector<std::string> Nodes;           // types a set names that list xanims in turn (states, aliases)
        std::string Anim = "xanim", Character = "character", Composite = "xcompositemodel", Model = "xmodel";
        // character: the script strings' low u32 (body, head, archetype)
        int64_t CharBody = -1, CharHead = -1, CharArchetype = -1;
        int64_t ModelName = 8;                    // an xmodel's name pointer ("a::b" exported as a_b)
        std::string (*AnimName)(uint64_t Hash) = nullptr;                      // an xanim as exported
        std::string (*AssetName)(const char* Type, uint64_t Hash) = nullptr;   // an index's name, else "<type>_<hex>"
    };

    // The zone and those served beside it (the zone first), read for their facts
    class Zones
    {
    public:
        Zones(std::vector<Served> All, const Layout& L);
        ~Zones();
        // sets {id, name, zone, anims, links}, characters {id, name, zone, body, head, archetype}
        Json Facts() const;
        // an xcompositemodel's xmodels (the zone-level xmodels its reads name), by its name
        std::vector<std::string> CompositeModels(const std::string& Name) const;

    private:
        struct Index;
        std::unique_ptr<Index> I;
    };

    // A scriptbundle's scene objects (as the bundles kind writes it) into Facts' "scenes";
    // Composite gives a composite model's xmodels by its name
    void AddScenes(Json& Facts, const Json& Bundle, const std::string& BundleName,
                   const std::function<std::vector<std::string>(const std::string&)>& Composite);
    // The character names an aitype bundle gives (its "character" strings)
    void AddCharacterNames(Json& Facts, const Json& AiType);
    // The map entities with an "animation" and a "model" into Facts' "mapents"
    void AddMapEntities(Json& Facts, const std::vector<std::vector<std::pair<std::string, std::string>>>& Entities);

    // The trees a zone's exporter of kind K (bundles, aitypes) would write, handed over in memory
    // (DiskGame::Context::OnTree): (file stem, JSON)
    void ReadExported(const DiskGame::IDiskZone::Exporter& Export, DiskGame::Kind K, std::vector<std::pair<std::string, Json>>& Out);

    // The zones of Zm (a zone whose name has a zm_ part) or Mp a zone is loaded with that hold its
    // mode's anim sets and characters, found beside it, in Install or in Install/Folder
    std::vector<std::string> SharedZones(const std::string& Zone, const std::string& Install, const char* Folder,
                                         const std::vector<std::string>& Zm, const std::vector<std::string>& Mp);
    // True while Fill reads a shared zone's document: its scene bundles are left out (a global
    // zone holds thousands of bundles and no scene)
    bool ReadingShared();

    // The run's anims' owners: each zone's document, then the documents of Shared(zone, facts),
    // the zones loaded with it it did not serve beside itself (opened once per process; none is
    // open during the hook)
    void Fill(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& Options, const std::vector<DiskGame::IDiskGame::AnimZone>& Zones,
              AnimOwners::Book& Out, const std::function<std::vector<std::string>(const std::string& Zone, const Json& Facts)>& Shared);
}
