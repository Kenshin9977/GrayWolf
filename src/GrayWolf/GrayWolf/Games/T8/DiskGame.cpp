#include "stdafx.h"

// Black Ops 4 (T8) on the disk template: a ZonekitZone with the bo4 profile, served to BO4's live
// readers (its string table decrypted, images read the disk way). What is BO4's own: zones from
// CASC with their .fd patches, the 1080/4k UI sets, names from the hash index, images through
// Bo4DiskExport (in-zone or CASC-streamed), and sounds from the zone's own .sabl banks.

#include "Bo4DiskExport.h"
#include "DBGameAssets.h"
#include "DiskGame.h"
#include "FileSystems.h"
#include "GameBlackOps4.h"
#include "Games/Treyarch/TreyarchLevelNames.h"
#include "Games/Treyarch/TreyarchMaps.h"
#include "Games/Treyarch/TreyarchPackages.h"
#include "Games/Treyarch/TreyarchScriptBundles.h"
#include "Games/Treyarch/TreyarchWeaponStats.h"
#include "XPAKCache.h"
#include "MapLive.h"
#include "WraithX.h"
#include "ZonekitZone.h"
#include "WraithNameIndex.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <format>
#include <set>

namespace fs = std::filesystem;

namespace
{
    constexpr uint64_t HashMask = 0x0FFFFFFFFFFFFFFFull;   // the 60-bit asset name hash

    std::string KnownName(uint64_t Hash);
    std::string NameOf(uint64_t Hash, bool Full);
    std::string FullName(uint64_t Hash) { return NameOf(Hash, true); }

    using DiskGame::ExportOptions;

    // An asset's full name (its folders and extension kept) from the name index, else <prefix>_<hash>
    std::string HashedName(uint64_t Hash, const char* Prefix)
    {
        const auto& Names = GameBlackOps4::AssetNameCache.NameDatabase;
        if (const auto It = Names.find(Hash); It != Names.end()) return It->second;
        return std::format("{}_{:x}", Prefix, (unsigned long long)Hash);
    }

    // The 1080_ and 4k_ zones hold the same UI images at two sizes (Bo4DiskExport::KeepZoneForUiRes).
    const DiskGame::Setting UiRes{ "ui_res", "UI textures", { { "4k", "4K" }, { "1080", "1080p" }, { "all", "both" } },
                                   "4k", { DiskGame::Kind::Image } };

    CoDPackageCache* Packages(const ExportOptions& Options)
    {
        return Options.Install.empty() ? nullptr : TreyarchPackages::Open<XPAKCache>(Options.Install);
    }

    std::string CacheDir(const ExportOptions& Options, const char* Default)
    {
        return !Options.Cache.empty() ? Options.Cache : (fs::temp_directory_path() / Default).string();
    }

