#pragma once

// The template every game's offline extraction fills in. A game gives an IDiskGame (what it is,
// which zones to read, how to open one, the settings it takes) and its zones an IDiskZone (an
// exporter per asset kind it supports). Export is the one use case: the zone loop, the kinds, the
// output layout out/<game>/<kind>/ and the failures - no game writes its own. The CLI
// (DiskGameCli.h) and the GUI's disk panel are its two adapters; both fill ExportOptions.
// A reader family implements IDiskZone once (ZonekitZone for the zonekit table walker); a game
// then only supplies that family's hooks. Games register from Games/<game>/DiskGame.cpp.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace AnimOwners { struct Book; }

namespace DiskGame
{
    // The asset kinds, one row each in Kinds below. A new kind is a row there (and here) plus its
    // exporter in the readers that support it: --type, the report and the GUI read the table.
    enum class Kind { Model, Anim, Image, Sound, Rawfile, Data, Localize, Map, Script, Gdt, Fx, MaterialLinks, SoundLinks, AssetRefs, Techsets, ScriptBundles, Weapons, WeaponStats, MapInfo, Scriptables, MapEnts, AiTypes, Material };

    struct KindInfo
    {
        Kind Id;
        const char* Name;     // --type and the saved GUI choice: "model"
        const char* Folder;   // <out>/<game>/<folder>/ and the report's prefix: "models"
        const char* Label;    // the GUI checkbox
        bool ByDefault = true; // part of a run that names no kind
        bool Research = false; // a data dump for naming or reading the game, not an asset: the GUI folds it away
    };
    inline constexpr KindInfo Kinds[] = {
        { Kind::Model, "model", "models", "Models" },
        { Kind::Anim, "anim", "anims", "Anims" },
        { Kind::Image, "image", "images", "Images" },
        { Kind::Sound, "sound", "sounds", "Sounds" },
        { Kind::Rawfile, "rawfile", "rawfiles", "Rawfiles" },
        { Kind::Data, "data", "data", "Data" },
        { Kind::Localize, "localize", "localize", "Localize" },
        // the level itself: world geometry, static models, clip map, entities
        { Kind::Map, "map", "maps", "Maps" },
        // the game's scripts, source or compiled (ScriptExport.h); asked for by name only
        { Kind::Script, "script", "scripts", "Scripts", false },
        // assets as Black Ops 3 Mod Tools GDT entries: BO3 own, a later title ported (Gdt.h); by name only
        { Kind::Gdt, "gdt", "gdt", "BO3 GDT entries", false },
        // effects as data: each effect's elements, their timings and what they draw, as JSON
        // (Games/Treyarch/TreyarchEffects.h); asked for by name only
        { Kind::Fx, "fx", "fx", "Effects (JSON)", false, true },
        // which images each material names, one CSV per zone (ZonekitZone::ExportsMaterialLinks):
        // for naming work on hashed images; asked for by name only
        { Kind::MaterialLinks, "matlinks", "matlinks", "Material image links (CSV)", false, true },
        // which sound files each sound alias plays, one TSV per zone (ZonekitZone::ExportsSoundLinks):
        // for naming work on hashed sounds; asked for by name only
        { Kind::SoundLinks, "sndlinks", "sndlinks", "Sound alias links (TSV)", false, true },
        // what each asset of a zone references, one CSV per zone (ZonekitZone::ExportsAssetRefs):
        // for naming work; asked for by name only
        { Kind::AssetRefs, "refs", "refs", "Asset references (CSV)", false, true },
        // each techset of a zone: its techniques, their shaders (bytecode and reflection), the
        // arguments binding code and material data to registers, the materials' bindings, one folder
        // per zone (Techsets.h): for porting materials into the game; asked for by name only
        { Kind::Techsets, "techsets", "techsets", "Techsets, shaders and material bindings", false, true },
        // the settings assets scripts read with getscriptbundle, one JSON per bundle under its zone
        // (ZonekitZone::ExportsScriptBundles): the numbers behind perks, upgrades, ammo mods...;
        // asked for by name only
        { Kind::ScriptBundles, "bundles", "bundles", "Script bundles (JSON)", false, true },
        // the weapon assets (weapon, its tunables, accuracy, trigger, archetype, blueprint, attachments)
        // as raw struct dumps, one JSON per asset under its zone and type (ZonekitZone::ExportsStructDumps):
        // every field by offset, the blocks it points to followed; for naming the fields; by name only
        { Kind::Weapons, "weapons", "weapons", "Weapon data (struct dumps, JSON)", false, true },
        // the weapons' stats in user units, one row per weapon and tunables set (CSV + JSON, README with
        // the units and formulas), only the proven fields; each zone adds its facts, the table is
        // rebuilt from all of them (Games/IW/IwWeaponStats.h); by name only
        { Kind::WeaponStats, "weaponstats", "weaponstats", "Weapon stats (CSV + JSON)", false, true },
        // each map's settings asset as JSON, its key/value tree as the bundles kind writes it (Black
        // Ops 7's mapinfo: what getactivemapinfobundle() returns: destructibles, their loot...); by name only
        { Kind::MapInfo, "mapinfo", "mapinfo", "Map info (JSON)", false, true },
        // the scriptable assets (and their variants) as struct dumps, one JSON per asset under its zone
        // and type (ZonekitZone::ExportsStructDumps); by name only
        { Kind::Scriptables, "scriptables", "scriptables", "Scriptables (struct dumps, JSON)", false, true },
        // a map zone's entities, one line per key/value pair "entity_index<TAB>key<TAB>value", one TSV per
        // zone (Black Ops 6 and 7: the entity strings, spawn points, scriptable instances); by name only
        { Kind::MapEnts, "mapents", "mapents", "Map entities (TSV)", false, true },
        // each AI type's settings as JSON, its key/value tree as the bundles kind writes it (Black Ops
        // 7's aitype: the AI's settings bundle, its shared functions' settings...); by name only
        { Kind::AiTypes, "aitypes", "aitypes", "AI types (JSON)", false, true },
        // each material as JSON (its techset, images and blend factors) and its images: what an
        // effect draws with, pulled from the zone that defines it (ZonekitZone::ExportMaterials,
        // FxPreview reads them); by name only
        { Kind::Material, "material", "materials", "Materials (JSON + images)", false, true },
    };
    constexpr bool KindsInOrder()
    {
        for (size_t I = 0; I < std::size(Kinds); I++)
            if (static_cast<size_t>(Kinds[I].Id) != I) return false;
        return true;
    }
    static_assert(KindsInOrder(), "DiskGame::Kinds: one row per Kind, in enum order");
    const std::vector<Kind>& AllKinds();   // in table order, the order a zone exports them
    const KindInfo& Info(Kind K);
    inline const char* KindName(Kind K) { return Info(K).Name; }
    inline std::string KindFolder(Kind K) { return Info(K).Folder; }
    // "model,anims": names or folders, comma-separated; the kinds ByDefault when empty
    bool ParseKinds(const std::string& Text, std::set<Kind>& Out, std::string& Error);

