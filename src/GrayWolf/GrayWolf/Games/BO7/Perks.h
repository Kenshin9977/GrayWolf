#pragma once

// Black Ops 7's perks kind (Games/BO7/DiskGame.cpp, ExportsPerks): what the perk assets' settings
// trees mean, and the player's base settings they scale. The perk asset's key/value table is read
// by the game's server scripts, which the install does not ship: a field's name is the one whose
// FNV-1a 64 is its key (recovered by brute force over the zones' own words; Perks.cpp hashes each
// name, so a name is only ever applied to its own key), its unit and meaning inferred from that
// name, its value and the perk's in-game text.

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace BO7Perks
{
    using Json = nlohmann::ordered_json;

    // A perk's tree as ZonekitZone::ExportsBundleTrees reads it: its fields named where the name is
    // proven, the effect fields given "unit" and "meaning"; a perk holding effect fields adds one
    // entry to Effects ({id, perk: its name's localize key, effects: [{field, value, unit,
    // meaning}]})
    void Annotate(Json& Tree, Json& Effects);

    // The match options the game's private match menus offer (a script bundle {category_list:
    // [{options_list: [{group, name, values_list}]}]}, each value a bundle {text_ref, value_struct}),
    // from the zone's script bundles (stem, tree): per option its values and the one labelled
    // "Default (&&1)" (and "Hardcore (&&1)"); null when the zone holds no such menu
    Json MatchOptions(const std::vector<std::pair<std::string, Json>>& Bundles);
}
