#pragma once

// The titles on Black Ops 7's engine (Black Ops 7, Modern Warfare 4): one zone class reads them all
// (Games/BO7/DiskGame.cpp, its readers in GameBlackOps7.cpp), each game giving what differs: its
// profile and folder, and the offsets its loaders moved (a Spec). A game on this engine is a Spec
// and a Game; the zone class never names a title.

#include "DiskGame.h"
#include "ZonekitZone.h"
#include "Games/IW/IwTerrain.h"
#include "Games/IW/IwWeaponStats.h"

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Techsets
{
    struct Format;
}
namespace Vfx
{
    struct Layout;
}

namespace BO7Family
{
    // a struct dumps kind's type (ZonekitZone::ExportsStructDumps): its name field, its fields
    // proven so far, the field holding its 63-bit name hash
    struct DumpType
    {
        std::string Type;
        int64_t NameAt = -1;
        std::map<std::string, ZonekitZone::DumpField> Fields = {};
        int64_t IdAt = 0;
    };

    struct Spec
    {
        // the game: --game, its title, GameModule engine, the live readers' engine (LiveGames)
        const char *Id = "", *Title = "", *Engine = "", *LiveGame = "";
        const char* Profile = "";             // the zonekit profile
        const char* Folder = "";              // its folder in Call of Duty HQ (cod25)
        std::vector<const char*> NamePackages;   // the package index's name files, first wins
        std::string IndexSuffix = "+parts1";      // IndexKey's: the parts the index lists
        // the zones the game loads before a level (code, global, the mode's ingame zones; their
        // techsets_ zones are added) and those holding the mode's anim sets and characters; empty:
        // Black Ops 7's (multiplayer, or zombies for a zm_ level)
        std::vector<std::string> LevelZones, AnimZones;

        // soundbank: aliases, u32 count, alias size, its hash, entries, u16 count, entry size, its sndasset
        std::array<int64_t, 8> SoundLinks{};
        std::shared_ptr<const Techsets::Format> (*Techsets)() = nullptr;   // nullptr: no techsets kind
        const Vfx::Layout& (*VfxLayout)() = nullptr;                        // particlesystem
        // script bundle value codes read otherwise than Black Ops 7's (ScriptBundleLayout::Types)
        std::vector<std::pair<uint8_t, BundleValue>> BundleTypes;
        // the scriptable kind's types (scriptable, scriptablevariant), the weapons kind's
        std::vector<DumpType> Scriptables, Weapons;
        const IwWeaponStats::Layout* (*Stats)() = nullptr;                  // nullptr: no weapon stats kind
        // perk: its settings (a script bundle's key/value table) and the assets it names (the perks
        // kind, Games/BO7/Perks.h); Black Ops 7's perk (0x60, 0x387e940): the table at 8, an
        // impulsemotion at 0x40
        int64_t PerkTable = 8;
        std::vector<std::pair<std::string, int64_t>> PerkAssets = { { "impulsemotion", 0x40 } };

        // ---- the level
        // gfx_map_trzone (0x190): its index i32, its draw data's inline struct (position data size
        // u32 0, its pointer 0x18, the u16 indices 0x20, the 32-byte packed tables 0x28, the packed
        // indices 0x30)
        int64_t TzIndex = 0x14, TzDraw = 0x18;
        // map_ents: the spawn points' u32 count and pointer (-1: none there), the scriptable
        // instances' count and records (0x60 each)
        int64_t SpawnCount = 0x38, Spawns = 0x40, ScriptableCount = 0x318, ScriptableRecords = 0x388;
        // map_spawn_points: where its spawn list is (u16 count, the records' pointer 8 bytes on; -1: no
        // such asset read)
        int64_t SpawnPoints = -1;
        // scriptable: its size, its models' u32 count and xmodel pointer list
        int64_t ScriptableSize = 0xd8, ScriptableModelCount = 0xb4, ScriptableModels = 0xb8;
        const IwTerrain::Layout* Terrain = &IwTerrain::BlackOps7;          // nullptr: no terrain read
    };

    // A title on this engine: its zones opened by the family's zone class with its Spec
    class Game : public DiskGame::IDiskGame
    {
    public:
        explicit Game(const Spec& S) : S(S) {}
        const char* Id() const override { return S.Id; }
        const char* Title() const override { return S.Title; }
        const char* Engine() const override { return S.Engine; }
        std::unique_ptr<DiskGame::IDiskZone> Unopened() const override;
        // an install holding its folder (Call of Duty HQ's root: cod25, cod26 and sp26 side by
        // side): that folder's zones only; any other folder: all of them
        bool ListZones(const std::string& Install, const DiskGame::ExportOptions& Options, std::vector<std::string>& Names,
                       std::string& Error) const override;
        // its zones open on the index's workers: their decompression and walk share nothing, its
        // handlers and fixups run under the session lock, ResolveZones is serialized
        bool IndexInParallel() const override { return true; }
        const DiskGame::CipherSource* CipherStrings() const override;
        std::unique_ptr<DiskGame::IDiskZone> Open(const std::string& Path, const DiskGame::ExportOptions& Options, std::string& Error) const override;
        std::vector<std::string> AnimOwnerDocuments() const override;
        void AnimOwners(const DiskGame::ExportOptions& Options, const std::vector<AnimZone>& Zones, ::AnimOwners::Book& Out) const override;
        // the char_heads setting (its zones' CharacterHeads)
        const std::vector<DiskGame::Setting>& Settings() const override
        {
            static const std::vector<DiskGame::Setting> Own{ DiskGame::CharacterHeads };
            return Own;
        }

    protected:
        const Spec& S;
    };
}