    class Bo4Zone final : public ZonekitZone
    {
    public:
        Bo4Zone(const std::string& Path, CoDPackageCache* P, std::vector<std::shared_ptr<const Bo4DiskExport::MaterialBook>> Common)
            : ZonekitZone(Path), Pkg(P), Common(std::move(Common))
        {
            using namespace DiskGame;
            Exports(Kind::Image, [this](const Context& C, Totals& T) { ExportImages(C, T); });
            Exports(Kind::Sound, [this](const Context& C, Totals& T) { ExportSounds(C, T); });
            Exports(Kind::Map, [this](const Context& C, Totals& T) { ExportMap(C, T); });
            // scriptparsetree {XHash name, buffer 0x10, i32 len 0x18}: a GSCOBJ of the T8 VM, decompiled by acts
            Exports(Kind::Script, [this](const Context& C, Totals& T) {
                ExportScripts(C, T, Scripts::Acts, { { "scriptparsetree", [this](int64_t Pos, Scripts::Script& S) {
                                  S.Compiled = true;
                                  return BytesAt(PointerAt(Pos + 0x10), At<uint32_t>(Image(), Pos + 0x18), S.Bytes);
                              } } });
            });
            ExportsGdt();
            ExportsAssetRefs();
            // the weapon assets as struct dumps: weapon (WeaponDef 0x1550, its name hash at 0), its
            // tunables (0x1218, the name hash at 0x40, acts' bo4_unlinker_weapon_tunables), attachments;
            // their fields named by the gdt weapon layouts
            ExportsStructDumps(Kind::Weapons, { { "weapon" }, { "weapon_tunables", -1, {}, 0x40 }, { "attachment" }, { "attachment_unique" } },
                               nullptr, "weapon");
            Exports(Kind::WeaponStats, [this](const Context& C, Totals& T) { Treyarch::WeaponStats::Export(*this, C, T, "Black Ops 4"); });
            Exports(Kind::MapEnts, [this](const Context& C, Totals& T) { Treyarch::ExportMapEnts(C, T, Name(), EntityString()); });
            // material (BO4XMaterial): its image records at 0x38 (0x20 each {image 0, semantic hash u32
            // 8}, BO4XMaterialImage), their u8 count at 0x130; an image {hash 0x20, format u32 0x64, width
            // u16 0x68, height 0x6a} (Bo4DiskExport::DescribeZone)
            {
                MaterialLinkLayout M;
                M.SlotCount = 0x130, M.Slots = -1, M.Images = 0x38;
                M.RecordSize = 0x20, M.RecordSemantic = 8, M.RecordImage = 0;
                M.ImageHash = 0x20, M.ImageWidth = 0x68, M.ImageHeight = 0x6a, M.ImageFormat = 0x64, M.WideFormat = true;
                ExportsMaterialLinks(M);
            }
            // sound (SndBank 0x130, acts' bo4 sound): aliases at 0x38 (u32 count 0x30) of 0x28 {name hash
            // 0, entries 0x10, i32 count 0x18}, an entry (SndAlias) 0x1c0 holding its file's hash (assetId) at 0x48
            ExportsSoundLinks({ 0x38, 0x30, 0x28, 0, 0x10, 0x18, 0x1c0, 0x48, "sound", true });
            // no mapinfo asset: the map tables hold each map's settings (MapTable 0x30 {name, u32 count,
            // MapTableEntry list: names, loading images, objectives, accolades... acts' bo4 map_table}),
            // and aitype (0xf00, acts' bo4 aitype: weapons, characters, behaviour, its settings bundle)
            // is a flat struct, not a bundle: both as struct dumps
            ExportsStructDumps(Kind::MapInfo, { { "maptable" }, { "maptable_list" } });
            ExportsStructDumps(Kind::AiTypes, { { "aitype" } });
            // scriptbundle (0x40, acts' bo4 unlinker): bundles/<zone>/<settings type>/<name>.json
            Exports(Kind::ScriptBundles, [this](const Context& C, Totals& T) {
                const Treyarch::BundleZone Z{ &Image(), [this](uint64_t A) { return Off(A); },
                                              [this](uint32_t I) {
                                                  const auto& Strings = Bo4DiskExport::ZoneStringTable(Zone());
                                                  return I < Strings.size() ? Strings[I] : std::string();
                                              },
                                              [this](uint64_t H) { return KnownName(H); }, HashMask };
                ExportJsonAssets(Kind::ScriptBundles, C, T, "scriptbundle", [&](int64_t Pos, const std::vector<std::pair<int64_t, int64_t>>& Reads, std::string& Folder, std::string& Json) {
                    return Treyarch::ReadScriptBundle(Z, Treyarch::BundleLayout::Bo4, Pos, Reads, Folder, Json);
                });
            });
            // rawfile and luafile (0x20) {XHash name, i32 len 0x10, buffer 0x18 (len + 1 read)}
            Exports(Kind::Rawfile, [this](const Context& C, Totals& T) {
                const RawfileReader Plain = [this](int64_t Pos, std::string&, std::vector<uint8_t>& Bytes) { return RawBytes(Pos, { 0x10, 0x18 }, Bytes); };
                ExportRawfiles(C, T, { { "rawfile", Plain }, { "luafile", [this](int64_t Pos, std::string& Name, std::vector<uint8_t>& Bytes) {
                                                                if (!RawBytes(Pos, { 0x10, 0x18 }, Bytes)) return false;
                                                                NameLua(Name, Bytes);
                                                                return true;
                                                            } } });
            });
            Exports(Kind::Data, [this](const Context& C, Totals& T) {
                ExportStringTables(C, T, "stringtable", [this](int64_t Pos, uint32_t& Columns, uint32_t& Rows, std::vector<std::string>& Cells) {
                    return TypedCells(Pos, Columns, Rows, Cells);
                });
            });
        }
        ~Bo4Zone() override
        {
            if (!Serving) return;   // never served (unopened): the served zone is another's
            Bo4DiskExport::EndServe();
            GameBlackOps4::ImagesElsewhere = nullptr;
        }

        // the material headers the common zones' books filled, and those they did not
        void Measures(DiskGame::Totals& T) const override
        {
            ZonekitZone::Measures(T);
            if (!Booked.empty()) T.Counters["materials_from_common"] += Booked.size();
            if (!NotBooked.empty()) T.Counters["materials_in_no_common_zone"] += NotBooked.size();
        }

        // with the images that are not name-only headers
        void Defines(std::vector<DiskGame::AssetId>& Out) const override
        {
            ZonekitZone::Defines(Out);
            for (const auto& I : Assets.Images)
                if (!Bo4DiskExport::IsReference(I)) Out.push_back({ DiskGame::Kind::Image, I.Name });
        }

