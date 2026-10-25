#include "stdafx.h"

// Black Ops 6 (multiplayer and zombies, cod24) on the disk template: a ZonekitZone with the bo6
// profile, served to GameBlackOps6's readers. The install is plain Steam files: the .ff (their .fp
// patches beside them, Games/IW/Container25.cpp) and the streamed data in .xsub packages (cod24,
// sp24, and the root's codhq24_shared). Pointers resolve by key and by name hash (the profile's
// stream.keyed_refs), names are 63-bit hashes unless the zone carries the string.

#include "Games/Common/MeshDecode.h"
#include "DiskGame.h"
#include "FileSystems.h"
#include "Games/BO6/TechsetFormat.h"
#include "Games/BO6/VfxLayout.h"
#include "GameBlackOps6.h"
#include "Games/IW/IwctxReaders.h"
#include "Games/IW/IwLanguageNames.h"
#include "Games/IW/IwWeaponStats.h"
#include "HalfFloats.h"
#include "MapLive.h"
#include "SettingsManager.h"
#include "Strings.h"
#include "Games/IW/IwBesideZones.h"
#include "Games/IW/IwEntityKeys.h"
#include "Games/IW/IwTerrain.h"
#include "Games/Treyarch/TreyarchPackages.h"
#include "WraithNameIndex.h"
#include "XSUBCache.h"
#include "ZonekitZone.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <mutex>
#include <unordered_set>

namespace
{
    namespace fs = std::filesystem;

    using DiskGame::Context;
    using DiskGame::ExportOptions;
    using DiskGame::Kind;
    using DiskGame::Totals;

    class BlackOps6Zone final : public ZonekitZone
    {
    public:
        // Campaign: a campaign zone (sp24, profile bo6sp, the readers' campaign layout); its world
        // (gfx_map 0x9aa0, the GfxWorld inline) is not read yet
        explicit BlackOps6Zone(std::string Path, bool Campaign = false) : ZonekitZone(std::move(Path)), Campaign(Campaign)
        {
            // a model's mesh may be another zone's xmodelsurfs: those zones are loaded beside first
            if (const auto* Models = ExporterOf(Kind::Model))
                Exports(Kind::Model, [this, Read = *Models](const Context& C, Totals& T) {
                    LoadBeside();
                    LoadMaterialsBeside();
                    Read(C, T);
                });
            Exports(Kind::Image, [this](const Context& C, Totals& T) { ExportImages(C, T); });
            Exports(Kind::Map, [this](const Context& C, Totals& T) { ExportMap(C, T); });
            Exports(Kind::Sound, [this](const Context& C, Totals& T) { ExportSounds(C, T); });
            // gscobj (0x18, 0x283db80): hash 0, size u32 8, the GSCOBJ at 0x10; decompiled by acts
            Exports(Kind::Script, [this](const Context& C, Totals& T) {
                constexpr Scripts::Vm Bo6Vm{ Scripts::Tool::Acts, "", "pc", "c", "cod24-cod_dump.exe" };
                constexpr Scripts::Vm Bo6SpVm{ Scripts::Tool::Acts, "", "pc", "c", "sp24-cod_dump.exe" };
                ExportScripts(C, T, this->Campaign ? Bo6SpVm : Bo6Vm, { { "gscobj", [this](int64_t Pos, Scripts::Script& S) {
                                  S.Compiled = true;
                                  return BytesAt(PointerAt(Pos + 0x10), At<uint32_t>(Image(), Pos + 8), S.Bytes);
                              } } });
            });
            // rawfiles, luafile (0x68) {hash, i32 size 0x3c, buffer 0x40} and string tables
            ExportsHashedRawfiles(0x3c, 0x40);
            ExportsMaterialLinks({ 0x1c, 0x21, 0x30, 0x38, 0x28, 0x10, 0x20, 0x48, 0x22, 0x90 });
            // soundbank (0x50; the campaign's 0x88, aliases 0x28, count 0x20): aliases 0x20 (u32
            // count 0x48) of 0x40 {entries 0x10, hash 0x18, u16 count 0x2c}, an entry 0x38 with its
            // sndasset at 0x28
            ExportsSoundLinks({ Campaign ? 0x28 : 0x20, Campaign ? 0x20 : 0x48, 0x40, 0x18, 0x10, 0x2c, 0x38, 0x28 });
            // (the campaign's techset and technique differ, 0x38 and 0xb0: not read)
            if (!Campaign) ExportsTechsets(BO6::TechsetFormat());
            // material (0x80) with its atlas, and particlesystem (0x80) as the vfx core's JSON (Games/IW/Vfx.h);
            // the campaign's module table is not read
            if (!Campaign)
            {
                ExportsMaterials("material", Treyarch::MaterialKind(Materials(), [this](uint64_t A, nlohmann::ordered_json& J) {
                    Vfx::AddAtlas(J, BO6::VfxLayout(), Image(), Off(A), 0x80);
                }));
                Exports(Kind::Fx, [this](const Context& C, Totals& T) {
                    auto Z = EffectView();
                    Z.AssetName = [this](int64_t Pos) { return AssetName(Kind::Fx, Pos); };
                    Z.ReadMaterials(C, Materials());
                    Vfx::Counts N;
                    ExportEffects(C, T, "particlesystem", [&](int64_t Pos, const std::string& Name, std::string& J) {
                        // an asset another zone defines: its name hash (bit 63), named as that zone names it
                        return Vfx::ReadSystem(Z, BO6::VfxLayout(), Pos, Name, J, &N, [](uint64_t A) {
                            return A >> 63 ? GameBlackOps6::HashedName("asset", A) : std::string();
                        });
                    });
                    for (const auto& [K, V] : N.Named()) Counter(K) += V;
                });
            }
            ExportsColumnTables(ColumnTypes::Bo6, [](const std::string& Raw) { return Raw; });
            // scriptbundle (0x30, 0x285c8e0): Black Ops 7's but for an array (0x18 {typed entries 0,
            // u32 count 0x10}, 0x285d7c0) and an animation (9, 18, its record inline: written raw)
            ScriptBundleLayout Bundles;
            Bundles.ArrayType = -1, Bundles.ArrayElements = 0, Bundles.ArrayCount = 0x10, Bundles.ArrayStride = 0x10;
            Bundles.Types[9] = Bundles.Types[18] = BundleValue::Raw;
            // (the campaign's value loader 0x25b74c0 differs: not read)
            if (!Campaign)
            {
                ExportsScriptBundles(Bundles);
                // mapinfo (0xd0, its loader 0x2845bc0): Black Ops 7's but its scriptbundle table at 0xb0 (the
                // table loader 0x285ce50); string fields 8 to 0x30, 0x40, 0x48, an image at 0x38
                ExportsBundleTrees(Kind::MapInfo,
                                   { { "mapinfo", 0xb0, -1,
                                       { { "map", 0x08 }, { "gametypes", 0x10 }, { "script_folder", 0x18 }, { "0x20", 0x20 }, { "0x28", 0x28 },
                                         { "loading_image_name", 0x30 }, { "omnvars_table", 0x40 }, { "0x48", 0x48 } },
                                       { { "loading_image", 0x38 } },
                                       [](const std::string& Name) { return IwctxReaders::AssetHash(Name); } } },
                                   Bundles);
                // aitype (0x158, 0x28197c0): a scriptbundle's table at 0xa8 (0x285ce50), a string at 0x70
                ExportsBundleTrees(Kind::AiTypes,
                                   { { "aitype", 0xa8, -1, { { "0x70", 0x70 } }, {},
                                       [](const std::string& Name) { return IwctxReaders::AssetHash("aitype:" + Name); } } },
                                   Bundles);
            }
            // scriptable (0xd0) and scriptablevariant (0x58), the campaign's too: no key/value table,
            // dumped by offset
            ExportsStructDumps(Kind::Scriptables, { { "scriptable" }, { "scriptablevariant" } });
            // the zone's entity strings, one TSV (IwctxReaders::ExportMapEnts)
            Exports(Kind::MapEnts, [this](const Context& C, Totals& T) {
                std::vector<IwctxReaders::MapEntity> Ents;
                EntityStrings(Ents);
                IwctxReaders::ExportMapEnts(C, T, Name(), Ents);
            });
            // the weapon assets as struct dumps (Black Ops 7's types), a weapon's name at 8, an
            // attachment's at 0x238 (the campaign's 0x128, its fields by offset only), the fields the
            // weapon stats prove named (StatsLayout)
            if (Campaign)
                ExportsStructDumps(Kind::Weapons, { { "weapon", 8 }, { "attachment", 0x128 }, { "weapontunables" }, { "weaponaccuracy" },
                                                    { "weapontrigger" }, { "weaponblueprint" }, { "attachmentblueprint" },
                                                    { "deployableweaponconfig" }, { "animpkg" }, { "suit" } });
            else
            {
                const auto& L = StatsLayout();
                ExportsStructDumps(Kind::Weapons,
                                   { { "weapon", L.WeaponName, IwWeaponStats::DumpFields(L, "weapon") },
                                     { "attachment", L.AttachmentName, IwWeaponStats::DumpFields(L, "attachment") },
                                     { "weapontunables", -1, IwWeaponStats::DumpFields(L, "weapontunables") }, { "weaponaccuracy" },
                                     { "weapontrigger" }, { "weaponblueprint" }, { "attachmentblueprint" }, { "grappleweapontunables" },
                                     { "deployableweaponconfig" }, { "animpkg", -1, IwWeaponStats::DumpFields(L, "animpkg") },
                                     { "suit", -1, IwWeaponStats::DumpFields(L, "suit") } });
                // the weapon stats table: this zone's facts, then the table rebuilt from every zone's
                Exports(Kind::WeaponStats, [this](const Context& C, Totals& T) {
                    IwWeaponStats::ZoneView V;
                    V.Image = &Image();
                    V.Walk = &Walk();
                    V.Off = [this](uint64_t A) { return Off(A); };
                    V.Text = [this](int64_t Field) { return TextAt(Field); };
                    V.LocalizeText = [this](int64_t Pos) { return TextAt(Pos + 8); };
                    IwWeaponStats::Export(StatsLayout(), V, fs::path(FastFilePath()).stem().string(), C, T);
                });
            }
        }

