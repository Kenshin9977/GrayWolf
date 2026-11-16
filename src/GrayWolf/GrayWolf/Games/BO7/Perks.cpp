#include "stdafx.h"

// Black Ops 7's perks kind: the meaning of the perk assets' fields (Perks.h). Where each fact comes
// from: graywolf-tools docs/games/BO7.md, "Perks".

#include "Games/BO7/Perks.h"

#include <cstdint>
#include <format>
#include <map>
#include <unordered_map>

namespace
{
    // a script bundle key: FNV-1a 64 of its lowercase name, all 64 bits
    uint64_t KeyHash(const std::string& Name)
    {
        uint64_t H = 0xCBF29CE484222325ull;
        for (const char C : Name) H = (H ^ (uint8_t)(C >= 'A' && C <= 'Z' ? C + 0x20 : C)) * 0x100000001B3ull;
        return H;
    }

    struct Field
    {
        const char* Name;
        const char* Unit = nullptr;      // nullptr: a name alone, no effect
        const char* Meaning = nullptr;
    };

    // The names recovered for the perk table's keys (each one's FNV-1a 64 is the key; checked at
    // start). The zombie perks' effect fields carry the unit and meaning their name and value give:
    // the server scripts that read them are not on disk, so a unit is inferred, never traced.
    const Field Fields[] = {
        // Jugger-Nog ("Increase maximum health.")
        { "zombieperkjuggernauthealth", "health points", "maximum health added by the perk (base maximum health: the match option commonOption.maxHealth, default 100)" },
        { "zombieperkjuggernauthealthupgrade", "health points", "maximum health added by the perk's health upgrade (augment)" },
        // Deadshot Daiquiri ("Improve ADS precision and increase Critical Damage of bullets.")
        { "zombieperkdeadshotcriticalmultiplier", "multiplier", "critical damage of bullets multiplied by it (1.1: +10%)" },
        // Stamin-Up ("Increase movement speed.")
        { "zombieperkstaminupspeedup", "fraction", "movement speed added (0.1: +10%)" },
        // Elemental Pop ("Attacks can trigger random Ammo Mods.")
        { "zombieperkelementalpopchance", "percent", "chance an attack triggers a random ammo mod" },
        { "zombieperkelementalpopcooldown", "seconds", "time before Elemental Pop can trigger again" },
        { "zombieperkelementalpopcooldownmult", "multiplier", "multiplier on the cooldown of the ammo mod it triggered (meaning from the name only)" },
        // Wisp Tea (w_mod, French "Thé fumant": "Drink to summon a companion wisp after killing zombies.")
        { "wmodcooldown", "seconds", "time before a wisp can be summoned again" },
        { "wmodsummontime", "seconds", "how long a summoned wisp stays" },
        { "wmodrapidkillthreshold", "count", "kills that summon the wisp (meaning from the name only: a count or a time window)" },
        { "wmoddamagetime", "milliseconds", "time between the wisp's damage ticks (meaning from the name only)" },
        { "wmodattackrate", "seconds", "time between the wisp's attacks (meaning from the name only)" },
        { "wmodattackrange", "units", "range of the wisp's attack (game units, inches)" },
        { "wmodsearchrange", "units", "range in which the wisp looks for targets (game units, inches)" },
        { "wmodplayerrange", "units", "distance the wisp keeps to its player (game units, inches)" },
        { "wmodbasemaxspeed", "units per second", "the wisp's top speed (meaning from the name only)" },
        { "wmodwispscriptable", nullptr },
        // Quick Revive ("Recover health and revive allies faster.")
        { "zombieperkquickreviveregendelay", "multiplier", "multiplier on the delay before health regenerates (0.5: half)" },
        // Death Perception ("Obscured enemies are keylined.")
        { "zombieperkdeathperceptionradius", "units", "radius in which obscured enemies are keylined (game units, inches)" },
        // PHD Flopper ("Explosive dive to prone and immunity to self-inflicted explosive damage.")
        { "zombieperkphdflopperradius", "units", "radius of the dive's explosion (game units, inches)" },
        { "zombieperkphdfloppermindamage", "damage", "the explosion's least damage" },
        { "zombieperkphdflopperdamagehuman", "damage", "the explosion's damage to human enemies" },
        { "zombieperkphdflopperdamagespecial", "fraction", "the explosion's damage to special zombies, as a fraction of their health (meaning from the name only)" },
        { "zombieperkphdflopperdamageelite", "fraction", "the explosion's damage to elite zombies, as a fraction of their health (meaning from the name only)" },
        { "zombieperkphdflopperdamageboss", "fraction", "the explosion's damage to bosses, as a fraction of their health (meaning from the name only)" },
        { "zombieperkphdflopperheight", "units", "fall height the dive needs (meaning from the name only)" },
        { "zombieperkphdflopperheightstep", "units", "fall height per damage step (meaning from the name only)" },
        { "zombieperkphdflopperdamagestep", "damage", "damage added per height step (meaning from the name only)" },
        // Widow's Wine (y_mod: "When melee attacks bring your health low, retaliate with an explosion of webs.")
        { "ymodrange", "units", "radius of the web explosion (game units, inches)" },
        { "ymodslowtime", "seconds", "how long webbed enemies are slowed" },
        { "ymodslowpercentage", "fraction", "speed taken from webbed enemies (0.5: half)" },
        { "ymodcooldowntime", "seconds", "time before the webs can trigger again" },
        // Melee Macchiato, Mule Kick, Double Tap, Vulture Aid
        { "punchradius", "units", "the melee punch's reach (game units, inches; meaning from the name only)" },
        { "maxdamageround", "round", "the round up to which the melee damage grows (meaning from the name only)" },
        { "vulturedroppercent", "percent", "chance an enemy drops extra loot (meaning from the name only)" },
        { "vulturedropradius", nullptr },
        { "vulturedroplist", nullptr },
        // the perks' common fields (names only)
        { "zmperkcost", "essence", "the perk machine's price" },
        { "zmperkgesture", nullptr },
        { "zmperksound", nullptr },
        { "zombiesperk", nullptr },
        { "zombiesextraperks", nullptr },
        { "supersprintduration", nullptr },
        { "currencycost", nullptr },
        { "isfinaltier", nullptr },
        { "menuslot", nullptr },
        { "flccategory", nullptr },
        { "flclayout", nullptr },
        { "brdesc", nullptr },
        { "cpdesc", nullptr },
        { "brdisabled", nullptr },
        { "extrainfo", nullptr },
        { "fullimage", nullptr },
        { "legacyimage", nullptr },
        { "ingameimagepro", nullptr },
        { "perkaebitfield", nullptr },
        { "perkbufflist", nullptr },
        { "perkbonusdescription", nullptr },
        { "rewarddata", nullptr },
        { "specialistbr", nullptr },
        { "specialistperks", nullptr },
        { "specialistlist", nullptr },
        { "specialistmodel", nullptr },
        { "variantperks", nullptr },
        { "assassin_minimap_icon", nullptr },
        { "modelgreed", nullptr },
        { "compositemodelgreed", nullptr },
        { "shroudgrenadeweapon", nullptr },
        { "looter_chance", nullptr },
        { "meleedamagemod", nullptr },
        { "platetimemod", nullptr },
        { "standspeedmod", nullptr },
        { "armorregendelay", nullptr },
        { "armorregenrate", nullptr },
        { "healonheadshot", nullptr },
        { "ammoonkill", nullptr },
        { "durationmovementspeed", nullptr },
        { "boostmovementspeed", nullptr },
    };