    struct ExportOptions;

    // A sound as a dry run of the sound kind tells it (Context::OnSoundHeader): what its reader
    // would hand ExportWriters::WriteSound, read from the game's headers, without its samples
    struct SoundHeader
    {
        std::string Name;          // its file under sounds/ without extension: the same file once
        enum class Form { Wav, Flac, Pcm16, Opus, Other } Is = Form::Wav;
        uint64_t Stored = 0;       // its bytes as given (a whole .wav or .flac file, samples, packets)
        uint32_t Rate = 0, Channels = 0;
        uint64_t Frames = 0;       // samples per channel, 0 when the header does not say
        uint32_t Bits = 16;        // bits per sample (a FLAC's STREAMINFO says; 16 otherwise)
        bool KeptUnderWav = false; // a FLAC file written as it is under WAV (Black Ops II's)
    };

    // An asset by kind and name, the name as its reader writes it (its file, without extension),
    // compared without case: what a run can be limited to (Context::Only), what one zone needs
    // from another (Context::Needs) and what the install index lists.
    struct AssetId
    {
        Kind K = Kind::Model;
        std::string Name;
        bool operator<(const AssetId& O) const;
    };
    using Selection = std::set<AssetId>;

    // A choice a game takes, shown in the GUI only while one of its kinds is ticked; the CLI maps
    // its own flag onto the same key. Of() is the value a run asked for, or the default.
    struct Setting
    {
        const char* Key;                                          // "ui_res"
        const char* Label;
        std::vector<std::pair<std::string, std::string>> Values;  // (value, label)
        std::string Default;
        std::set<Kind> Concerns;
        std::string Of(const ExportOptions& O) const;
        std::string Of(const std::map<std::string, std::string>& Values) const;
        // a value or a label (case-insensitive) as its value; false when it is neither
        bool Parse(const std::string& Text, std::string& Value) const;
    };