        // ---- the weapon stats' layout (Games/IW/IwWeaponStats.h): offsets read off the code of
        // bo6_flat.exe (RVAs of cod24-cod_dump.exe, PE timestamp 1783954360), each function matched to
        // Black Ops 7's by the GSC builtin that calls it (the same name hash in both exes) or by its
        // code, the field read at the same place
        static const IwWeaponStats::Layout& StatsLayout();

        bool ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models) override;
        // the zone's entity strings: map_game_objects' pairs, then each script brush model's
        void EntityStrings(std::vector<IwctxReaders::MapEntity>& Out) const;

        // What LoadBeside looks the meshes up in: the install index of this game
        void SetInstall(const DiskGame::IDiskGame* G, const ExportOptions& O, const std::string& I)
        {
            Game = G;
            Options = O;
            Install = I;
        }

        // What other zones name and this one holds (the install index's parts): its xmodelsurfs,
        // materials and images by name hash (another zone's models' meshes, materials and their
        // images), its gfx_map_trzone by map and number (map id u32 0x10, zone number u32 0x14: a
        // map's surfaces name their transient zone by number)
        void Parts(std::vector<std::pair<std::string, uint64_t>>& Out) const override
        {
            for (const auto& I : Walk().Instances)
                if (OwnerIs(I, "xmodelsurfs") || OwnerIs(I, "material") || OwnerIs(I, "image"))
                    Out.emplace_back(Walk().OwnerNames[I.Owner], At<uint64_t>(Image(), I.Pos) & 0x7FFFFFFFFFFFFFFFull);
                else if (OwnerIs(I, "gfx_map_trzone"))
                    Out.emplace_back("gfx_map_trzone", (uint64_t)At<uint32_t>(Image(), I.Pos + 0x10) << 32 | At<uint32_t>(Image(), I.Pos + 0x14));
        }

    protected:
        const char* Profile() const override { return Campaign ? "bo6sp" : "bo6"; }
        const ICoDGame& LiveGame() const override { return LiveGames::Get(Campaign ? "BO6SP" : "BO6"); }
        VirtualGameSession::Handlers Handlers() override
        {
            GameBlackOps6::PerformInitialSetup();
            // the package index's bo6_names.wni (the campaign's too): names found for the game's
            // hashes in its own strings and the index's (cod-hash-recovery; the hash packages)
            static std::once_flag Names;
            std::call_once(Names, [] {
                CoDAssets::AssetNameCache.LoadIndex(
                    (fs::path(FileSystems::GetApplicationPath()) / "package_index" / "bo6_names.wni").string());
            });
            return { SlotString, Campaign ? GameBlackOps6::LoadXImageCampaign : GameBlackOps6::LoadXImage };
        }

        // a model's name string at 8 when the zone has it ("base::variant" kept as base_variant),
        // else any asset's 63-bit hash at 0, named from the index when known
        std::string AssetName(Kind K, int64_t Pos) const override
        {
            if (K == Kind::Model)
            {
                std::string Name = StringAt(PointerAt(Pos + GameBlackOps6::ModelName));
                for (size_t At = Name.find("::"); At != std::string::npos; At = Name.find("::")) Name.replace(At, 2, "_");
                if (!Name.empty()) return Name;
            }
            const char* Type = K == Kind::Model ? "xmodel" : K == Kind::Anim ? "xanim" : K == Kind::Image ? "ximage" : K == Kind::Script ? "script"
                             : K == Kind::Rawfile ? "rawfile" : K == Kind::Data ? "stringtable" : "asset";
            return GameBlackOps6::HashedName(Type, At<uint64_t>(Image(), Pos));
        }
        // localize (0x10, 0x2842a90): the key's hash at 0 (named from the index when known), the
        // text at 8, plain UTF-8
        void LocalizeEntry(int64_t Pos, std::string& Key, std::string& Text) const override
        {
            Key = GameBlackOps6::HashedName("localize", At<uint64_t>(Image(), Pos));
            Text = TextAt(Pos + 8);
        }

        // every zone-level asset is streamed whole (its list header -1); a name-only reference is
        // a string read in place of the pointer, never an asset instance
        bool IsReference(Kind, const std::string&, int64_t) const override { return false; }

        void Defines(std::vector<DiskGame::AssetId>& Out) const override
        {
            ZonekitZone::Defines(Out);
            for (const auto& I : Walk().Instances)
                if (OwnerIs(I, "image") && HasPixels(I.Pos)) Out.push_back({ Kind::Image, AssetName(Kind::Image, I.Pos) });
        }
        std::string IndexKey() const override { return ZonekitZone::IndexKey() + "+images1+parts3"; }
        void Served(bool On) override
        {
            if (On)
            {
                Beside.Serve();
                IwctxReaders::ServeSlotStrings(Strings);
            }
        }
        // the zone's script strings: they name the anims' notetracks, by hash (multiplayer) or slot
        // (the campaign)
        void Fixups() override
        {
            std::string Error;
            if (const auto* Tables = ExportWriters::ZoneProfile(Profile(), Error))
                Strings = IwctxReaders::ZoneStrings(Zone(), Tables->Profile.Dir, (size_t)Rules().ZoneLoadValue("asset_list_size", 0x30));
            IwctxReaders::RegisterScriptStrings(Strings);
            IwctxReaders::ServeSlotStrings(Strings);
        }

    private:
        // the live material reader, for the material and fx kinds, with the images another zone holds
        // (a name hash in the slot: named, the image kind takes them from there); slots {u8 semantic,
        // u8 image} 0x30 (count u8 0x1c), images 0x38 (count u8 0x21), colour 44/82/96/110/124; emissive,
        // opacity, transmittance and ramp 84, 85, 88, 25 (IwctxReaders::SlotMaterials)
        decltype(Treyarch::EffectZone::Material) Materials()
        {
            static const IwctxReaders::MaterialSlots L{ 0x80, 0x30, 0x1c, 0x38, 0x21, { 44, 82, 96, 110, 124 }, 84, 85, 88, 25 };
            return IwctxReaders::SlotMaterials(
                L, [](uint64_t A) { return GameBlackOps6::ReadXMaterial(A); }, [this]() -> const std::vector<uint8_t>& { return Image(); },
                [this](uint64_t A) { return Off(A); }, [](uint64_t V) { return GameBlackOps6::HashedName("ximage", V); });
        }

        bool Campaign = false;
        std::vector<std::string> Strings;
        const DiskGame::IDiskGame* Game = nullptr;
        ExportOptions Options;
        std::string Install;
        IwBeside::Set Beside;
        bool BesideTried = false, MaterialsTried = false;
        // the transient zone numbers the map's surface records name (ReadMap's layout)
        std::set<int32_t> TransientNumbers() const;