    private:
        void ExportImages(const DiskGame::Context& C, DiskGame::Totals& T)
        {
            using DiskGame::Kind;
            const auto S = Bo4DiskExport::ExportImages(Zone(), Assets, C.KindDir(Kind::Image),
                [&](const std::string& N) { return C.Wants(Kind::Image, N); }, Pkg,
                [&](size_t Done, size_t Count, const std::string& N) { return C.Advance(Done, Count, N); },
                [&](const std::string& N) { C.Needs(Kind::Image, N); });
            auto& Kt = T.Kinds[DiskGame::Kind::Image];
            Kt.Seen += S.Images - S.References;
            Kt.Written += S.Written;
            Kt.Linked += S.Linked;
            Kt.References += S.References;
            Counter("images_from_packages") += S.FromPackages;
            if (S.Missing + S.Failed)
                T.Fail(Name(), DiskGame::Kind::Image, "", std::to_string(S.Missing) + " without pixels, " + std::to_string(S.Failed) + " not written");
        }

        // not in the zone: the install's banks named after it (zm_red.all.sabl, zm_red.fr.sabs...)
        void ExportSounds(const DiskGame::Context& C, DiskGame::Totals& T)
        {
            auto& Kt = T.Kinds[DiskGame::Kind::Sound];
            if (Pkg == nullptr)
            {
                T.Fail(Name(), DiskGame::Kind::Sound, "", "sounds need --install");
                return;
            }
            WraithX::InitializeAPI();
            const auto S = Bo4DiskExport::ExportSounds(Pkg, Name(), C.KindDir(DiskGame::Kind::Sound),
                [&](const std::string& N) { return C.Wants(DiskGame::Kind::Sound, N); },
                [&](size_t Done, size_t Count, const std::string& N) { return C.Advance(Done, Count, N); },
                C.OnSoundHeader);
            if (C.OnSoundHeader) T.Counters["sound_headers"] = 1;
            Kt.Seen += S.Images;
            Kt.Written += S.Written;
            Counter("sounds_without_samples") += S.Empty;
            for (const auto& M : S.MissingSamples) T.Fail(Name(), DiskGame::Kind::Sound, M, "");
        }

        // stringtable (0x40) {XHash name, i32 columns 0x10, rows 0x14, the cells at 0x28 of 0x18 bytes
        // {16-byte value, u32, type u8 0x14}}: as acts writes them, 1 a string, 2 a hash (#name), 4
        // an int, 5 a float, 6 a bool, 7 a localized hash (&#name), 8 a raw hash (8#name), 3 and 9
        // unknown (their value as hex), 0 nothing
        bool TypedCells(int64_t Pos, uint32_t& Columns, uint32_t& Rows, std::vector<std::string>& Cells) const
        {
            const int32_t Cols = At<int32_t>(Image(), Pos + 0x10), Count = At<int32_t>(Image(), Pos + 0x14);
            Columns = Rows = 0;
            Cells.clear();
            if (Cols <= 0 || Count <= 0) return Cols >= 0 && Count >= 0;
            const uint64_t All = (uint64_t)Cols * (uint64_t)Count;
            const int64_t At0 = Off(PointerAt(Pos + 0x28));
            if (At0 < 0 || All > (1u << 24) || (uint64_t)At0 + All * 0x18 > Image().size()) return false;
            Columns = (uint32_t)Cols, Rows = (uint32_t)Count;
            const auto Hash = [this](const char* Prefix, uint64_t H) {
                const std::string N = KnownName(H & HashMask);
                return std::string(Prefix) + (N.empty() ? std::format("{:x}", (unsigned long long)(H & HashMask)) : N);
            };
            for (uint64_t K = 0; K < All; K++)
            {
                const int64_t Cell = At0 + (int64_t)K * 0x18;
                const uint64_t V = At<uint64_t>(Image(), Cell);
                switch (At<uint8_t>(Image(), Cell + 0x14))
                {
                case 1: Cells.push_back(TextAt(Cell)); break;
                case 2: Cells.push_back(Hash("#", V)); break;
                case 4: Cells.push_back(std::to_string(At<int32_t>(Image(), Cell))); break;
                case 5: Cells.push_back(std::format("{}", At<float>(Image(), Cell))); break;
                case 6: Cells.push_back(At<uint8_t>(Image(), Cell) ? "TRUE" : "FALSE"); break;
                case 7: Cells.push_back(Hash("&#", V)); break;
                case 8: Cells.push_back(Hash("8#", V)); break;
                case 0: Cells.emplace_back(); break;
                default: Cells.push_back(std::format("u{}?{:x}", At<uint8_t>(Image(), Cell + 0x14), (unsigned long long)V)); break;
                }
            }
            return true;
        }