    // How a run reads its zones: what the games' hooks take, filled by the CLI or the GUI.
    struct ExportOptions
    {
        std::string Install;          // the game's install (BO4: its CASC packages)
        std::string Cache;            // where pulled/unpacked files go; a temp folder when empty
        std::string Profile;          // another zonekit profile than the game's own
        std::string AudioFormat;      // wav|flac|opus (BO3 reads it; every game the exportsnd setting it sets), the saved setting when empty
        std::string ActsExe;          // acts.exe (BO3 rawfiles, scripts), located when empty
        std::string GscToolExe;       // gsc-tool.exe (scripts), located when empty
        std::string GameExe;          // the game's dumped exe (encrypted script strings), acts' deps one when empty
        std::string OatModelFormat, OatImageFormat;   // OAT dumps; OAT's defaults when empty
        bool OatGdt = false;          // OAT: also write source_data/<zone>.gdt
        unsigned Threads = 0;         // the install index's workers (0: the default, at most 8)
        // set on what the install index opens a zone with: only its Defines, Parts and Footprint are
        // asked, so a game may leave out what only an export reads (Black Ops 7: the zones beside a map)
        bool Indexing = false;
        // set by a run whose kinds read each zone alone (material links): a game may then leave out
        // the zones it loads beside a map
        bool ZoneAlone = false;
        std::map<std::string, std::string> Settings;  // Setting::Key -> value
    };

    // The settings every game takes, beside its own (IDiskGame::AllSettings).
    // map_png: PNG copies of a map's colour maps under maps/textures/ for glTF tools other than
    // Blender (the .glb then names them; else it names the image kind's files, which the import
    // script reads, DDS included).
    extern const Setting MapPng;
    // map_textures: full, 2k or 1k: a map's textures larger than that are copied under
    // maps/textures/ at that size with mips (ExportWriters::ResizedDdsCopies), and read from there.
    extern const Setting MapTextures;
    // map_cast: also the map as a Cast scene (MapCastWriter.h): <map>.cast, its placements referring
    // to maps/models/<model>.cast, for Maya and Cast's importers (the .glb stays the Blender one).
    extern const Setting MapCast;
    // map_radiant: also the map as a Radiant source (MapRadiantWriter.h): <map>.map, its clip map's
    // brushes, entities and model placements, its world and models as XMODEL_EXPORT, for the mod tools
    extern const Setting MapRadiant;
    // material_gdt: also Black Ops III Mod Tools GDT entries for the exported materials (MaterialGdt.h):
    // the models' as source_data/graywolf_<out folder>.gdt (their XMODEL_EXPORT files naming them),
    // a map's in its Radiant GDT (with map_radiant)
    extern const Setting GdtMaterials;
    // sound_aliases: also the zone's sound aliases as a Black Ops III Mod Tools alias CSV beside the
    // audio (SoundAliasCsv.h), sounds/<zone>.csv
    extern const Setting SoundAliases;
    // map_collision: also the map's collision as JSON (MapCollisionWriter.h):
    // <map>_collision.json, its clip map's brushes, brush models and triangles
    extern const Setting MapCollision;
    // map_content: everything, or the terrain alone (its meshes "terrain*" and their materials: a
    // quick look at a terrain, and the regress's terrain witnesses)
    extern const Setting MapContent;
    // decompile: compiled scripts also decompiled (acts or gsc-tool, by the game's VM), and the
    // compiled Lua files (LuaExport.h), else written as bytecode (ScriptExport.h)
    extern const Setting ScriptDecompile;
    const std::vector<Setting>& CommonSettings();

    // A material's images as its zone's reader reads them (XImage_t's usage as a number, the
    // semantic's hash, the image's name), by the material's name as the install index lists it
    struct MaterialImage
    {
        uint8_t Usage = 0;
        uint32_t Hash = 0;
        std::string Image;
    };
    using MaterialBook = std::map<std::string, std::vector<MaterialImage>>;
    class IDiskZone;

    // What a run writes, the same for every game.
    struct Context
    {
        std::string OutRoot;       // <--out>/<game id>
        std::string NameFilter;    // --name-filter, "a,b" (ExportNaming::NameFilter)
        std::set<Kind> Kinds;      // --type, all kinds when empty on the command line
        double ResolveSeconds = 0; // the adapter's naming or pulling of the zones (a CASC extract), logged
        std::string KindDir(Kind K) const;   // OutRoot/<folder>, created
        // An exporter's place within its kind: Done of Count assets, and the asset it starts now
        // when it names it (the run log times it until the next call: its slowest assets, what a
        // stall is stuck on). Export sets Step and throttles what reaches the progress callback,
        // so a reader may call Advance for every asset.
        std::function<void(size_t Done, size_t Count, const std::string* Asset)> Step;
        // False once the run is cancelled: the reader stops the kind there.
        bool Advance(size_t Done, size_t Count) const
        {
            if (Step) Step(Done, Count, nullptr);
            return !Cancelled();
        }
        bool Advance(size_t Done, size_t Count, const std::string& Asset) const
        {
            if (Step) Step(Done, Count, &Asset);
            return !Cancelled();
        }
        const std::atomic<bool>* Cancel = nullptr;
        bool Cancelled() const { return Cancel != nullptr && Cancel->load(); }