    const std::unordered_map<uint64_t, const Field*>& ByHash()
    {
        static const std::unordered_map<uint64_t, const Field*> M = [] {
            std::unordered_map<uint64_t, const Field*> M;
            for (const auto& F : Fields) M.emplace(KeyHash(F.Name), &F);
            return M;
        }();
        return M;
    }

    uint64_t HashOf(const BO7Perks::Json& F)
    {
        const std::string H = F.value("key_hash", std::string());
        return H.empty() ? 0 : std::stoull(H, nullptr, 16);
    }

    // a localized or asset value's hash (the "value_hash" the tree keeps beside a named value)
    uint64_t ValueHash(const BO7Perks::Json& F)
    {
        if (F.contains("value_hash") && F["value_hash"].is_string()) return std::stoull(F["value_hash"].get<std::string>(), nullptr, 16);
        if (F.contains("value") && F["value"].is_string())
        {
            const std::string& S = F["value"].get_ref<const std::string&>();
            if (S.size() == 16 && S.find_first_not_of("0123456789abcdef") == std::string::npos) return std::stoull(S, nullptr, 16);
        }
        return 0;
    }

    const BO7Perks::Json* Find(const BO7Perks::Json& Fields, uint64_t Key)
    {
        for (const auto& F : Fields)
            if (HashOf(F) == Key) return &F;
        return nullptr;
    }
}

void BO7Perks::Annotate(Json& Tree, Json& Effects)
{
    if (!Tree.contains("fields") || !Tree["fields"].is_array()) return;
    Json Own = Json::array();
    std::string Perk;
    for (auto& F : Tree["fields"])
    {
        if (F.value("key", std::string()) == "name" && F.contains("value") && F["value"].is_string()) Perk = F["value"].get<std::string>();
        const auto It = ByHash().find(HashOf(F));
        if (It == ByHash().end()) continue;
        F["key"] = It->second->Name;
        if (It->second->Unit == nullptr) continue;
        F["unit"] = It->second->Unit;
        F["meaning"] = It->second->Meaning;
        Json E;
        E["field"] = It->second->Name;
        E["value"] = F.contains("value") ? F["value"] : Json(nullptr);
        E["unit"] = It->second->Unit;
        E["meaning"] = It->second->Meaning;
        Own.push_back(std::move(E));
    }
    if (Own.empty()) return;
    Json P;
    P["id"] = Tree.value("id", std::string());
    if (!Tree.value("name", std::string()).empty()) P["name"] = Tree["name"];
    P["perk"] = Perk;
    P["effects"] = std::move(Own);
    Effects.push_back(std::move(P));
}