        // The zones holding the xmodelsurfs this zone's models name by hash (another zone's: the
        // game has the global zones and a map's transient zones loaded with it), found in the
        // install index's parts and loaded beside (Games/IW/IwBesideZones.h), each reference then
        // linked across them. Once per zone, before models or a map are read.
        void LoadBeside()
        {
            if (BesideTried || Game == nullptr || Install.empty()) return;
            BesideTried = true;
            const auto& Rs = Rules();
            const auto& R = Walk();
            std::set<uint64_t> Wanted;
            for (size_t K = 0; K < R.BackRefs.size(); K++)
            {
                if (K >= R.BackRefIsAsset.size() || !R.BackRefIsAsset[K]) continue;
                const auto Type = R.RefTypes.find(R.BackRefs[K].first);
                if (Type == R.RefTypes.end() || Type->second >= R.OwnerNames.size() || R.OwnerNames[Type->second] != "xmodelsurfs") continue;
                if (Off(PointerAt(R.BackRefs[K].first)) < 0) Wanted.insert(R.BackRefs[K].second & Rs.AssetHashMask);
            }
            Counter("model_meshes_elsewhere") += Wanted.size();
            // a map's transient zones: the numbers its surfaces name that it does not hold
            std::set<uint64_t> Transient;
            if (const int64_t Own = Root("gfx_map_trzone"); Own >= 0)
            {
                const uint64_t Map = (uint64_t)At<uint32_t>(Image(), Own + 0x10) << 32;
                for (const int32_t N : TransientNumbers())
                    if (N != At<int32_t>(Image(), Own + 0x14)) Transient.insert(Map | (uint32_t)N);
            }
            Counter("map_transient_zones_named") += Transient.size();
            if (Wanted.empty() && Transient.empty()) return;
            std::string Error;
            // the index (built now when the install has none: every zone opened in turn, which
            // needs this one's session ended meanwhile)
            Park();
            const auto Index = DiskGame::ZoneIndex::For(*Game, Options, Install, IndexKey(), nullptr, nullptr, Error);
            Resume();
            if (!Index)
            {
                Counter("model_meshes_no_index")++;
                return;
            }
            // the zones giving the most of them first, this one left out
            // (a trzone key gets bit 63, apart from any 63-bit hash)
            std::map<size_t, std::set<uint64_t>> Gives;
            const auto Give = [&](const char* Type, uint64_t Part, uint64_t Tag) {
                for (const size_t Z : Index->PartIn(Type, Part))
                    if (ExportNaming::Lower(fs::path(Index->Zones()[Z]).stem().string()) != ExportNaming::Lower(Name())) Gives[Z].insert(Part | Tag);
            };
            for (const uint64_t H : Wanted) Give("xmodelsurfs", H, 0);
            for (const uint64_t T : Transient) Give("gfx_map_trzone", T, 1ull << 63);
            std::vector<std::string> Paths;
            std::set<uint64_t> Left = Wanted;
            for (const uint64_t T : Transient) Left.insert(T | 1ull << 63);
            for (const size_t Z : IwBeside::Cover(std::move(Gives), Left))
            {
                std::vector<std::string> Found;
                std::string Why;
                if (Game->ResolveZones(Install, { Index->Zones()[Z] }, Options, Found, Why) && !Found.empty()) Paths.push_back(Found.front());
            }
            for (const uint64_t P : Left) Counter(P >> 63 ? "map_transient_zones_not_indexed" : "model_meshes_not_indexed")++;
            std::string ProfileError;
            const auto* Loaded = ExportWriters::ZoneProfile(Profile(), ProfileError);
            if (Loaded == nullptr || Paths.empty()) return;
            std::map<std::string, size_t> Counts;
            Beside.Load(Paths, *Loaded, Live().Origin() + Image().size(), Counts);
            Beside.Link(Walk(), Live().Origin(), Zone(), Rs, [this](uint64_t Field, uint64_t Value) {
                if (Live().Contains(Field)) Live().Poke(Field - Live().Origin(), Value);
            }, Counts);
            for (const auto& [K, N] : Counts) Counter(K) += N;
            Beside.Serve();
        }

        // The materials and images this zone's models (and the zones loaded beside) name by hash and
        // no loaded zone defines (a weapon part's _tr zone names the materials of a shared zone), found
        // in the install index's parts and loaded beside as the meshes are, a few rounds (a material
        // loaded beside naming an image of yet another zone). Once per zone, before models are read.
        void LoadMaterialsBeside()
        {
            if (MaterialsTried || Game == nullptr || Install.empty()) return;
            MaterialsTried = true;
            const auto& Rs = Rules();
            const auto& R = Walk();
            std::string ProfileError;
            const auto* Loaded = ExportWriters::ZoneProfile(Profile(), ProfileError);
            if (Loaded == nullptr) return;
            const auto Typed = [](const std::string& Type) { return Type == "material" || Type == "image"; };
            const auto PutMain = [this](uint64_t Field, uint64_t Value) {
                if (Live().Contains(Field)) Live().Poke(Field - Live().Origin(), Value);
            };
            std::unique_ptr<DiskGame::ZoneIndex> Index;
            std::set<std::pair<std::string, uint64_t>> Asked;
            std::map<std::string, size_t> Counts;
            for (int Round = 0; Round < 3; Round++)
            {
                std::set<std::pair<std::string, uint64_t>> Wanted;
                const auto Want = [&](const std::string& Type, uint64_t Value) {
                    if (Typed(Type) && !Asked.count({ Type, Value & Rs.AssetHashMask })) Wanted.emplace(Type, Value & Rs.AssetHashMask);
                };
                if (Round == 0)
                    for (size_t K = 0; K < R.BackRefs.size(); K++)
                    {
                        if (K >= R.BackRefIsAsset.size() || !R.BackRefIsAsset[K]) continue;
                        const auto Type = R.RefTypes.find(R.BackRefs[K].first);
                        if (Type == R.RefTypes.end() || Type->second >= R.OwnerNames.size()) continue;
                        if (Off(PointerAt(R.BackRefs[K].first)) < 0) Want(R.OwnerNames[Type->second], R.BackRefs[K].second);
                    }
                else
                {
                    // the images the materials asked for leave unresolved in the zones that gave them
                    // (a trial link, its counts dropped; their zones' other references are not wanted)
                    std::map<std::string, std::vector<std::pair<int64_t, int64_t>>> FromMaterials;
                    for (const auto& Z : Beside.All())
                        for (const auto& I : Z->R.Instances)
                            if (I.Owner < Z->R.OwnerNames.size() && Z->R.OwnerNames[I.Owner] == "material" && (uint64_t)I.Pos + 8 <= Z->Bytes.size() &&
                                Asked.count({ "material", ExportNaming::At<uint64_t>(Z->Bytes, I.Pos) & Rs.AssetHashMask }))
                                FromMaterials[Z->Name].emplace_back(I.Pos, I.End);
                    std::map<std::string, size_t> Trial;
                    Beside.Link(R, Live().Origin(), Zone(), Rs, PutMain, Trial,
                                [&](const std::string& In, uint64_t Field, uint64_t Value, const std::string& Type) {
                                    if (Type != "image") return;
                                    const auto It = FromMaterials.find(In);
                                    if (It == FromMaterials.end()) return;
                                    for (const auto& [From, To] : It->second)
                                        if ((int64_t)Field >= From && (int64_t)Field < To) return Want(Type, Value);
                                });
                }
                if (Wanted.empty()) break;
                Asked.insert(Wanted.begin(), Wanted.end());
                Counts["materials_beside_wanted"] += Wanted.size();
                if (!Index)
                {
                    std::string Error;
                    Park();
                    Index = DiskGame::ZoneIndex::For(*Game, Options, Install, IndexKey(), nullptr, nullptr, Error);
                    Resume();
                    if (!Index)
                    {
                        Counts["materials_beside_no_index"]++;
                        break;
                    }
                }
                // the zones giving the most of them first, none already loaded
                std::set<std::string> Have{ ExportNaming::Lower(Name()) };
                for (const auto& S : Beside.All()) Have.insert(ExportNaming::Lower(S->Name));
                std::map<size_t, std::set<std::pair<std::string, uint64_t>>> Gives;
                for (const auto& W : Wanted)
                    for (const size_t Z : Index->PartIn(W.first, W.second))
                        if (!Have.count(ExportNaming::Lower(fs::path(Index->Zones()[Z]).stem().string()))) Gives[Z].insert(W);
                std::set<std::pair<std::string, uint64_t>> Left = Wanted;
                std::vector<std::string> Paths;
                for (const size_t Z : IwBeside::Cover(std::move(Gives), Left))
                {
                    std::vector<std::string> Found;
                    std::string Why;
                    if (Game->ResolveZones(Install, { Index->Zones()[Z] }, Options, Found, Why) && !Found.empty()) Paths.push_back(Found.front());
                }
                Counts["materials_beside_not_indexed"] += Left.size();
                if (Paths.empty()) break;
                Counts["materials_beside_zones"] += Beside.Load(Paths, *Loaded, Live().Origin() + Image().size(), Counts);
            }
            if (!Beside.Empty() && Counts["materials_beside_zones"] != 0)
            {
                Beside.Link(R, Live().Origin(), Zone(), Rs, PutMain, Counts);
                Beside.Serve();
            }
            for (const auto& [K, N] : Counts)
                if (N != 0) Counter(K) += N;
        }

        // an image instance of a zone loaded beside (LoadMaterialsBeside's)
        bool ImageBeside(uint64_t Address) const override { return Beside.Holds("image", Address); }

        // an image with pixels: in the zone, or levels in the packages
        bool HasPixels(int64_t Pos) const
        {
            return PointerAt(Pos + GameBlackOps6::ImagePixels) != 0 ||
                   (PointerAt(Pos + GameBlackOps6::ImageMips) != 0 && (At<uint8_t>(Image(), Pos + GameBlackOps6::ImageMipCount) & 0xF) != 0);
        }

        // each sndasset, named by its hash (path folders kept when the index knows it; an index name
        // in another of the game's languages when it lacks it: IwLanguageNames), decoded by
        // GameBlackOps6::ReadXSound (Games/IW/IwctxReaders' export, shared with Black Ops 7)
        void ExportSounds(const Context& C, Totals& T)
        {
            static IwLanguageNames ByLanguage({ std::begin(GameBlackOps6::Languages), std::end(GameBlackOps6::Languages) });
            std::vector<IwctxReaders::SoundEntry> Found;
            std::vector<uint64_t> Hashes;
            const int64_t Rate = (Campaign ? GameBlackOps6::Campaign : GameBlackOps6::Multiplayer).SoundRate;
            for (const auto& I : Walk().Instances)
                if (OwnerIs(I, "sndasset"))
                {
                    Hashes.push_back(At<uint64_t>(Image(), I.Pos));
                    Found.push_back({ GameBlackOps6::HashedName("xsound", Hashes.back()), Live().Addr((uint64_t)I.Pos),
                                      At<uint16_t>(Image(), I.Pos + Rate), At<uint32_t>(Image(), I.Pos + GameBlackOps6::SoundFrames) });
                }
            if (const size_t N = ByLanguage.Rename(Hashes, Found)) Counter("sounds_named_by_language") += N;
            IwctxReaders::ExportSounds(C, T, Name(), Found);
        }