        // Only these assets when set, whatever NameFilter says: the dependencies a run pulls from
        // the zone that defines them, or the assets a user picked.
        std::shared_ptr<const Selection> Only;
        // --limit: at most this many assets of each kind, the first ones Wants accepts in the run
        // (all its zones); 0 for all. Not applied to Only (the dependencies those assets pull).
        size_t Limit = 0;
        struct Limited;
        std::shared_ptr<Limited> LimitSeen;   // Wants' count, shared by the run's copies (Export)
        // False: what the exported assets need from other zones is not pulled (Export's last
        // phase): a run measuring a sample of a zone's own assets (DiskGameSizes.h).
        bool PullDependencies = true;
        // Set: a dry run of the sound kind. A reader that can tells each sound it would write, as
        // the game's headers give it, reads no sample, writes nothing and counts the zone under
        // the counter "sound_headers" (DiskGameSizes.h); one that cannot exports as usual.
        std::function<void(const SoundHeader&)> OnSoundHeader;
        // The run's settings (ExportOptions::Settings, set by Export): Setting::Of reads them. The
        // tools given in ExportOptions too, as acts_exe and gsc_tool_exe; a decompiler not given there
        // as graywolf.json names it (acts_exe, gsc_tool_exe, cod_luajit_decompiler_exe, cod_lua_decompiler_exe, ffmpeg_exe).
        std::map<std::string, std::string> Settings;
        // One of Settings by its key, "" when unset
        std::string Setting(const char* Key) const
        {
            const auto It = Settings.find(Key);
            return It == Settings.end() ? std::string() : It->second;
        }
        // What a reader asks of every asset it may export: in Only when set, else NameFilter.
        bool Wants(Kind K, const std::string& Name) const;
        // An asset this zone cannot give whole (a name-only reference, a texture whose pixels are
        // elsewhere): the run exports it from the zone that defines it.
        void Needs(Kind K, const std::string& Name) const { if (OnNeeds) OnNeeds({ K, Name }); }
        // Work that needs what the run writes later (a map's textures pulled from another
        // zone): run once the dependencies are written, in the order given.
        void After(std::function<void()> Work) const
        {
            if (OnAfter) OnAfter(std::move(Work));
            else Work();
        }
        // Export's own: what Wants accepted and what Needs asked, zone by zone; the After work.
        std::function<void(const AssetId&)> OnWanted, OnNeeds;
        std::function<void(std::function<void()>)> OnAfter;
        // The materials a zone names only (a model's name-only material headers), as the zones the
        // install index says define them read them (IDiskZone::MaterialImages), into Out by name:
        // Export's own, the asking zone parked meanwhile; unset, none is looked up.
        std::function<void(IDiskZone& Asking, const std::set<std::string>& Names, MaterialBook& Out)> MaterialsElsewhere;
        // Set: a JSON tree kind (script bundles, AI types: ZonekitZone::ExportsBundleTrees) hands
        // each asset's tree here, (file stem, tree), and writes nothing: a game's anim owners read
        // the trees of a zone it holds open
        std::function<void(const std::string& Stem, nlohmann::ordered_json& Tree)> OnTree;
    };

    struct KindTotals
    {
        size_t Seen = 0, Written = 0;
        size_t Known = 0;        // same content already written by an earlier asset: not read again
        size_t Linked = 0;       // written as a hard link to an identical file
        size_t References = 0;   // name-only headers: another zone defines the asset
    };

    // What a run did, reported the same way for every game.
    struct Totals
    {
        size_t Zones = 0, ZonesWalked = 0, ZonesFailed = 0;
        std::map<Kind, KindTotals> Kinds;
        std::set<Kind> Unsupported;                // asked for, and this game cannot export it
        std::map<std::string, size_t> Counters;    // a reader's own measures (links_*, images_streamed...)
        std::vector<std::string> Failures;         // "<zone>: <kind> <name> <why>"
        std::vector<std::string> ZoneErrors;       // "<zone file>: <why>" for a zone that did not open
        // dependencies: what the exported assets needed, written from the zones that define it
        std::map<Kind, size_t> Pulled;
        std::map<Kind, std::set<std::string>> PulledFrom;
        std::vector<std::string> Unresolved;       // "<kind> <name>: <why>", a dependency not pulled
        std::vector<std::string> IndexRenamed;     // the install index built by the run: ZoneIndex::Renamed
        bool Cancelled = false;
        // where the time went (time_<phase>_ms, rate_<kind>..., slowest_<n>), as the run log sums it
        std::vector<std::pair<std::string, std::string>> Timing;
        void Fail(const std::string& Zone, Kind K, const std::string& Name, const std::string& Why);
        // "<zone>: <kind> <name> <why>": an asset the zone names whose file no archive or folder of
        // the install holds (the game ships without it). Reported, not a failure.
        std::vector<std::string> Absent;
        void Lacks(const std::string& Zone, Kind K, const std::string& Name, const std::string& Why);
        // the run's asset index: one row per file a data kind wrote (bundles, mapinfo, struct dumps,
        // mapents), written once after the run as <out>/asset_index.csv (Export)
        struct IndexRow
        {
            Kind K;
            std::string Id, Type, TypeName, Zone, Name, File;
        };
        std::vector<IndexRow> Indexed;
        void Index(Kind K, std::string Id, std::string Type, std::string TypeName, std::string Zone, std::string Name, std::string File)
        {
            Indexed.push_back({ K, std::move(Id), std::move(Type), std::move(TypeName), std::move(Zone), std::move(Name), std::move(File) });
        }
    };