BO7Perks::Json BO7Perks::MatchOptions(const std::vector<std::pair<std::string, Json>>& Bundles)
{
    // keys and labels whose names are not recovered, by hash: an option's group ("commonOption")
    // and name ("maxHealth"), a value's asset; the labels "Default (&&1)" and "Hardcore (&&1)"
    // (ens_global / fra_global: localize_734c96301bd21b4e, localize_29a1f94ed3201591)
    constexpr uint64_t Group = 0x37bb0763e06a05deull, Option = 0xb8854eed1afa2fa8ull, ValueAsset = 0xbdbef00c00fca2e5ull;
    constexpr uint64_t DefaultLabel = 0x734c96301bd21b4eull, HardcoreLabel = 0x29a1f94ed3201591ull;
    std::unordered_map<std::string, const Json*> ById;
    for (const auto& [Stem, B] : Bundles) ById.emplace(B.value("id", std::string()), &B);
    const auto Named = [](const Json& Fields, const char* Key) -> const Json* {
        for (const auto& F : Fields)
            if (F.value("key", std::string()) == Key) return &F;
        return nullptr;
    };
    // a value bundle: its number (value_struct {variant_object {value}}) and its label's hash
    const auto ValueOf = [&](uint64_t Asset, Json& Value, uint64_t& Label) {
        const auto It = ById.find(std::format("{:016x}", Asset & 0x7FFFFFFFFFFFFFFFull));
        if (It == ById.end() || !It->second->contains("fields")) return false;
        const Json& Fs = (*It->second)["fields"];
        const Json* Text = Named(Fs, "text_ref");
        Label = Text != nullptr ? ValueHash(*Text) : 0;
        const Json* S = Named(Fs, "value_struct");
        if (S == nullptr || !(*S)["value"].is_array()) return false;
        const Json* V = Named((*S)["value"], "variant_object");
        if (V == nullptr || !(*V)["value"].is_array() || (*V)["value"].empty()) return false;
        Value = (*V)["value"][0]["value"];
        return true;
    };
    std::map<std::string, Json> Options;   // by group.name: the first menu's, the menus naming it
    for (const auto& [Stem, B] : Bundles)
    {
        if (!B.contains("fields")) continue;
        const Json* Cats = Named(B["fields"], "category_list");
        if (Cats == nullptr || !(*Cats)["value"].is_array()) continue;
        for (const auto& Cat : (*Cats)["value"])
        {
            if (!Cat.is_array()) continue;
            const Json* List = Named(Cat, "options_list");
            if (List == nullptr || !(*List)["value"].is_array()) continue;
            for (const auto& O : (*List)["value"])
            {
                if (!O.is_array()) continue;
                const Json* G = Find(O, Group);
                const Json* N = Find(O, Option);
                const Json* Vs = Named(O, "values_list");
                if (G == nullptr || N == nullptr || Vs == nullptr || !(*G)["value"].is_string() || !(*N)["value"].is_string()) continue;
                const std::string Path = (*G)["value"].get<std::string>() + "." + (*N)["value"].get<std::string>();
                auto& Out = Options[Path];
                if (Out.is_null())
                {
                    Out["option"] = Path;
                    Json Values = Json::array();
                    for (const auto& V : (*Vs)["value"])
                    {
                        if (!V.is_array() || V.empty()) continue;
                        const Json* A = Find(V, ValueAsset);
                        Json Value;
                        uint64_t Label = 0;
                        if (A == nullptr || !ValueOf(ValueHash(*A), Value, Label)) continue;
                        Values.push_back(Value);
                        if (Label == DefaultLabel) Out["default"] = Value;
                        if (Label == HardcoreLabel) Out["hardcore"] = Value;
                    }
                    Out["values"] = std::move(Values);
                    if (const Json* E = Named(O, "is_enabled")) Out["enabled"] = (*E)["value"];
                    Out["menus"] = Json::array();
                }
                Out["menus"].push_back(B.value("id", std::string()));
            }
        }
    }
    if (Options.empty()) return nullptr;
    Json Out;
    Out["source"] = "the private match option menus (script bundles {category_list: [{options_list}]}); default: the value labelled \"Default (&&1)\"";
    Out["options"] = Json::array();
    for (auto& [Path, O] : Options) Out["options"].push_back(std::move(O));
    return Out;
}