    protected:
        const char* Profile() const override { return "bo4"; }
        std::string HashName(uint64_t Hash) const override { return KnownName(Hash); }
        uint64_t HashBits() const override { return HashMask; }
        const ICoDGame& LiveGame() const override { return LiveGames::Get("T8"); }
        VirtualGameSession::Handlers Handlers() override { return Bo4DiskExport::LiveHandlers(); }
        // the zone the game loads: the .ff decompressed, with its .fd update patch applied
        bool LoadBytes(std::vector<uint8_t>& Zone, std::string& Error) override
        {
            return Bo4DiskExport::LoadZone(FastFilePath(), Zone, Error);
        }
        // an asset pointer's back reference goes through resolve_asset (0x2eba2b0), which reads
        // the pointer at the referenced place: another material's image slot, not an image
        bool DerefAssetPointers() const override { return true; }
        void Fixups() override
        {
            Bo4DiskExport::DescribeZone(Zone(), Walk(), Assets);
            Served(true);
        }
        void Served(bool On) override
        {
            Serving = On;
            if (!On)
            {
                Bo4DiskExport::EndServe();
                GameBlackOps4::ImagesElsewhere = nullptr;
                return;
            }
            Bo4DiskExport::ServeZone(Zone(), Assets);
            // a material header takes its images from the common zone that defines it
            GameBlackOps4::ImagesElsewhere = [this](uint64_t Hash, GameBlackOps4::MaterialElsewhere& Out) {
                for (const auto& Book : Common)
                    if (const auto It = Book->find(Hash); It != Book->end())
                    {
                        Out.Images = It->second.Images;
                        Out.Techset = It->second.Techset;
                        Out.Constants = It->second.Constants;
                        Booked.insert(Hash);
                        return true;
                    }
                NotBooked.insert(Hash);
                return false;
            };
        }
        std::string AssetName(DiskGame::Kind K, int64_t Pos) const override
        {
            if (K == DiskGame::Kind::Anim)
                return Bo4DiskExport::LiveName(At<uint64_t>(Zone(), Pos + offsetof(BO4XAnim, NamePtr)) & HashMask, "xanim");
            if (K == DiskGame::Kind::Script) return HashedName(At<uint64_t>(Zone(), Pos) & HashMask, "script");
            if (K == DiskGame::Kind::Rawfile || K == DiskGame::Kind::Data)
            {
                // a path, with its extension, by the package index's names and strings
                const uint64_t H = At<uint64_t>(Zone(), Pos) & HashMask;
                const std::string N = FullName(H);
                return !N.empty() ? N : std::format("{}_{:x}", K == DiskGame::Kind::Rawfile ? "rawfile" : "stringtable", (unsigned long long)H);
            }
            return Bo4DiskExport::LiveName(At<uint64_t>(Zone(), Pos) & HashMask, "xmodel");
        }
        // a name-only header (no LOD and no bone; no frame and no bone): another zone defines it
        bool IsReference(DiskGame::Kind K, const std::string&, int64_t Pos) const override
        {
            if (K == DiskGame::Kind::Script) return Off(PointerAt(Pos + 0x10)) < 0 || At<int32_t>(Image(), Pos + 0x18) <= 0;
            // a rawfile or a string table whose data another zone holds (its pointer not in this one)
            if (K == DiskGame::Kind::Rawfile) return At<int32_t>(Image(), Pos + 0x10) > 0 && Off(PointerAt(Pos + 0x18)) < 0;
            if (K == DiskGame::Kind::Data)
                return At<int32_t>(Image(), Pos + 0x10) > 0 && At<int32_t>(Image(), Pos + 0x14) > 0 && Off(PointerAt(Pos + 0x28)) < 0;
            if (K == DiskGame::Kind::Anim)
                return At<uint16_t>(Zone(), Pos + offsetof(BO4XAnim, NumFrames)) == 0 &&
                       At<uint64_t>(Zone(), Pos + offsetof(BO4XAnim, BoneIDsPtr)) == 0;
            return At<uint32_t>(Zone(), Pos + 0xc8) == 0 && At<uint8_t>(Zone(), Pos + 0x14b) == 0;
        }
        // LocalizeEntry (0x18): the text pointer at +0, the name hash at +8. The text is the
        // game's encrypted string when its first byte is 10xxxxxx (cipher id, then the length),
        // decrypted as the string table is (Bo4DiskExport::ReadZoneStrings).
        const char* LocalizeType() const override { return "localize_entry"; }
        void LocalizeEntry(int64_t Pos, std::string& Key, std::string& Text) const override
        {
            const uint64_t Addr = At<uint64_t>(Image(), Pos);
            if (Addr < VirtualGameSession::Base || Addr >= VirtualGameSession::Base + Zone().size()) return;
            const auto& Z = Zone();
            size_t End = 0;
            Text = Bo4DiskExport::ZoneString(Z, (size_t)(Addr - VirtualGameSession::Base), End);
            Key = Bo4DiskExport::LiveName(At<uint64_t>(Z, Pos + 8) & HashMask, "localize");
        }