        void ExportImages(const Context& C, Totals& T)
        {
            ImageList L;
            for (const auto& I : Walk().Instances)
                if (OwnerIs(I, "image") && HasPixels(I.Pos)) L.Order.push_back(I.Pos);
            L.KeyOf = [this](int64_t Pos, const std::string& Extension) {
                ExportWriters::ContentKey Key;
                return Key.Mix(At<uint64_t>(Image(), Pos)).Mix(Extension).Key();
            };
            L.Decode = [this](int64_t Pos, const std::string& Name) {
                GameBlackOps6::LayoutScope S(Campaign ? GameBlackOps6::Campaign : GameBlackOps6::Multiplayer);
                return GameBlackOps6::LoadXImage(XImage_t(ImageUsageType::DiffuseMap, 0, Live().Addr((uint64_t)Pos), Name));
            };
            L.FailWhy = [this](int64_t Pos) { return std::string(PointerAt(Pos + GameBlackOps6::ImagePixels) != 0 ? "in zone" : "streamed"); };
            L.OnePerName = true;
            for (const int64_t Pos : L.Order)
                if (AssetName(Kind::Image, Pos).rfind("ximage_", 0) != 0) Counter("images_named")++;
            ExportImageList(C, T, L);
        }
    };

    const IwWeaponStats::Layout& BlackOps6Zone::StatsLayout()
    {
        static const IwWeaponStats::Layout L = [] {
            IwWeaponStats::Layout L;
            // weapon (0x488): name 8, definition 0x20 (0x33024d0 reads 0xc7d0a40[index] + 0x20 + 0x5538),
            // the display name's localize hash 0x40 (t10_weapon/<name>); 16 attachment slots at 0x58 +
            // slot * 0x10, count at +8 (0x329d750: slot < 0x10)
            L.WeaponName = 8, L.WeaponDef = 0x20, L.WeaponDisplay = 0x40, L.Slots = 0x58, L.SlotCount = 16;
            // definition (0x5540, the table 0xc7d2980): class u8 0x5511 (weaponclass 0x59f2130 ->
            // 0x329e7e0), base animation package 0x318 (0x329f4a0), the tunables sets 0x5520 / 0x5528 /
            // 0x5530 (0x3266270 picks one into 0x5538), overrides 0x60 / count 0x528c (0x3296240, their
            // conditions 0x32706f0)
            L.DefClass = 0x5511, L.DefAnimPkg = 0x318, L.DefSets = 0x5520, L.DefSetCount = 0x5528, L.DefSetTags = 0x5530, L.DefTunables = 0x5538,
            L.DefOverrides = 0x60, L.DefOverrideCount = 0x528c;
            L.FireTime = 0x64c, L.BurstCount = 0x688, L.StartAmmo = 0x6e8, L.StartAmmoOverride = 0x6ec, L.ClipSize = 0x718, L.MaxAmmo = 0x714,
            L.MaxAmmoByClips = 0x740, L.FireType = 0x648, L.AdsTime = 0x888, L.AdsOutTime = 0x88c, L.ReloadType = 0x9e9, L.ExplosionRadius = 0x548,
            L.ExplosionRadiusAlt = 0x570, L.ExplosionAlt = 0x598, L.Damage = 0x3c, L.Hit = 0x394, L.SprintOutScale = 0x638,
            L.SprintSpeedScale = 0x99c, L.SuperSprintSpeedScale = 0x9b0, L.ProneSpeedScale = 0x990;
            // attachment (0xc50): name 0x238, clip 0xa50 / 0x3b8, modifiers 0x240, ADS override 0x410
            // (0x329c960), explosion 0x530
            L.AttachmentName = 0x238, L.AttClipScale = 0xa50, L.AttClipOverride = 0x3b8, L.AttModifiers = 0x240, L.AttAdsOverride = 0x410,
            L.AttExplosion = 0x530, L.AttExplosionAlt = 0x28;
            // the burst delay f32 0x690 (0x32875e0, Black Ops 7's 0x4637000 instruction for instruction; its
            // caller 0x32c581c adds floor(t + 0.5) to the fire time 0x64c for fire type 2), the explosion's
            // damages 0x554 / 0x558 and three rings 0x568.. (0x32898c0, the same output as Black Ops 7's
            // 0x4639d80), the base and aiming move scales 0x978 / 0x984 (the speed-scale builder 0x30aa9c0
            // reads them into Black Ops 7's 0x5bf0 / 0x5bfc entries)
            L.BurstDelay = 0x690, L.ExplosionDamage = 0x554, L.ExplosionRings = 0x568, L.MoveSpeedScale = 0x978, L.AdsMoveSpeedScale = 0x984;
            // attachment: burst delay i32 * 0x598 (not from an alternate mode's, u8 0xbf5), its scale block
            // 0x240->0xe8 +4 (0x32875e0); explosion block damages +8 / +0xc, rings +0x20.., ring flag +0x50
            // (0x32898c0)
            L.AttBurstDelay = 0x598, L.AttAltMode = 0xbf5, L.AttBurstDelayScale = 0xe8, L.AttExplosionDamage = 8, L.AttExplosionRings = 0x20,
            L.AttExplosionFlag = 0x50;
            // animation package: block 0x18 (0x329f4a0 reads it at a byte offset)
            L.PkgBlock = 0x18, L.PkgFirstRaise = 0x2, L.PkgReloadEmpty = 0x74, L.PkgReload = 0x76, L.PkgSprintOut = 0x2e0, L.PkgSuperSprintOut = 0x2e2,
            L.PkgRaise = 0xda, L.PkgDrop = 0xd8, L.PkgRaiseQuick = 0xf4, L.PkgReloadStart = 0x8c, L.PkgReloadEnd = 0xb4;
            // 0xffff is the package's "no time", as in Black Ops 7: left out (empty), not written as 65535
            L.PkgFfffIsNone = true;
            // suit (0x18d8, 0x3177b00 finds it): its hash at 0, sprint 0xc28 (0x30d2430; 0xbfc under a
            // perk), super sprint 0xc84 (0x30ae060)
            L.SuitId = 0, L.SuitSprint = 0xc28, L.SuitSuperSprint = 0xc84;
            // the gametype settings' tunables tag (0x3267ca0) and suit (0x37ec000) keys: Black Ops 7's
            L.TagKey = 0x65811ecb02190e17ull, L.SuitKey = 0x9465580f7953cea8ull;
            L.TagNames = { "default", "t10_br", "t10_zm", "t10_mp", "t10_ob", "t10_cp" };
            // the weapon stats UI (0x37ece60) starts from 190.0 (0x95b49a8)
            L.BaseSpeed = 190.0f;
            // the suit global_shared_mp's settings bundles name for multiplayer (the others: _wz, _zm)
            L.MpSuit = "t10_defaultsuit_mp";
            L.NameHash = [](const std::string& Name) { return IwctxReaders::AssetHash(Name); };
            L.Title = "Black Ops 6 weapon stats";
            L.Columns =
                "\nA value its zone was not read for stays empty: the guns are in global_stream_mp, their animation\n"
                "packages (reload, raise, sprint-to-fire) in ingame_shared_mp, the suits and game-mode settings in\n"
                "global_shared_mp, the display names in ens_global. Export those zones into the same --out (a .txt\n"
                "list of zone paths works) and the table is rebuilt from every facts file.\n\n"
                "Columns and units (RVAs of bo6_flat.exe; each function matched to Black Ops 7's through the GSC\n"
                "builtin calling it, the same name hash in both, or by its code)\n"
                "- set: the tunables set (weapon_def 0x5520[set-1]); 0x3266270 picks the set whose tags (0x5530) hold\n"
                "  the gametype settings' tag (0x3267ca0) into 0x5538. tags: FNV-1a 64 names.\n"
                "- fire_time_ms: tunables 0x64c (weaponfiretime 0x59f23b0 -> 0x328a150). rpm = 60000 / fire_time_ms.\n"
                "- burst_count: 0x688 (weaponburstcount 0x59f2040 -> 0x329e6c0). fire_type: u8 0x648 (0x329f7b0; an\n"
                "  attachment's +0x2f8 overrides it): 0 auto (weaponisauto), 2 burst, 5 beam (weaponisbeam), 1, 3, 4\n"
                "  semi-auto (weaponissemiauto).\n"
                "- burst_delay_ms: the wait after a burst, floor(f32 0x690 + 0.5): 0x32875e0 (Black Ops 7's 0x4637000\n"
                "  instruction for instruction); 0x32c581c adds it to the fire time 0x64c for fire type 2. Held by every\n"
                "  weapon, used by burst weapons only.\n"
                "- clip_size: 0x718 (weaponclipsize 0x59f22f0 -> 0x33024d0). start_ammo: 0x6ec when not 0, else 0x6e8\n"
                "  (weaponstartammo -> 0x32ba410). max_ammo: 0x714 (weaponmaxammo -> 0x32af480); empty when u8 0x740 is\n"
                "  set (then it counts magazines, not reproduced).\n"
                "- reload_ms, reload_empty_ms, first_raise_ms: the base animation package (weapon_def 0x318), u16 0x76,\n"
                "  0x74 and 0x2 of its time block (getweaponreloadtime 0x58bebc0: 0x32b7170 with ammo left, 0x32b5630\n"
                "  empty; weaponfirstraisetime 0x59f2460 -> 0x327e460), before attachments and perks.\n"
                "  A package time of 0xffff is none, as in Black Ops 7 (its override search skips it): its cell, and the\n"
                "  sprint-to-fire times computed from it, stay empty (the base getter returns it unchanged).\n"
                "- reload_type: u8 0x9e9 (0x3296ca0; an attachment's block 0x370 +4 overrides it): 0 magazine, 1 per round;\n"
                "  getweaponreloadtime also handles 2 and 3. For 1 it adds reload_start_ms (u16 0x8c, 0x32b7070) and\n"
                "  reload_end_ms (u16 0xb4, 0x32b5a60; 0xc0 under a weapon-state flag).\n"
                "- raise_ms (u16 0xda, 0x32b43c0), raise_quick_ms (u16 0xf4 when player flag 0x864 is set, else 0x106,\n"
                "  0x32ae320), drop_ms (u16 0xd8, 0x327d5b0): the raise and drop states' times (0x31d4fa0, 0x31d54f0).\n"
                "- sprint_to_fire_ms = max(1, (int)(u16 0x2e0 x tunables 0x638)) (0x32b9c60, 0x3273a90; 0x2d0 under a\n"
                "  flag), super_sprint_to_fire_ms the same with u16 0x2e2 (0x32bb130).\n"
                "- ads_time_ms: 0x888 (aiming in). ads_out_time_ms: 0x88c (aiming out); 0x3280340 reads 4 times there.\n"
                "- muzzle_velocity_mps: not proven for this game (empty).\n"
                "- explosion_radius_m: getweaponexplosionradius (0x5b66900 -> 0x327dce0): i32 0x548, or 0x570 when u8\n"
                "  0x598 is set; an attachment's block at 0x530 overrides it. Inches / 39.37. Empty when 0.\n"
                "- explosion_inner_damage / explosion_outer_damage: 0x32898c0 fills radii[3] and damages[3] as Black Ops\n"
                "  7's 0x4639d80: one ring radii 0 / 0x548 / 0x548, damages i32 0x554 (centre) / 0x558 / 0x558 (edge); with\n"
                "  u8 0x598 three rings: radii 0x568 / 0x56c / 0x570, damages 0x574 / 0x578 / 0x57c. inner = damages[0],\n"
                "  outer = damages[2]; three-ring weapons also explosion_inner_radius_m, explosion_mid_radius_m,\n"
                "  explosion_mid_damage. Written with the radius.\n"
                "- damage_*: the first record of the table at 0x3c (0x3288900: 2 profiles of 3 records, 0x12c, 0x21c and\n"
                "  0x30c for the other modes): damage_max up to range_max_m, then the first mid whose range is set and\n"
                "  beyond the distance, else damage_min (0x3297920, 0x3295a80). Ranges are inches / 39.37.\n"
                "- hit_*: the multipliers of the table at 0x394 (0x3290910; 0x404 and 0x474 for the other modes), by the\n"
                "  game's hit location order (its table 0x9a71520).\n"
                "- sprint_speed_scale (0x99c), super_sprint_speed_scale (0x9b0), prone_speed_scale (0x990): scales\n"
                "  (0x30aa9c0, the speed-scale builder).\n"
                "- move_speed_scale (0x978), ads_move_speed_scale (0x984): the builder's standing entries of the two\n"
                "  families, where Black Ops 7's reads 0x5bf0 / 0x5bfc (the second used while aiming).\n"
                "- sprint_mps = 190 x suit 0xc28 x sprint_speed_scale / 39.37, super_sprint_mps also x suit 0xc84: the\n"
                "  weapon stats UI's formula (0x37ece60). suit: the suit of the gametype settings whose tag the set\n"
                "  holds (the settings bundles of global_shared_mp name no tag: empty). *_mp_suit: the same with\n"
                "  t10_defaultsuit_mp, the multiplayer one of the suits those bundles name (key 0x37ec000 reads).\n"
                "- reload_add_ms, reload_empty_add_ms: not proven for this game (empty).\n";
            L.AttachmentNotes =
                "\nweapon_attachments.csv: each weapon's default set with one attachment (every slot's list, weapon\n"
                "+0x58 + slot * 0x10, slot and index as 0x329d750 and 0x32706f0 read them). weapon_loadouts.csv: the\n"
                "lines of loadouts.txt in this folder ('<weapon asset>: <attachment>, <attachment>...'), each loadout of\n"
                "the default set. How the attachments combine (the code):\n"
                "- clip_size: the i32 at 8 of an attachment's block 0x3b8 when >= 0, else (int)(clip x (1 + sum(attachment\n"
                "  0xa50 - 1))) (0x33024d0).\n"
                "- reload_ms, reload_empty_ms, the sprint-out times: the animation package of the override entry (weapon_def\n"
                "  0x60) with the most condition groups whose groups all match, else the base package (0x3296240).\n"
                "- sprint_to_fire_ms = max(1, (int)(time x 0x638 x (1 + sum(attachment 0x240->0x08 +0x18 - 1)) +\n"
                "  sum((int)(1000 x attachment 0x240->0x68 +4)))) (0x3273a90).\n"
                "- ads_time_ms = max(1, ads_time / (1 + sum(0x240->0x18 +4 - 1)) + (int)(1000 x sum(0x240->0x90 +4)))\n"
                "  (0x3280340, 0x3282680); ads_time is replaced by (int)(1000 x t) of the first attachment with ADS times\n"
                "  at 0x410 (0x329c960). ads_out_time_ms: the same with ads_out_time.\n"
                "- burst_delay_ms = floor(t x max(0, 1 + sum(attachment 0x240->0xe8 +4 - 1)) + 0.5) (0x32875e0): t is the\n"
                "  i32 the first attachment's pointer 0x598 leads to (that attachment's own scale then left out), else\n"
                "  tunables 0x690; an alternate mode's attachment (u8 0xbf5 = 1) overrides only while active: not here.\n"
                "- explosion_inner_damage / explosion_outer_damage: the first attachment with an explosion block (0x530)\n"
                "  gives the radius, the damages (+8 / +0xc; three rings +0x20 radii, +0x2c / +0x34 damages) and the ring\n"
                "  flag (u8 +0x50) (0x32898c0).\n"
                "- Attachment display names are not read for this game.\n";
            return L;
        }();
        return L;
    }

    // ---- the level (C2M's BlackOps6.cs, checked against the gfx_map loader's tables 0x2839910)
    // gfx_map (0x10): the world at 8. GfxWorld (0xae48): name 8, surfaces inline 0xd8, static models inline 0x260.
    constexpr int64_t WorldAt = 8, WorldName = 8, Surfs = 0xd8, SModels = 0x260;
    // GfxWorldSurfaces (0x188, 0x283bbc0): counts u32 0 (surfaces), 0xc (materials), 0x14 (brush
    // models); brush models (60: first surface u32 48, end u32 52) 0x58, surfaces (40) 0x78,
    // materials 0xa0, the surfaces' vertex records (128) 0xc0
    constexpr int64_t SurfCount = 0, BrushCount = 0x14, Brushes = 0x58, BrushSize = 60, BrushFirst = 48, BrushEnd = 52;
    constexpr int64_t SurfArray = 0x78, SurfSize = 40, Materials = 0xa0, MaterialCount = 0xc, Records = 0xc0, RecordSize = 128;
    // A surface (40): first index u32 4, first table u32 8, record u32 12, material u32 16, faces u16 20,
    // tables u16 22, vertices u16 24, packed indices u32 36
    constexpr int64_t SfFirstIndex = 4, SfTable = 8, SfRecord = 12, SfMaterial = 16, SfFaces = 20, SfTables = 22, SfVertices = 24, SfPacked = 36;
    // A record (128): offset 3 floats and scale 0, transient zone u32 16, uv layers u32 28; byte offsets
    // into the zone's position data: positions (21-bit x3) 40, tangent frames 44, colours 52 (0: none), uvs 56
    constexpr int64_t RcZone = 16, RcLayers = 28, RcPositions = 40, RcNormals = 44, RcColors = 52, RcUvs = 56;
    // gfx_map_trzone (0x1a0, 0x283c370): zone number i32 0x14, draw data inline 0x18 (position data
    // size u32 0, the data 0x18, u16 indices 0x20, 32-byte tables 0x28, packed indices 0x30)
    constexpr int64_t TrIndex = 0x14, TrDraw = 0x18, DrPosSize = 0, DrPos = 0x18, DrIndices = 0x20, DrTables = 0x28, DrPacked = 0x30;
    // GfxWorldStaticModels (0x5518, 0x283ad60): counts u32 4 (models), 8 (collections), 0xc
    // (placements); models (16, the xmodel at 0) 0x78, collections (20: first placement i32 0, count
    // u16 4, model u16 8) 0x88, placements (24: origin 3 x i32 / 4096, quaternion 4 x u16 over [-1, 1],
    // half scale 20) 0xd8
    constexpr int64_t SmModelCount = 4, SmCollectionCount = 8, SmInstanceCount = 0xc, SmModels = 0x78, SmCollections = 0x88, SmPlacements = 0xd8;

    // The two layouts: the multiplayer's above, the campaign's (sp24, its gfx_map 0x9aa0 the GfxWorld
    // itself: name 8, surfaces inline 0xc8, static models inline 0x250; brush models 56 bytes (first
    // and end surface u32 48, 52); surfaces 36 bytes: the vertices' byte offset u32 0 (no record
    // index: the record whose positions start there and whose vertex count matches), first index u32
    // 4, first table u32 8, material u32 16, faces u16 20, tables u16 22, vertices u16 24, packed
    // indices u32 32; face tables 28 bytes; static models: models 0x68, collections (16) 0x78,
    // placements 0xc0. Read off sp_t10_intro: the offsets index that zone's buffers)
    struct MapLayout
    {
        int64_t WorldAt, WorldName, Surfs, SModels, SurfCount, BrushCount, Brushes, BrushSize, BrushFirst, BrushEnd, SurfArray, SurfSize, Materials, MaterialCount, Records, RecordSize, SfFirstIndex, SfTable, SfRecord, SfMaterial, SfFaces, SfTables, SfVertices, SfPacked, RcZone, RcLayers, RcPositions, RcNormals, RcColors, RcUvs, TrIndex, TrDraw, DrPosSize, DrPos, DrIndices, DrTables, DrPacked, SmModelCount, SmCollectionCount, SmInstanceCount, SmModels, SmCollections, SmPlacements, FaceTable, CollectionSize, ColFirst, ColCount, ColModel;
    };
    constexpr MapLayout MpMap{ WorldAt, WorldName, Surfs, SModels, SurfCount, BrushCount, Brushes, BrushSize, BrushFirst, BrushEnd, SurfArray, SurfSize, Materials, MaterialCount, Records, RecordSize, SfFirstIndex, SfTable, SfRecord, SfMaterial, SfFaces, SfTables, SfVertices, SfPacked, RcZone, RcLayers, RcPositions, RcNormals, RcColors, RcUvs, TrIndex, TrDraw, DrPosSize, DrPos, DrIndices, DrTables, DrPacked, SmModelCount, SmCollectionCount, SmInstanceCount, SmModels, SmCollections, SmPlacements, 32, 20, 0, 4, 8 };
    constexpr MapLayout SpMap{ -1, 8, 0xc8, 0x250, 0, 0x14, 0x58, 56, 48, 52, 0x78, 36, 0xa0, 0xc, 0xc0, 128, 4, 8, -1, 16, 20, 22, 24, 32, 16, 28, 40, 44, 52, 56, 20, 24, 0, 24, 32, 40, 48, 4, 8, 12, 104, 120, 192, 28, 16, 0, 4, 8 };

    // ---- the entity string and the lights
    std::set<int32_t> BlackOps6Zone::TransientNumbers() const
    {
        std::set<int32_t> Out;
        const MapLayout& L = Campaign ? SpMap : MpMap;
        const int64_t Map = Root("gfx_map");
        const int64_t World = Map < 0 ? -1 : L.WorldAt < 0 ? Map : Off(PointerAt(Map + L.WorldAt));
        if (World < 0) return Out;
        const auto& Img = Image();
        const int64_t S = World + L.Surfs, Recs = Off(PointerAt(S + L.Records));
        const uint32_t Count = At<uint32_t>(Img, S + 8);
        for (uint32_t K = 0; Recs >= 0 && K < Count && Recs + (int64_t)(K + 1) * L.RecordSize <= (int64_t)Img.size(); K++)
            Out.insert(At<int32_t>(Img, Recs + (int64_t)K * L.RecordSize + L.RcZone));
        return Out;
    }

    void BlackOps6Zone::EntityStrings(std::vector<IwctxReaders::MapEntity>& Out) const
    {
        const auto OffOf = [this](uint64_t A) { return Off(A); };
        const auto PtrAt = [this](int64_t P) { return PointerAt(P); };
        const auto KeyScheme = Campaign ? IwEntityKeys::Scheme::SaltAfter : IwEntityKeys::Scheme::SaltFirst;
        if (const int64_t Go = Root("map_game_objects"); Go >= 0)
            if (const int64_t P = Off(PointerAt(Go + 8)); P >= 0) IwctxReaders::EntityPairs(Image(), P, OffOf, PtrAt, Out, KeyScheme);
        for (const auto& I : Walk().Instances)
            if (OwnerIs(I, "scriptbrushmodel")) IwctxReaders::EntityPairs(Image(), I.Pos + 0x10, OffOf, PtrAt, Out, KeyScheme);
    }

    bool BlackOps6Zone::ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models)
    {
        LoadBeside();
        const MapLayout& L = Campaign ? SpMap : MpMap;
        GameBlackOps6::LayoutScope Readers(Campaign ? GameBlackOps6::Campaign : GameBlackOps6::Multiplayer);
        const int64_t Map = Root("gfx_map");
        const int64_t World = Map < 0 ? -1 : L.WorldAt < 0 ? Map : Off(PointerAt(Map + L.WorldAt));
        if (World < 0) return false;
        const auto& Img = Image();
        const int64_t Size = (int64_t)Img.size();
        Scene.Name = StringAt(PointerAt(World + L.WorldName));
        if (Scene.Name.empty()) Scene.Name = Name();

        // the transient zones by number: the map zone's own, and those of the zones loaded beside
        // (a big map's _tr zones), each in its own bytes (pointers there are served addresses)
        struct Transient
        {
            const std::vector<uint8_t>* Bytes;
            int64_t Pos;
            uint64_t Origin;
        };
        std::unordered_map<int32_t, Transient> Zones;
        for (const auto& I : Walk().Instances)
            if (OwnerIs(I, "gfx_map_trzone")) Zones.emplace(At<int32_t>(Img, I.Pos + L.TrIndex), Transient{ &Img, I.Pos, Live().Origin() });
        for (const auto& Z : Beside.All())
            for (const auto& I : Z->R.Instances)
                if (I.Owner < Z->R.OwnerNames.size() && Z->R.OwnerNames[I.Owner] == "gfx_map_trzone" &&
                    (uint64_t)I.Pos + L.TrIndex + 4 <= Z->Bytes.size() &&
                    ExportNaming::At<uint32_t>(Z->Bytes, I.Pos + 0x10) == At<uint32_t>(Img, Root("gfx_map_trzone") + 0x10))
                    Zones.emplace(ExportNaming::At<int32_t>(Z->Bytes, I.Pos + L.TrIndex), Transient{ &Z->Bytes, I.Pos, Z->Base });

        const int64_t S = World + L.Surfs;
        const uint32_t SurfaceCount = At<uint32_t>(Img, S + L.SurfCount), BrushTotal = At<uint32_t>(Img, S + L.BrushCount);
        const int64_t Surfaces = Off(PointerAt(S + L.SurfArray)), BrushArray = Off(PointerAt(S + L.Brushes)), Recs = Off(PointerAt(S + L.Records));
        const int64_t Mats = Off(PointerAt(S + L.Materials));
        const uint32_t MatCount = At<uint32_t>(Img, S + L.MaterialCount);
        std::unordered_map<uint32_t, std::string> MaterialNames;
        const auto MaterialOf = [&](uint32_t Index) -> std::string {
            auto It = MaterialNames.find(Index);
            if (It != MaterialNames.end()) return It->second;
            std::string MatName;
            const uint64_t Address = Mats >= 0 && Index < MatCount ? PointerAt(Mats + (int64_t)Index * 8) : 0;
            if (Off(Address) >= 0) MatName = Maps::AddMaterial(Scene, GameBlackOps6::ReadXMaterial(Address));
            MaterialNames.emplace(Index, MatName);
            return MatName;
        };
        // the campaign's surfaces name no record: the one by (positions offset, vertex count)
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> RecordOf;
        const uint32_t RecordTotal = At<uint32_t>(Img, S + 8);
        if (L.SfRecord < 0)
            for (uint32_t K = 0; Recs >= 0 && K < RecordTotal && Recs + (int64_t)(K + 1) * L.RecordSize <= Size; K++)
            {
                const int64_t R = Recs + (int64_t)K * L.RecordSize;
                RecordOf.emplace(std::make_pair(At<uint32_t>(Img, R + L.RcPositions), At<uint32_t>(Img, R + 32)), K);
            }
        for (uint32_t B = 0; Surfaces >= 0 && BrushArray >= 0 && Recs >= 0 && B < BrushTotal; B++)
        {
            const int64_t Brush = BrushArray + (int64_t)B * L.BrushSize;
            const uint32_t First = At<uint32_t>(Img, Brush + L.BrushFirst), End = std::min(At<uint32_t>(Img, Brush + L.BrushEnd), SurfaceCount);
            Maps::Mesh Mesh{ B == 0 ? std::string("world") : "brushmodel_" + std::to_string(B), {} };
            std::unordered_map<std::string, size_t> Group;
            for (uint32_t K = First; K < End; K++)
            {
                const int64_t Sf = Surfaces + (int64_t)K * L.SurfSize;
                uint32_t RecIndex = 0;
                if (L.SfRecord >= 0)
                    RecIndex = At<uint32_t>(Img, Sf + L.SfRecord);
                else if (const auto It = RecordOf.find({ At<uint32_t>(Img, Sf), At<uint16_t>(Img, Sf + L.SfVertices) }); It != RecordOf.end())
                    RecIndex = It->second;
                else
                {
                    Counter("map_surfaces_without_record")++;
                    continue;
                }
                const int64_t Rec = Recs + (int64_t)RecIndex * L.RecordSize;
                const auto Zone = Rec + L.RecordSize <= Size ? Zones.find((int32_t)At<uint32_t>(Img, Rec + L.RcZone)) : Zones.end();
                if (Zone == Zones.end())
                {
                    Counter("map_surfaces_in_other_zones")++;
                    continue;
                }
                // the draw data: in the transient zone's own bytes
                const std::vector<uint8_t>& Tz = *Zone->second.Bytes;
                const auto In = [&](int64_t Field) {
                    const uint64_t A = ExportNaming::At<uint64_t>(Tz, Field);
                    return A >= Zone->second.Origin && A - Zone->second.Origin < Tz.size() ? (int64_t)(A - Zone->second.Origin) : (int64_t)-1;
                };
                const int64_t Draw = Zone->second.Pos + L.TrDraw;
                const int64_t Pos = In(Draw + L.DrPos), Idx = In(Draw + L.DrIndices);
                const int64_t Tab = In(Draw + L.DrTables), Pk = In(Draw + L.DrPacked);
                const int64_t PosSize = ExportNaming::At<uint32_t>(Tz, Draw + L.DrPosSize);
                const uint16_t Vertices = At<uint16_t>(Img, Sf + L.SfVertices);
                const uint32_t Layers = std::max(At<uint32_t>(Img, Rec + L.RcLayers), 1u);
                const int64_t P = At<uint32_t>(Img, Rec + L.RcPositions), N = At<uint32_t>(Img, Rec + L.RcNormals);
                const int64_t Cl = At<uint32_t>(Img, Rec + L.RcColors), Uv = At<uint32_t>(Img, Rec + L.RcUvs);
                const bool Fits = Pos >= 0 && Idx >= 0 && Tab >= 0 && Pk >= 0 && P + (int64_t)Vertices * 8 <= PosSize &&
                                  N + (int64_t)Vertices * 4 <= PosSize && Uv + (int64_t)Vertices * 8 * Layers <= PosSize &&
                                  (Cl == 0 || Cl + (int64_t)Vertices * 4 <= PosSize);
                if (!Fits)
                {
                    Counter("map_surfaces_unread")++;
                    continue;
                }
                const std::string Material = MaterialOf(At<uint32_t>(Img, Sf + L.SfMaterial));
                auto [G, New] = Group.emplace(Material, Mesh.Surfaces.size());
                if (New) Mesh.Surfaces.push_back({ Material, {}, {} });
                Maps::Surface& Out = Mesh.Surfaces[G->second];
                const uint32_t Base = (uint32_t)Out.Vertices.size();
                const float Ox = At<float>(Img, Rec), Oy = At<float>(Img, Rec + 4), Oz = At<float>(Img, Rec + 8), Scale = At<float>(Img, Rec + 12);
                for (uint32_t V = 0; V < Vertices; V++)
                {
                    Maps::Vertex Vx;
                    const uint64_t Packed = ExportNaming::At<uint64_t>(Tz, Pos + P + (int64_t)V * 8);
                    Vx.Pos[0] = (float)(Packed & 0x1FFFFF) * Scale + Ox;
                    Vx.Pos[1] = (float)((Packed >> 21) & 0x1FFFFF) * Scale + Oy;
                    Vx.Pos[2] = (float)((Packed >> 42) & 0x1FFFFF) * Scale + Oz;
                    const Vector3 Normal = MeshDecode::UnpackPackedQuat(ExportNaming::At<uint32_t>(Tz, Pos + N + (int64_t)V * 4));
                    Vx.Normal[0] = Normal.X, Vx.Normal[1] = Normal.Y, Vx.Normal[2] = Normal.Z;
                    for (int J = 0; J < 2; J++) Vx.Uv[J] = ExportNaming::At<float>(Tz, Pos + Uv + (int64_t)V * 8 * Layers + J * 4);
                    if (Cl != 0) std::memcpy(Vx.Color, Tz.data() + Pos + Cl + (int64_t)V * 4, 4);
                    Out.Vertices.push_back(Vx);
                }
                const uint16_t Faces = At<uint16_t>(Img, Sf + L.SfFaces), Tables = At<uint16_t>(Img, Sf + L.SfTables);
                const uint64_t Indices = (uint64_t)Idx + (uint64_t)At<uint32_t>(Img, Sf + L.SfFirstIndex) * 2;
                const uint64_t TableAt = (uint64_t)Tab + (uint64_t)At<uint32_t>(Img, Sf + L.SfTable) * L.FaceTable;
                const uint64_t PackedAt = (uint64_t)Pk + At<uint32_t>(Img, Sf + L.SfPacked);
                for (uint32_t F = 0; F < Faces; F++)
                {
                    uint32_t X[3]{};
                    if (!GameBlackOps6::UnpackFace(Tz.data(), Tz.size(), Tables, Indices, TableAt, PackedAt, F, X) ||
                        X[0] >= Vertices || X[1] >= Vertices || X[2] >= Vertices)
                    {
                        Counter("map_triangles_unread")++;
                        continue;
                    }
                    for (const uint32_t Corner : X) Out.Indices.push_back(Base + Corner);
                }
            }
            std::erase_if(Mesh.Surfaces, [](const Maps::Surface& One) { return One.Indices.empty(); });
            if (!Mesh.Surfaces.empty()) Scene.Objects.push_back(std::move(Mesh));
        }

        // ---- the static models: collections of placements of one model each
        const int64_t Sm = World + L.SModels;
        const uint32_t ModelCount = At<uint32_t>(Img, Sm + L.SmModelCount), CollectionCount = At<uint32_t>(Img, Sm + L.SmCollectionCount);
        const uint32_t InstanceCount = At<uint32_t>(Img, Sm + L.SmInstanceCount);
        const int64_t ModelArray = Off(PointerAt(Sm + L.SmModels)), Collections = Off(PointerAt(Sm + L.SmCollections));
        const int64_t Placements = Off(PointerAt(Sm + L.SmPlacements));
        if (FILE* Dbg = Maps::DebugFile())
            std::fprintf(Dbg, "[map] %s surfaces %u brush models %u; static models %u collections %u placements %u\n", Scene.Name.c_str(),
                         SurfaceCount, BrushTotal, ModelCount, CollectionCount, InstanceCount);
        for (uint32_t K = 0; ModelArray >= 0 && Collections >= 0 && Placements >= 0 && K < CollectionCount; K++)
        {
            const int64_t Col = Collections + (int64_t)K * L.CollectionSize;
            const int32_t FirstInstance = At<int32_t>(Img, Col + L.ColFirst);
            const uint16_t Count = At<uint16_t>(Img, Col + L.ColCount), Model = At<uint16_t>(Img, Col + L.ColModel);
            if (Model >= ModelCount || FirstInstance < 0) continue;
            const uint64_t Address = PointerAt(ModelArray + (int64_t)Model * 16);
            const std::string ModelName = ModelAt(Address);
            if (ModelName.empty())
            {
                Counter("map_smodels_not_in_zone") += Count;
                continue;
            }
            Models.emplace(ModelName, Address);
            for (uint32_t J = (uint32_t)FirstInstance; J < (uint32_t)FirstInstance + Count && J < InstanceCount; J++)
            {
                const int64_t Pl = Placements + (int64_t)J * 24;
                Maps::Instance In = Maps::PackedPlacement(Img, Pl, true);
                In.Name = "smodel_" + std::to_string(J);
                In.Model = ModelName;
                Scene.StaticInstances.push_back(In);
            }
        }
        Counter("map_smodels") += Scene.StaticInstances.size();

        // ---- the terrain: vhmdata's heightmap, drawn where stterrain says (Games/IW/IwTerrain.h)
        if (const int64_t Vhm = Root("vhmdata"); Vhm >= 0)
        {
            const int64_t St = Root("stterrain");
            const auto Bytes = [this](uint64_t A, uint64_t N) -> const uint8_t* {
                if (Live().Contains(A) && A - Live().Origin() + N <= Image().size()) return Image().data() + (A - Live().Origin());
                return Beside.Locate(A, N);
            };
            IwTerrain::Source Src;
            Src.Locate = Bytes;
            Src.ImageName = [Bytes](uint64_t Field) {
                if (Field == 0) return std::string();
                uint64_t Hash = Field;
                if (const uint8_t* P = Bytes(Field, 8)) std::memcpy(&Hash, P, 8);
                return GameBlackOps6::HashedName("ximage", Hash);
            };
            // the campaign's tiles are images: their R16 samples, from the zone or the packages
            Src.ImageTile = [Bytes](uint64_t Entry, IwTerrain::Tile& Out) {
                if (Bytes(Entry, 0x40) == nullptr) return false;
                std::unique_ptr<XImageDDS> Dds;
                try
                {
                    Dds = GameBlackOps6::LoadXImage(XImage_t(ImageUsageType::Unknown, 0, Entry, std::string()));
                }
                catch (...)
                {
                    return false;
                }
                if (!Dds || Dds->DataBuffer == nullptr || Dds->DataSize < 148) return false;
                const auto* B = reinterpret_cast<const uint8_t*>(Dds->DataBuffer);
                std::memcpy(&Out.H, B + 12, 4);
                std::memcpy(&Out.W, B + 16, 4);
                const size_t Header = std::memcmp(B + 84, "DX10", 4) == 0 ? 148 : 128;
                if (Dds->DataSize < Header + (size_t)Out.W * Out.H * 2) return false;
                Out.Samples.resize((size_t)Out.W * Out.H);
                std::memcpy(Out.Samples.data(), B + Header, Out.Samples.size() * 2);
                return true;
            };
            if (const int64_t Vtm = Campaign ? -1 : Root("vtmdata"); Vtm >= 0) Src.Vtm = Live().Addr((uint64_t)Vtm);
            Src.LayerMaterial = [Bytes, &Scene](uint64_t Field) {
                return Bytes(Field, 0x78) != nullptr ? Maps::AddMaterial(Scene, GameBlackOps6::ReadXMaterial(Field)) : std::string();
            };
            std::map<std::string, size_t> Read;
            IwTerrain::Read(Scene, Src, Live().Addr((uint64_t)Vhm), St >= 0 ? Live().Addr((uint64_t)St) : 0,
                            Campaign ? IwTerrain::BlackOps6Campaign : IwTerrain::BlackOps6, IwTerrain::Options(), Read);
            for (const auto& [K, N] : Read) Counter("map_" + K) += N;
        }

        // ---- the entities (EntityStrings)
        {
            std::vector<IwctxReaders::MapEntity> Ents;
            EntityStrings(Ents);
            Scene.MapEnts += IwctxReaders::EntityText(Ents);
            Counter("map_ents") += Ents.size();
        }

        // ---- the lights: GfxWorld's 0xf0-byte records (count u32 0x5778, array 0x5780, read by
        // 0x2835f80 as com_map's single one is): type u8 0 (1 sun, 2 spot, else omni), colour 52,
        // direction 64, origin 88, radius 100, cosHalfFovOuter 136 (C2M's BO6ComPrimaryLight)
            {
                const uint32_t Count = At<uint32_t>(Img, World + 0x5778);
                const int64_t Lights = Off(PointerAt(World + 0x5780));
                for (uint32_t K = 0; Lights >= 0 && K < Count && Lights + (int64_t)(K + 1) * 0xf0 <= Size; K++)
                {
                    const int64_t L = Lights + (int64_t)K * 0xf0;
                    const uint8_t Type = Img[(size_t)L];
                    if (Type == 0) continue;
                    Maps::Light Out;
                    Out.Kind = Type == 1 ? Maps::Light::Type::Sun : Type == 2 ? Maps::Light::Type::Spot : Maps::Light::Type::Point;
                    for (int J = 0; J < 3; J++)
                    {
                        Out.Color[J] = At<float>(Img, L + 52 + J * 4);
                        Out.Dir[J] = At<float>(Img, L + 64 + J * 4);
                        Out.Origin[J] = At<float>(Img, L + 88 + J * 4);
                    }
                    Out.Radius = At<float>(Img, L + 100);
                    Out.CosHalfFovOuter = At<float>(Img, L + 136);
                    Scene.Lights.push_back(Out);
                    Counter(std::format("map_lights_type{}", Type))++;
                }
            }
        // ---- the campaign's lights: com_map is the ComWorld itself, its 0xd8-byte records (count u32
        // 0x5c, array 0x60, 0x2598850 in both): colour 3 floats 32, direction 44, origin 68, radius 80;
        // the multiplayer's 0xf0 record 20 bytes shorter at its start: type byte 141 (2 spot, its
        // cosHalfFovOuter float 116, as the multiplayer's 136; anything else omni). The GfxWorld's
        // own array (0x5338) is empty.
        if (Campaign)
            if (const int64_t Com = Root("com_map"); Com >= 0)
            {
                const uint32_t Count = At<uint32_t>(Img, Com + 0x5c);
                const int64_t Lights = Off(PointerAt(Com + 0x60));
                for (uint32_t K = 0; Lights >= 0 && K < Count && Lights + (int64_t)(K + 1) * 0xd8 <= Size; K++)
                {
                    const int64_t Rec = Lights + (int64_t)K * 0xd8;
                    Maps::Light Out;
                    for (int J = 0; J < 3; J++)
                    {
                        Out.Color[J] = At<float>(Img, Rec + 32 + J * 4);
                        Out.Dir[J] = At<float>(Img, Rec + 44 + J * 4);
                        Out.Origin[J] = At<float>(Img, Rec + 68 + J * 4);
                    }
                    if (Out.Color[0] == 0 && Out.Color[1] == 0 && Out.Color[2] == 0) continue;
                    const uint8_t Kind = Img[(size_t)(Rec + 141)];
                    Out.Kind = Kind == 2 ? Maps::Light::Type::Spot : Maps::Light::Type::Point;
                    Out.Radius = At<float>(Img, Rec + 80);
                    if (Kind == 2) Out.CosHalfFovOuter = At<float>(Img, Rec + 116);
                    Scene.Lights.push_back(Out);
                    Counter(std::format("map_lights_type{}", Kind))++;
                }
            }
        return true;
    }

    // The install root (the folder holding cod24\ and the shared .xsub) of a fast file in it
    std::string InstallOf(const std::string& FastFile)
    {
        const fs::path Dir = fs::path(FastFile).parent_path();
        const std::string Leaf = ExportNaming::Lower(Dir.filename().string());
        return Leaf == "cod24" || Leaf == "sp24" ? Dir.parent_path().string() : std::string();
    }

    // "bo6" the multiplayer and zombies zones (cod24\*.ff), "bo6sp" the campaign's (sp24\*.ff)
    class BlackOps6Game final : public DiskGame::IDiskGame
    {
    public:
        explicit BlackOps6Game(bool Campaign) : Campaign(Campaign) {}
        const char* Id() const override { return Campaign ? "bo6sp" : "bo6"; }
        const char* Title() const override { return Campaign ? "Call of Duty: Black Ops 6 (campaign)" : "Call of Duty: Black Ops 6"; }
        const char* Engine() const override { return Campaign ? "BO6SP" : "BO6"; }
        std::unique_ptr<DiskGame::IDiskZone> Unopened() const override { return std::make_unique<BlackOps6Zone>(std::string(), Campaign); }

        bool ListZones(const std::string& Install, const ExportOptions& Options, std::vector<std::string>& Names,
                       std::string& Error) const override
        {
            const char* Folder = Campaign ? "sp24" : "cod24";
            std::error_code Ec;
            for (const auto& E : fs::directory_iterator(fs::path(Install) / Folder, Ec))
                if (ExportNaming::Lower(E.path().extension().string()) == ".ff") Names.push_back((fs::path(Folder) / E.path().filename()).string());
            std::sort(Names.begin(), Names.end());
            if (Names.empty()) Error = std::string("no ") + Folder + " fast file in " + Install;
            return !Names.empty();
        }

        // a zone's dependencies come from the install its cod24 or sp24 folder is in
        std::string InstallOfZone(const std::string& Zone) const override
        {
            const std::string Root = InstallOf(Zone);
            return Root.empty() ? IDiskGame::InstallOfZone(Zone) : Root;
        }

        // (the index opens every zone, the multiplayer's 9 649 transient (_tr) ones included: a
        // map's transient zones and some of its models' meshes are there; 0.8 GB of fast files,
        // read by the index's workers)

        // its zones open on the index's workers: their decompression and walk share nothing, its
        // handlers and fixups run under the session lock, ResolveZones is serialized
        bool IndexInParallel() const override { return true; }

        std::unique_ptr<DiskGame::IDiskZone> Open(const std::string& Path, const ExportOptions& Options, std::string& Error) const override
        {
            // the packages the readers stream mesh and image data from: --install, else the install
            // the fast file sits in
            const std::string Install = !Options.Install.empty() ? Options.Install : InstallOf(Path);
            if (!Install.empty() && TreyarchPackages::Open<XSUBCache>(Install) == nullptr)
            {
                Error = "could not open the packages of " + Install;
                return nullptr;
            }
            auto Zone = std::make_unique<BlackOps6Zone>(Path, Campaign);
            Zone->SetInstall(this, Options, Install);
            if (!Zone->Open(Error)) return nullptr;
            return Zone;
        }

    private:
        bool Campaign;
    };

    const BlackOps6Game Game(false), CampaignGame(true);
    const DiskGame::Registration Registered(Game), RegisteredCampaign(CampaignGame);
}