    class IDiskZone
    {
    public:
        using Exporter = std::function<void(const Context&, Totals&)>;

        IDiskZone() = default;
        IDiskZone(const IDiskZone&) = delete;   // exporters hold this zone
        IDiskZone& operator=(const IDiskZone&) = delete;
        virtual ~IDiskZone() = default;
        virtual const std::string& Name() const = 0;
        virtual bool WalkedToEnd() const = 0;
        // the reader's own measures for this zone (links resolved...), added to Totals::Counters
        virtual void Measures(Totals&) const {}
        // What the zone holds whole: every asset its exporters write under that name. The install
        // index is made of it; a reader that lists nothing has no dependency pulled.
        virtual void Defines(std::vector<AssetId>&) const {}
        // The parts it holds that other zones' assets name by hash, (asset type, hash): an
        // xmodelsurfs a model of another zone points at (Black Ops 6). Kept in the install index
        // beside Defines (ZoneIndex::PartIn), for a reader to load the zone holding a part.
        virtual void Parts(std::vector<std::pair<std::string, uint64_t>>&) const {}
        // The localized strings it holds, (key, text) in zone order, the text in the game's own
        // encoding; an empty key is a reference to another zone's. What the localize kind writes,
        // and what level names are read from (LevelNames).
        virtual void Strings(std::vector<std::pair<std::string, std::string>>&) const {}
        // Its string tables (as CSV text, a line per row) and the text rawfiles its reader reads,
        // (name, text): where a game's level list is (LevelNaming::Lists).
        virtual void Documents(std::vector<std::pair<std::string, std::string>>&) const {}
        // The levels it holds when it holds several (a CoD1/CoD2 archive: its maps' file names
        // without extension), what LevelNaming::ByLevels names it by.
        virtual void HeldLevels(std::vector<std::string>&) const {}
        // What else makes an index of this reader's zones stale (its tables), and about how much
        // memory an open zone holds.
        virtual std::string IndexKey() const { return std::string(); }
        virtual uint64_t Footprint() const { return 0; }
        // How long opening it took, step by step ("load", "walk"...), in seconds: the run log's.
        virtual void OpenTimes(std::vector<std::pair<std::string, double>>&) const {}
        // Set aside, read but not in use, for a later round of dependencies to ask again without
        // reading it anew; Resume before its next Export. False when the reader cannot (the zone is
        // then closed). Only one zone is in use at a time.
        virtual bool Park() { return false; }
        virtual void Resume() {}
        // The images of every material it defines whole, by name, as its reader reads them; false
        // when its reader reads none (Context::MaterialsElsewhere)
        virtual bool MaterialImages(MaterialBook&) { return false; }

        // The one dispatch: K's exporter, false when this reader has none ("not supported").
        bool Export(Kind K, const Context& C, Totals& T) const;
        // the kinds it has an exporter for
        std::set<Kind> Supported() const;

    protected:
        // A reader supports K by giving its exporter; a later call replaces an earlier one.
        void Exports(Kind K, Exporter E) { Exporters[K] = std::move(E); }
        const Exporter* ExporterOf(Kind K) const;

    private:
        std::map<Kind, Exporter> Exporters;
    };

    class IDiskGame
    {
    public:
        virtual ~IDiskGame() = default;
        virtual const char* Id() const = 0;       // "ghosts": the output folder and --game
        virtual const char* Title() const = 0;
        virtual const char* Engine() const = 0;   // GameModule engine ("IW6"), for detection
        // extra subcommand names, e.g. "ghosts-disk-export"
        virtual std::vector<std::string> Aliases() const { return {}; }
        // The choices this game takes (none by default).
        virtual const std::vector<Setting>& Settings() const;
        // Its own settings, then the common ones concerning a kind it exports (what the CLI and
        // the panel offer).
        std::vector<const Setting*> AllSettings() const;
        // The kinds its readers export, read once off Unopened()'s Exports(): what the GUI offers
        // and what a run asks its zones for (the others are reported unsupported).
        std::set<Kind> Kinds() const;
        // A zone of this game's reader, constructed but not opened, for its exporters only: its
        // constructor registers them and touches nothing else (no shared state, no file).
        virtual std::unique_ptr<IDiskZone> Unopened() const = 0;