        bool ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models) override;
        // Count entities (LinkerSpawnVar, 0x30) at Array as map_ents text, their hashes named where
        // the name dbs can, Model giving one its "model" (Treyarch::EntityText)
        std::string Entities(int64_t Count, uint64_t Array, const std::function<std::string(uint64_t, int32_t)>& Model);
        // the entity_list's entities, then the trigger_list's, as the map_ents text ("" without them)
        std::string EntityString();

    private:
        CoDPackageCache* Pkg = nullptr;
        Bo4DiskExport::ZoneAssets Assets;
        std::vector<std::shared_ptr<const Bo4DiskExport::MaterialBook>> Common;
        std::set<uint64_t> Booked, NotBooked;   // material headers met, filled or not
        bool Serving = false;
    };

    // GfxWorld (gfxworld, 0x1ab0): the fields C2M's BO4 reader uses, their places from the
    // the zonekit table (GfxWorldDraw at 0xd8, GfxWorldDpvsStatic at 0x1b8, both embedded).
    constexpr int64_t WorldName = 0x10, WorldSurfaceCount = 0x18;
    constexpr int64_t WorldVertexCount = 0xf8, WorldPositions = 0x100, WorldVertexData = 0x128;
    constexpr int64_t WorldIndexCount = 0x150, WorldIndices = 0x158;
    constexpr int64_t WorldModelCount = 0x180, WorldModels = 0x188, BrushModelSize = 80;
    constexpr int64_t WorldSurfaces = 0x3f8;
    constexpr int64_t WorldSModelCount = 0x1bc, WorldSModels = 0x400, WorldSModelPlaces = 0x408, SModelSize = 0x38;
    // ComWorld: its primary lights (600 bytes each); clipMap: two dynamic entity lists (0x188 each)
    constexpr int64_t ComLightCount = 0x14, ComLights = 0x18;
    constexpr int64_t ClipDynCount = 0x148, ClipDyns = 0x150, DynSize = 0x188;
    // lighting: its states (0x14c0 each, table f2e1c560) hold four 0x28-byte sky entries from
    // 0x1308 (table f2e1c1d0), an XModel* first.
    constexpr int64_t LightingStateCount = 0x10, LightingStates = 0x18, StateSky = 0x1308, StateSkyStride = 0x28;
    // a sky entry's image at +0x18 (null for the light state: its sky material samples none, its
    // technique set is the Bruneton scattering shader, earth radius 6360 km at state +0x150); the
    // state's sun angles (pitch, yaw, degrees: light travels along their forward) at +0x50
    constexpr int64_t SkyImage = 0x18, StateSunAngles = 0x50;

    // entity_list (0x20): the entity count at 0x10, the entities at 0x18 (Treyarch::EntityText)
    constexpr int64_t EntCount = 0x10, Ents = 0x18;
    // trigger_list (0x50): MapTriggers at 0x10 (Maps::ReadTriggers), its entities' count (u64) at
    // 0x40 and array at 0x48
    constexpr int64_t TriggerModelCount = 0x10, TriggerEntCount = 0x40, TriggerEnts = 0x48;

    // clipMap_t (clipmap 0x2d8), its brushes in the table embedded at 0x260 (f2e143c0). A brush's sides are a run of the table's
    // planes, its pointers back references; materials have flags and no name.
    Maps::ClipLayout ClipLayout()
    {
        Maps::ClipLayout C;
        C.InfoAt = 0x260;
        C.Info.PlaneCount = 0, C.Info.Planes = 8, C.Info.LeafBrushNodeCount = 0x10, C.Info.LeafBrushNodes = 0x18;
        C.Info.BrushCount = 0x50, C.Info.Brushes = 0x58, C.Info.BrushCountBytes = 4, C.Info.MaterialCount = 0x60, C.Info.Materials = 0x68;
        C.MaterialSize = 8, C.MaterialName = -1, C.MaterialSurface = 0, C.MaterialContents = 4;
        C.BrushSize = 0x40, C.BrushSides = 0, C.BrushMins = 0x10, C.BrushContents = 0x1c, C.BrushMaxs = 0x20, C.AxialMaterial = 0x2c;
        C.BrushSideCount = 0x3a, C.BrushSideCountBytes = 2;
        C.SideSize = 0x14, C.SidePlane = 0, C.SidePlaneInline = true, C.SideMaterial = 0x10, C.SideMaterialBytes = 2;
        C.BackRefs = true;
        C.ModelCount = 0xb0, C.Models = 0xb8, C.ModelSize = 88, C.ModelLeafBrushNode = 0x48;
        C.NodeSize = 0x20, C.NodeCount = 4, C.NodeBrushes = 0x10, C.NodeChildren = 0x18, C.NodeWide = true;
        C.VertexCount = 0x70, C.Vertices = 0x78, C.TriangleCount = 0x80, C.Triangles = 0x88, C.TriangleIndexBytes = 4;
        return C;
    }

    std::string KnownName(uint64_t Hash) { return NameOf(Hash, false); }

    // A hash's name: the asset name cache, else the string hashes (script_sound...), else ""; with
    // its extension (a rawfile's path) or without (an asset's name)
    std::string NameOf(uint64_t Hash, bool Full)
    {
        static const WraithNameIndex Strings = [] {
            WraithNameIndex I;
            I.LoadIndex((std::filesystem::path(FileSystems::GetApplicationPath()) / "package_index\\fnv1a_string.wni").string());
            return I;
        }();
        Hash &= 0x0FFFFFFFFFFFFFFFull;
        const auto& Assets = GameBlackOps4::AssetNameCache.NameDatabase;
        if (const auto It = Assets.find(Hash); It != Assets.end()) return Full ? It->second : FileSystems::GetFileNamePurgeExtensions(It->second);
        if (const auto It = Strings.NameDatabase.find(Hash); It != Strings.NameDatabase.end()) return It->second;
        return std::string();
    }

    std::string Bo4Zone::Entities(int64_t Count, uint64_t Array, const std::function<std::string(uint64_t, int32_t)>& Model)
    {
        const auto& Img = Image();
        Treyarch::EntityCounts N;
        const std::string S = Treyarch::EntityText(Img, Off(Array), (uint64_t)Count, [this](uint64_t A) { return Off(A); },
                                                   [this](uint64_t A) { return StringAt(A); }, KnownName, N, Model);
        Counter("map_ents") += Count;
        Counter("map_ents_keys") += N.Keys;
        Counter("map_ents_keys_named") += N.KeysNamed;
        Counter("map_ents_hashes") += N.Hashes;
        Counter("map_ents_hashes_named") += N.HashesNamed;
        return S;
    }

    std::string Bo4Zone::EntityString()
    {
        const auto& Img = Image();
        std::string Text;
        if (const int64_t List = Root("entity_list"); List >= 0)
            Text = Entities((int64_t)At<uint64_t>(Img, List + EntCount), At<uint64_t>(Img, List + Ents),
                            [](uint64_t, int32_t Index) { return Index > 0 ? "*" + std::to_string(Index) : std::string(); });
        if (const int64_t List = Root("trigger_list"); List >= 0)
        {
            const uint32_t Volumes = At<uint32_t>(Img, List + TriggerModelCount);
            Text += Entities((int64_t)At<uint64_t>(Img, List + TriggerEntCount), At<uint64_t>(Img, List + TriggerEnts),
                             [Volumes](uint64_t E, int32_t) { return E < Volumes ? "?" + std::to_string(E) : std::string(); });
        }
        return Text;
    }

    bool Bo4Zone::ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models)
    {
        const int64_t World = Root("gfxworld");
        if (World < 0) return false;
        const auto& Img = Image();
        Scene.Name = StringAt(At<uint64_t>(Img, World + WorldName));
        if (Scene.Name.empty()) Scene.Name = Name();
        const auto Model = [this](uint64_t Address) { return ModelAt(Address); };

        // ---- world: brush model 0 is the level, the others its moving parts. Surfaces: the first
        // vertex (an index) at 12, the triangle count at 38, the first index at 40; brushes: the
        // surface count at 72, the first at 76.
        const uint32_t VertexCount = At<uint32_t>(Img, World + WorldVertexCount);
        const int64_t Positions = Off(At<uint64_t>(Img, World + WorldPositions)), Data = Off(At<uint64_t>(Img, World + WorldVertexData));
        Maps::WorldLayout L;
        L.Image = &Img;
        L.Surfaces = Off(At<uint64_t>(Img, World + WorldSurfaces));
        L.SurfaceCount = (uint32_t)std::max(At<int32_t>(Img, World + WorldSurfaceCount), 0);
        L.Brushes = Off(At<uint64_t>(Img, World + WorldModels));
        L.BrushCount = (uint32_t)std::max(At<int32_t>(Img, World + WorldModelCount), 0);
        L.Indices = Off(At<uint64_t>(Img, World + WorldIndices));
        L.IndexCount = At<uint32_t>(Img, World + WorldIndexCount);
        L.SurfaceSize = Treyarch::SurfaceSize, L.FirstVertex = 12, L.TriCount = 38, L.BaseIndex = 40, L.Material = Treyarch::SurfaceMaterial;
        L.BrushSize = BrushModelSize, L.FirstSurface = 76, L.SurfaceCountAt = 72;
        if (FILE* Dbg = Maps::DebugFile())
            std::fprintf(Dbg, "[map] %s vertices %u at %llx/%llx indices %u at %llx surfaces %u at %llx brushes %u at %llx\n",
                         Scene.Name.c_str(), VertexCount, (long long)Positions, (long long)Data, L.IndexCount, (long long)L.Indices,
                         L.SurfaceCount, (long long)L.Surfaces, L.BrushCount, (long long)L.Brushes);
        const auto MaterialOf = [&](uint64_t Address) {
            return Address == 0 ? std::string() : Maps::AddMaterial(Scene, GameBlackOps4::ReadXMaterial(Address));
        };
        if (const size_t Bad = Maps::AddWorld(Scene, L, Treyarch::WorldVertices(Img, Positions, Data, VertexCount), MaterialOf))
            Counter("map_triangles_unread") += Bad;

        // ---- static models: the model at +0; their placements beside (64 bytes each: a rotation
        // quaternion, the origin, the scale at 28)
        const uint32_t SModels = At<uint32_t>(Img, World + WorldSModelCount);
        const int64_t SModelArray = Off(At<uint64_t>(Img, World + WorldSModels));
        const int64_t Places = Off(At<uint64_t>(Img, World + WorldSModelPlaces));
        for (uint32_t K = 0; SModelArray >= 0 && Places >= 0 && K < SModels; K++)
        {
            const uint64_t Address = At<uint64_t>(Img, SModelArray + (int64_t)K * SModelSize);
            const std::string ModelName = ModelAt(Address);
            if (ModelName.empty()) continue;
            Maps::Instance In;
            In.Name = "smodel_" + std::to_string(K);
            In.Model = ModelName;
            const int64_t P = Places + (int64_t)K * 64;
            float Q[4];
            for (int J = 0; J < 4; J++) Q[J] = At<float>(Img, P + J * 4);
            Maps::AxisFromQuat(Q, In.Axis);
            for (int J = 0; J < 3; J++) In.Origin[J] = At<float>(Img, P + 16 + J * 4);
            In.Scale = At<float>(Img, P + 28);
            Scene.StaticInstances.push_back(In);
            Models.emplace(ModelName, Address);
        }

        // ---- dynamic entities (the clip map's two lists) and primary lights
        if (const int64_t Clip = Root("clipmap"); Clip >= 0)
            for (int List = 0; List < 2; List++)
                Treyarch::DynamicEntities(Img, Off(At<uint64_t>(Img, Clip + ClipDyns + List * 8)), At<uint16_t>(Img, Clip + ClipDynCount + List * 2),
                                          DynSize, "dynent_" + std::to_string(List) + "_", Model, Scene, Models);
        if (const int64_t Com = Root("comworld"); Com >= 0)
            Treyarch::Lights(Img, Off(At<uint64_t>(Img, Com + ComLights)), At<uint32_t>(Img, Com + ComLightCount),
                             { 600, 168, 84, 188, 200 }, Scene);

        // ---- the entities as the map_ents text: { "key" "value" ... } per entity; a brush model's
        // its model index as "*N". The trigger_list's entities after them, the first ones each
        // the trigger volume of its number ("?N": mp_frenetic's 61 volumes are its 61 first
        // entities, trigger_hurt's two the sky-sized last; the radius and box triggers after them
        // have none)
        Scene.MapEnts = EntityString();
        if (Root("trigger_list") >= 0) AddTriggers(Scene, TriggerModelCount, "trigger_list");

        // ---- the clip map's brushes, brush models and collision triangles (Maps::ClipLayout)
        AddCollision(Scene, { "clipmap" }, ClipLayout());

        // ---- the sky: the first lighting state's first sky model but the black default
        if (const int64_t Lit = Root("lighting"); Lit >= 0 && At<uint32_t>(Img, Lit + LightingStateCount) > 0)
            if (const int64_t State = Off(At<uint64_t>(Img, Lit + LightingStates)); State >= 0)
                for (int K = 0; K < 4; K++)
                {
                    const uint64_t Address = At<uint64_t>(Img, State + StateSky + K * StateSkyStride);
                    const std::string Sky = ModelAt(Address);
                    if (Sky.empty() || Sky == "skybox_default_black") continue;
                    Scene.Sky.Model = Sky;
                    Models.emplace(Sky, Address);
                    Scene.Sky.Atmosphere = At<uint64_t>(Img, State + StateSky + K * StateSkyStride + SkyImage) == 0;
                    const float Pitch = At<float>(Img, State + StateSunAngles) * 0.01745329f;
                    const float Yaw = At<float>(Img, State + StateSunAngles + 4) * 0.01745329f;
                    const float Forward[3] = { std::cos(Pitch) * std::cos(Yaw), std::cos(Pitch) * std::sin(Yaw), -std::sin(Pitch) };
                    for (int J = 0; J < 3; J++) Scene.Sky.Sun[J] = -Forward[J];
                    break;
                }
        return true;
    }

    class Bo4Game final : public DiskGame::IDiskGame
    {
    public:
        const char* Id() const override { return "bo4"; }
        const char* Title() const override { return "Call of Duty: Black Ops 4"; }
        const char* Engine() const override { return "T8"; }
        std::vector<std::string> Aliases() const override { return { "bo4-disk-export" }; }
        // the game's own bytes: no BC decode or PNG encode, half the disk
        const char* DefaultImageFormat() const override { return "DDS"; }
        const std::vector<DiskGame::Setting>& Settings() const override
        {
            static const std::vector<DiskGame::Setting> All = { UiRes };
            return All;
        }
        std::unique_ptr<DiskGame::IDiskZone> Unopened() const override
        {
            return std::make_unique<Bo4Zone>(std::string(), nullptr,
                                             std::vector<std::shared_ptr<const Bo4DiskExport::MaterialBook>>());
        }

        // en_core_common's strings, their keys hashed (Treyarch::HashedLocalizeKey): multiplayer
        // maps by mpui/ (mp_frenetic "Frequency"), zombies by zmui/ (zm_zodt8 "Voyage of Despair"),
        // Blackout's by mpui/ or presence/; a texture pack's or a censored set's copy of a level
        // (4k_, arasafe_...) by the level's. English, of the text languages an install may hold.
        const LevelNaming* LevelNames() const override
        {
            static const LevelNaming Naming = [] {
                LevelNaming N;
                N.From = { "en_core_common" };
                N.Keys = { { "mp_*", { "MPUI/{REST}", "PRESENCE/{NAME}" } },
                           { "zm_*", { "ZMUI/{REST}" } },
                           { "wz_*", { "MPUI/{NAME}", "PRESENCE/{NAME}" } },
                           { "*_mp_*", { "MPUI/{REST2}", "PRESENCE/MP_{REST2}" } },
                           { "*_zm_*", { "ZMUI/{REST2}" } },
                           { "*_wz_*", { "MPUI/WZ_{REST2}", "PRESENCE/WZ_{REST2}" } } };
                N.Hashed = Treyarch::HashedLocalizeKey;
                return N;
            }();
            return &Naming;
        }

        bool Zones(const std::string& Input, const ExportOptions& Options, std::vector<std::string>& Out,
                   std::string& Error) const override
        {
            size_t Skipped = 0;
            return Bo4DiskExport::Zones(Input, Packages(Options), CacheDir(Options, "greyhound_bo4ff"), UiRes.Of(Options),
                                        Out, Skipped, Error);
        }

        // the fast files in CASC, the UI resolution's set only (ui_res)
        bool ListZones(const std::string& Install, const ExportOptions& Options, std::vector<std::string>& Names,
                       std::string& Error) const override
        {
            CoDPackageCache* P = TreyarchPackages::Open<XPAKCache>(Install);
            if (P == nullptr)
            {
                Error = "cannot open the packages under " + Install;
                return false;
            }
            for (auto& N : TreyarchPackages::ListFastFiles(P))
                if (Bo4DiskExport::KeepZoneForUiRes(N, UiRes.Of(Options))) Names.push_back(std::move(N));
            if (Names.empty()) Error = "no fast file in " + Install;
            return !Names.empty();
        }

        // each picked fast file pulled from CASC (with its .fd patch) into the cache
        bool ResolveZones(const std::string& Install, const std::vector<std::string>& Names, const ExportOptions& Options,
                          std::vector<std::string>& Out, std::string& Error) const override
        {
            CoDPackageCache* P = TreyarchPackages::Open<XPAKCache>(Install);
            for (const auto& N : Names)
            {
                const auto Paths = TreyarchPackages::ExtractFastFiles(P, N, CacheDir(Options, "greyhound_bo4ff"), Error);
                Out.insert(Out.end(), Paths.begin(), Paths.end());
            }
            if (Out.empty() && Error.empty()) Error = "no picked zone in " + Install;
            return !Out.empty();
        }

        std::unique_ptr<DiskGame::IDiskZone> Open(const std::string& Path, const ExportOptions& Options,
                                                  std::string& Error) const override
        {
            // the common zones' materials first, each read alone (and once: kept in the cache)
            auto Common = Bo4DiskExport::CommonMaterials(Path, Packages(Options), CacheDir(Options, "greyhound_bo4ff"),
                                                         CacheDir(Options, "greyhound_bo4mat"));
            auto Zone = std::make_unique<Bo4Zone>(Path, Packages(Options), std::move(Common));
            if (!Options.Profile.empty()) Zone->SetProfile(Options.Profile);
            if (!Zone->Open(Error)) return nullptr;
            return Zone;
        }
    };

    const Bo4Game Game;
    const DiskGame::Registration Registered(Game);
}