        // The zones an input names; by default a .ff, or every .ff under a folder.
        virtual bool Zones(const std::string& Input, const ExportOptions& Options,
                           std::vector<std::string>& Out, std::string& Error) const;
        // The zones an install holds, by name, without reading or extracting any (what a user picks
        // from); then the picked names as zones Open takes. By default every non-empty .ff under the
        // folder, named by its path relative to it, then those of an Xbox app game's DLC packages
        // (XboxPackages::Dlc), named "<package name>\<path in it>".
        virtual bool ListZones(const std::string& Install, const ExportOptions& Options,
                               std::vector<std::string>& Names, std::string& Error) const;
        virtual bool ResolveZones(const std::string& Install, const std::vector<std::string>& Names,
                                  const ExportOptions& Options, std::vector<std::string>& Out, std::string& Error) const;
        // The zones the install index opens (ZoneIndex): every zone ListZones gives, unless a game
        // names fewer (Modern Warfare 2019 leaves out its 25 000 transient zones)
        virtual bool IndexedZones(const std::string& Install, const ExportOptions& Options,
                                  std::vector<std::string>& Names, std::string& Error) const
        {
            return ListZones(Install, Options, Names, Error);
        }
        // Whether the install index may open several zones at once (ZoneIndex): what Open does
        // before a zone's session is safe on several threads (its decompression, its walk, its
        // ResolveZones once the index serializes it); a zonekit zone's session, handlers and
        // fixups hold the process-wide session lock. False until a game's hooks are checked.
        virtual bool IndexInParallel() const { return false; }
        // The install a zone sits in, when --install is not given: where its dependencies are
        // pulled from. By default the zone's folder; a game whose zones sit in a subfolder of the
        // install (Black Ops 6's cod24\, sp24\) names the install above it.
        virtual std::string InstallOfZone(const std::string& Zone) const;
        // A zone read and ready to export (nullptr and Error when it does not read).
        virtual std::unique_ptr<IDiskZone> Open(const std::string& Zone, const ExportOptions& Options,
                                                std::string& Error) const = 0;

        // Whether its readers honour Context::NameFilter; false for one that writes a kind whole.
        virtual bool FiltersByName() const { return true; }

        // The image format when --image-format is not given (nullptr: the saved setting).
        virtual const char* DefaultImageFormat() const { return nullptr; }

        // Where its levels' display names are, in its own localized strings (LevelNames): the
        // zones holding them, and the keys a level zone's name is looked up with. Names are
        // file names without extension, case-insensitive, '*' and '?' wildcards.
        struct LevelNaming
        {
            // the zones whose strings are read, later ones taking precedence ("??_patch": fr_patch)
            std::vector<std::string> From;
            // per level zone pattern, the keys tried in order, {NAME} being the zone's name and
            // {REST} / {REST2} what the pattern's first / second '*' matched, upper-cased ("mp_*" ->
            // "MPUI_{REST}")
            std::vector<std::pair<std::string, std::vector<std::string>>> Keys;
            // the game's own level lists in those zones (IDiskZone::Documents), tried first: an
            // .arena rawfile (a map's longname, a key or the name itself) or a string table (a
            // map's row, the other cells the keys tried)
            std::vector<std::string> Lists = {};
            // the strings' code page when they are not UTF-8 (CODOL's GBK: 936), 0 for Windows-1252
            unsigned CodePage = 0;
            // how the zones name a key when the game keeps it hashed (BO4's localize_<hex>), each
            // key also looked up that way; nullptr when keys are kept as text
            std::string (*Hashed)(const std::string& Key) = nullptr;
            // a zone is a container of levels, named by those it holds (IDiskZone::HeldLevels): every
            // zone is opened once
            bool ByLevels = false;
        };
        // none by default: the game's zones are shown by file name only
        virtual const LevelNaming* LevelNames() const { return nullptr; }

        // Which models the anims a run exported play on, as the game's own data says (AnimOwners.h):
        // the run writes <out>/<game>/anims/anim_owners.json from it, merged with an earlier run's.
        // A zone that wrote anims, its documents (IDiskZone::Documents) whose name fits one of
        // AnimOwnerDocuments' patterns ('*' any run, no case) kept while it was open.
        struct AnimZone
        {
            std::string Zone;                                              // as Open took it
            // as exported: the zone's own, then those it named that another zone defines (its
            // dependencies, pulled later: the facts of the zone naming an anim own it)
            std::vector<std::string> Anims;
            std::vector<std::pair<std::string, std::string>> Documents;    // (name, text)
        };
        // the documents AnimOwners reads; none by default (no zone's documents are listed)
        virtual std::vector<std::string> AnimOwnerDocuments() const { return {}; }
        // Called once per run that wrote anims, after its zones, none open (the game may Open others:
        // its shared zones). Out: owners and the run's anims they play (others, and a dependency
        // the run could not pull, are left out). Nothing
        // by default: the preview then guesses by bones.
        virtual void AnimOwners(const ExportOptions&, const std::vector<AnimZone>&, ::AnimOwners::Book&) const {}
    };

    struct Registration
    {
        explicit Registration(const IDiskGame& Game);
    };
    const std::vector<const IDiskGame*>& Games();
    const IDiskGame* Find(const std::string& IdOrAlias);
    // The game a zone (or the first .ff under a folder) belongs to, by magic and version.
    const IDiskGame* Detect(const std::string& Input);

    // Where a run is, for a progress bar: zone Zone (0-based) of Zones, being read (no Current)
    // or exporting Current, Done of its Count assets (0 of 0 until its reader says); Running is
    // what the run did so far.
    // Asset is the one being exported, when its reader names it. The watchdog also calls the
    // callback, from its own thread, with only Stalled set (its warning, empty once the run moves
    // again) and FromWatchdog.
    struct Progress
    {
        size_t Zone = 0, Zones = 0;
        std::string ZoneName;
        std::optional<Kind> Current;
        size_t Done = 0, Count = 0;
        std::string Asset;
        const Totals* Running = nullptr;
        bool FromWatchdog = false;
        std::string Stalled;
        // Pulling a dependency after every zone, whose total is not known ahead of time: an ETA
        // (see Eta below) shows this as its own phase instead of a number it cannot back up.
        bool Dependencies = false;
    };
    using ProgressFn = std::function<void(const Progress&)>;

    // A rough time-left estimate built from the Progress a run emits, nothing game-specific.
    // Per kind, since a sound decode and a PNG write cost nothing alike; "estimating..." until
    // enough of the run has been seen, and its own phase text while dependencies are pulling.
    class Eta
    {
    public:
        Eta() = default;
        explicit Eta(std::set<Kind> Wanted) : WantedKinds(std::move(Wanted)) {}
        void Reset(std::set<Kind> Wanted) { *this = Eta(std::move(Wanted)); }
        void Observe(const Progress& P);
        std::string Text() const;

    private:
        std::set<Kind> WantedKinds;
        std::map<Kind, double> KindSeconds;      // total measured time spent in a kind, per zone
        std::map<Kind, size_t> KindZonesSeen;     // zones that measurement covers
        size_t CurZone = static_cast<size_t>(-1);
        std::optional<Kind> CurKind;
        std::chrono::steady_clock::time_point KindStarted, LastTick;
        size_t LastDone = 0, LastCount = 0;
        double CurKindRate = 0.0;                 // assets/sec, smoothed
        size_t TotalZones = 0;
        bool InDeps = false;
        std::string DepZone;
        size_t DepDone = 0, DepCount = 0;
    };

    // Where what is kept for an install goes: <--cache>/index (or %TEMP%/greyhound_index),
    // <game>_<install hash><Extension>
    std::string KeptFile(const IDiskGame& Game, const ExportOptions& Options, const std::string& Install,
                         const char* Extension);
    // The installs of Game an index was kept for (each one's folder is written beside it, <...>.install)
    std::vector<std::string> KeptInstalls(const IDiskGame& Game, const ExportOptions& Options);

    // Where each asset of an install is defined (IDiskZone::Defines of every zone ListZones
    // gives), built once and kept in a file keyed by the game, the install, the zone list and the
    // reader's IndexKey. What a run pulls dependencies from, and a zone's asset list for a picker.
    class ZoneIndex
    {
    public:
        // The cached index, or one built now (every zone opened in turn, Cancel checked between
        // zones). nullptr and Error when the install lists no zone or the build was cancelled.
        static std::unique_ptr<ZoneIndex> For(const IDiskGame& Game, const ExportOptions& Options,
                                              const std::string& Install, const std::string& ReaderKey,
                                              const std::function<void(size_t Done, size_t Count)>& Step,
                                              const std::atomic<bool>* Cancel, std::string& Error);
        // The index kept for this install whatever the reader's key (counts to show, possibly
        // from older tables), nullptr when none was built for its current zone list.
        static std::unique_ptr<ZoneIndex> Kept(const IDiskGame& Game, const ExportOptions& Options,
                                               const std::string& Install);
        // For, the reader's key taken from the first zone that opens (what Export does).
        static std::unique_ptr<ZoneIndex> Build(const IDiskGame& Game, const ExportOptions& Options,
                                                const std::string& Install,
                                                const std::function<void(size_t Done, size_t Count)>& Step,
                                                const std::atomic<bool>* Cancel, std::string& Error);
        const std::vector<std::string>& Zones() const { return Names; }   // as ListZones names them
        uint64_t Footprint(size_t Zone) const { return Sizes[Zone]; }
        const std::vector<AssetId>& Defined(size_t Zone) const { return Assets[Zone]; }
        // the zones that define A, in ListZones order
        std::vector<size_t> DefinedIn(const AssetId& A) const;
        // the zones holding the part (IDiskZone::Parts), in ListZones order
        std::vector<size_t> PartIn(const std::string& Type, uint64_t Hash) const;
        std::string File;                  // where it is kept
        bool Built = false;                // built by this call, not read from File
        // built now: "<zone>: <kind> <its bytes> -> <name>", each name a reader gave that was not
        // text (control bytes, not UTF-8), kept under a fallback name (DiskGame.Index.cpp)
        std::vector<std::string> Renamed;
        double Seconds = 0;                // how long building or reading it took
        size_t Entries = 0;

    private:
        // File read in when its key is Key (any key when Key is empty); false when it is not
        bool Load(const std::string& Key);
        void Add(size_t Zone, AssetId A);

        std::vector<std::string> Names;
        std::vector<uint64_t> Sizes;
        std::vector<std::vector<AssetId>> Assets;
        // (hashed: an index of every zone of an install holds millions of entries; an asset's name
        // compares without case, as AssetId's order does)
        struct AssetHash
        {
            size_t operator()(const AssetId& A) const;
        };
        struct AssetSame
        {
            bool operator()(const AssetId& A, const AssetId& B) const { return !(A < B) && !(B < A); }
        };
        struct PartHash
        {
            size_t operator()(const std::pair<std::string, uint64_t>& P) const
            {
                return std::hash<std::string>()(P.first) ^ (size_t)(P.second * 0x9E3779B97F4A7C15ull);
            }
        };
        std::unordered_map<AssetId, std::vector<size_t>, AssetHash, AssetSame> Where;
        std::unordered_map<std::pair<std::string, uint64_t>, std::vector<size_t>, PartHash> PartsWhere;
    };

    // The display names of an install's levels ("zm_highrise" -> "Die Rise"), read from the
    // game's own localized strings as its IDiskGame::LevelNames says, in the install's language.
    // Built once (the From zones opened in turn) and kept beside the install index, keyed by the
    // game, the install, its zone list and the naming rules.
    class LevelNames
    {
    public:
        // nullptr when the game names no level (Error empty) or its zones did not list (Error)
        static std::unique_ptr<LevelNames> For(const IDiskGame& Game, const ExportOptions& Options,
                                               const std::string& Install,
                                               const std::function<void(size_t Done, size_t Count)>& Step,
                                               std::string& Error);
        // A zone's level name, as ListZones names the zone; "" for a zone that is not a level
        std::string Of(const std::string& Zone) const;
        size_t Named() const { return Names.size(); }
        bool Built = false;   // built by this call, not read from its file

        // The name rules give a zone (a file name without extension) from Strings, "" for none.
        // A zone's language prefix (fr_, fra_...), localized_ / patch_ prefix and _patch / _load
        // suffix are tried off when its own name finds nothing.
        // Listed: the level lists' maps and their keys.
        static std::string Lookup(const IDiskGame::LevelNaming& Rules, const std::string& Zone,
                                  const std::map<std::string, std::string>& Strings,
                                  const std::map<std::string, std::vector<std::string>>& Listed);

    private:
        std::map<std::string, std::string> Names;   // ListZones name -> level name (UTF-8)
    };

    // The use case: Zones (as Zones/ResolveZones give them) exported for Game into C. What the
    // exported assets need from other zones is then exported from the zones that define them
    // (ZoneIndex), each such zone opened once, alone, dependencies of dependencies included.
    // Cancel is checked between zones and within kinds; a cancelled run returns what it did.
    // Every run appends to RunLogPath(C): its phases timed, its slowest assets, a watchdog's stall
    // warnings (GH_EXPORT_STALL_SECONDS, 30 by default; GH_EXPORT_STALLDUMP=<file> writes a
    // minidump on the first). ResolveSeconds: what naming or pulling the zones took the adapter.
    std::string RunLogPath(const Context& C);
    Totals Export(const IDiskGame& Game, const ExportOptions& Options, const std::vector<std::string>& Zones,
                  const Context& C, const ProgressFn& OnProgress = nullptr, const std::atomic<bool>* Cancel = nullptr);

    std::string Report(const Totals& T);
}
