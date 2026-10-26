#include "stdafx.h"

// Black Ops Cold War (T9) on the disk template: a ZonekitZone with the t9 profile, served to the
// live Cold War readers (GameBlackOpsCW). What is Cold War's own: asset names are 60-bit hashes
// (named from the name index when known), the string slots come from the zone's own string table,
// its text encrypted (StringCipher.h).
// Zones come from CASC (casc:<pattern>, .fd patches applied), streamed data from the .xsub packages.

#include "DiskGame.h"
#include "FileSystems.h"
#include "Games/Treyarch/TreyarchAnimOwners.h"
#include "Games/Treyarch/TreyarchEffects.h"
#include "GameBlackOpsCW.h"
#include "Games/Treyarch/TreyarchLevelNames.h"
#include "Games/Treyarch/TreyarchMaps.h"
#include "MapLive.h"
#include "Games/Treyarch/TreyarchPackages.h"
#include "Games/Treyarch/TreyarchScriptBundles.h"
#include "Games/Treyarch/TreyarchWeaponStats.h"
#include "Games/IW/IwctxReaders.h"
#include "ClipMap.h"
#include "StringCipher.h"
#include "Terrain.h"
#include "XSUBCache.h"
#include "ZonekitZone.h"
#include "WraithNameIndex.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <format>
#include <mutex>
#include <unordered_map>

namespace
{
    constexpr uint64_t HashMask = 0x0FFFFFFFFFFFFFFFull;   // Cold War's asset name hashes are 60-bit

    namespace fs = std::filesystem;

    using DiskGame::Context;
    using DiskGame::ExportOptions;
    using DiskGame::Kind;
    using DiskGame::Totals;

    class ColdWarZone final : public ZonekitZone
    {
    public:
        explicit ColdWarZone(std::string Path) : ZonekitZone(std::move(Path))
        {
            // scriptparsetree {hash, buffer 8, u32 len 0x10}: a GSCOBJ of the T9 VM, decompiled by acts;
            // its string literals are encrypted (first byte 0x80-0xBF), decrypted by the game's own
            // function, which acts finds in the dumped exe
            Exports(Kind::Script, [this](const Context& C, Totals& T) {
                constexpr Scripts::Vm T9Vm{ Scripts::Tool::Acts, "", "pc", "c", "BlackOpsColdWar_dump.exe" };
                ExportScripts(C, T, T9Vm, { { "scriptparsetree", [this](int64_t Pos, Scripts::Script& S) {
                                  S.Compiled = true;
                                  return BytesAt(PointerAt(Pos + 8), At<uint32_t>(Image(), Pos + 0x10), S.Bytes);
                              } } });
            });
            ExportsGdt();
            const auto Decrypted = [this](const std::string& Raw) {
                std::string Error, Text;
                return CipherTable(Error) && ColdWar::DecryptString(Raw, Text) ? Text : Raw;
            };
            ExportsAssetRefs(Decrypted);
            // the weapon assets as struct dumps: weapon (WeaponDef 0x1918, its name hash at 0), its tunables
            // (0x1418, a hash at 0x30 distinct on each of mp_common's 100, 0 its bounce pointers), style
            // settings, blueprints, attachments; their fields named by the gdt weapon layouts
            ExportsStructDumps(Kind::Weapons,
                               { { "weapon" }, { "weapontunables", -1, {}, 0x30 }, { "weaponstylesettings" }, { "weaponfull" }, { "weaponblueprint" },
                                 { "weaponsecondarymovement" }, { "attachment" }, { "attachmentunique" }, { "attachmenttable" } },
                               Decrypted, "weapon");
            Exports(Kind::WeaponStats, [this](const Context& C, Totals& T) { Treyarch::WeaponStats::Export(*this, C, T, "Black Ops Cold War"); });
            Exports(Kind::MapEnts, [this](const Context& C, Totals& T) { Treyarch::ExportMapEnts(C, T, Name(), EntityString()); });
            // the map tables (each map's settings: maptable, its maptableentry assets) and the AI types
            // (a flat struct, its settings bundle among its pointers) as struct dumps
            ExportsStructDumps(Kind::MapInfo, { { "maptable" }, { "maptableentry" }, { "maptablelist" } }, Decrypted);
            ExportsStructDumps(Kind::AiTypes, { { "aitype" } }, Decrypted);
            // scriptbundle (0x30, acts' cw unlinker): bundles/<zone>/<settings type>/<name>.json, its
            // script strings the zone's decrypted ones
            Exports(Kind::ScriptBundles, [this](const Context& C, Totals& T) {
                if (!SlotsRead)
                {
                    Slots = ZoneStrings(Zone());
                    SlotsRead = true;
                }
                const Treyarch::BundleZone Z{ &Image(), [this](uint64_t A) { return Off(A); },
                                              [this](uint32_t I) { return I < Slots.size() ? Slots[I] : std::string(); },
                                              [this](uint64_t H) { return HashName(H); }, HashMask };
                ExportJsonAssets(Kind::ScriptBundles, C, T, "scriptbundle", [&](int64_t Pos, const std::vector<std::pair<int64_t, int64_t>>& Reads, std::string& Folder, std::string& Json) {
                    return Treyarch::ReadScriptBundle(Z, Treyarch::BundleLayout::ColdWar, Pos, Reads, Folder, Json);
                });
            });
            Exports(Kind::Map, [this](const Context& C, Totals& T) { ExportMap(C, T); });
            // the images, pixels from the .xsub packages (the live LoadXImage): a map's textures
            Exports(Kind::Image, [this](const Context& C, Totals& T) { ExportImages(C, T); });
            // the sound_assets, Opus from the .xsub packages (the live ReadXSound)
            Exports(Kind::Sound, [this](const Context& C, Totals& T) { ExportSounds(C, T); });
            // fx (0x90, elements 0x340) as far as the tables say: types, visuals, effects started
            Exports(Kind::Fx, [this](const Context& C, Totals& T) {
                auto Z = EffectView();
                Z.AssetName = [this](int64_t P) { return NamedAsset("asset", P); };
                // with the images: each material's, for the preview to find among them
                Z.ReadMaterials(C, Treyarch::MaterialReader([](uint64_t A) { return GameBlackOpsCW::ReadXMaterial(A); }, [this](uint64_t A) { return Off(A); }),
                                false);
                ExportEffects(C, T, "fx", [&](int64_t Pos, const std::string& N, std::string& J) {
                    return Treyarch::ReadEffect(Z, Treyarch::EffectLayout::ColdWar, Pos, N, J);
                });
            });
            // rawfile and luafile (0x18) {hash, u32 len 8, buffer 0x10 (len + 1 read)}, as they are
            // (rawfilepreproc and rawtextfile, the same shape, are encrypted: not written)
            Exports(Kind::Rawfile, [this](const Context& C, Totals& T) {
                ExportRawfiles(C, T, { { "rawfile", [this](int64_t Pos, std::string&, std::vector<uint8_t>& Bytes) { return RawBytes(Pos, { 8, 0x10 }, Bytes); } },
                                       { "luafile", [this](int64_t Pos, std::string& Name, std::vector<uint8_t>& Bytes) {
                                            if (!RawBytes(Pos, { 8, 0x10 }, Bytes)) return false;
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

        // stringtable (0x38) {hash, u32 columns 8, rows 0xc, cells 0x18 of 0x10 bytes {8-byte value, u32,
        // type u8 0xc}}, the types Black Ops 4's (a string unless 2 to 8, its text encrypted as the
        // zone's strings; 2 a hash #name, 4 an int, 5 a float, 6 a bool, 7 a localized hash &#name,
        // 8 a raw hash 8#name, 3 unknown: its value as hex)
        bool TypedCells(int64_t Pos, uint32_t& Columns, uint32_t& Rows, std::vector<std::string>& Cells) const
        {
            Columns = At<uint32_t>(Image(), Pos + 8), Rows = At<uint32_t>(Image(), Pos + 0xc);
            const uint64_t All = (uint64_t)Columns * Rows;
            Cells.clear();
            if (All == 0) return true;
            const int64_t At0 = Off(PointerAt(Pos + 0x18));
            if (At0 < 0 || All > (1u << 24) || (uint64_t)At0 + All * 0x10 > Image().size()) return false;
            std::string Error;
            const bool Cipher = CipherTable(Error);
            const auto Hash = [this](const char* Prefix, uint64_t H) {
                const std::string N = HashName(H & HashMask);
                return std::string(Prefix) + (N.empty() ? std::format("{:x}", (unsigned long long)(H & HashMask)) : N);
            };
            for (uint64_t K = 0; K < All; K++)
            {
                const int64_t Cell = At0 + (int64_t)K * 0x10;
                const uint64_t V = At<uint64_t>(Image(), Cell);
                const uint8_t Type = At<uint8_t>(Image(), Cell + 0xc);
                switch (Type)
                {
                case 2: Cells.push_back(Hash("#", V)); break;
                case 3: Cells.push_back(std::format("u3?{:x}", (unsigned long long)V)); break;
                case 4: Cells.push_back(std::to_string(At<int32_t>(Image(), Cell))); break;
                case 5: Cells.push_back(std::format("{}", At<float>(Image(), Cell))); break;
                case 6: Cells.push_back(At<uint8_t>(Image(), Cell) ? "TRUE" : "FALSE"); break;
                case 7: Cells.push_back(Hash("&#", V)); break;
                case 8: Cells.push_back(Hash("8#", V)); break;
                default:
                {
                    std::string Text;
                    const std::string Raw = TextAt(Cell);
                    Cells.push_back(Cipher && ColdWar::DecryptString(Raw, Text) ? Text : Raw);
                }
                }
            }
            return true;
        }

        // with its anim owners' facts (TreyarchAnimOwners.h): aitype (0x12f8: its name a string at
        // 8, characters 0x798, animmappingtables 0x800), character (bodies and heads at 0x60-0x78),
        // xmodelalias (models from 8), animmappingtable {hash, aliases 8, lists 0x10, count 0x18} of
        // lists {u32 count, XAnim* array 8}, weapon (view model 0xaf8, szXAnims 0x4d0,
        // its 128 attachmentuniques 0x628 with theirs at 0x508)
        void Documents(std::vector<std::pair<std::string, std::string>>& Out) const override
        {
            ZonekitZone::Documents(Out);
            TreyarchAnimOwners::Layout L;
            L.AiText = 8, L.Characters = 0x798, L.AnimMaps = 0x800;
            L.Body = 0x60, L.BodyAlias = 0x68, L.Head = 0x70, L.HeadAlias = 0x78;
            L.AliasModels = 8, L.AliasSlots = 50;
            L.EntrySize = 0x10, L.EntryAnims = 8, L.EntryCount = 0;
            L.GunModel = 0xaf8, L.WeaponAnims = { { 0x4d0, 404 } };
            L.UniqueType = "attachmentunique", L.Uniques = 0x628, L.UniqueSlots = 128, L.UniqueAnims = { { 0x508, 404 } };
            TreyarchAnimOwners::ZoneView V;
            V.Image = &Image();
            V.Off = [this](uint64_t A) { return Off(A); };
            V.Assets = [this](const char* Type) {
                std::vector<int64_t> P;
                for (const auto& I : Walk().Instances)
                    if (I.Parent < 0 && OwnerIs(I, Type)) P.push_back(I.Pos);
                return P;
            };
            V.Model = [this](int64_t P) { return AssetName(Kind::Model, P); };
            V.Anim = [this](int64_t P) { return AssetName(Kind::Anim, P); };
            V.Hashed = [this](uint64_t H, const char* Prefix) {
                const std::string N = HashName(H);
                return N.empty() ? std::format("{}_{:x}", Prefix, (unsigned long long)H) : N;
            };
            V.Text = [this](uint64_t A) {
                std::string Error, Text;
                const std::string Raw = StringAt(A);
                return CipherTable(Error) && ColdWar::DecryptString(Raw, Text) ? Text : Raw;
            };
            V.HashMask = HashMask;
            V.Key = IndexKey();
            Out.emplace_back(TreyarchAnimOwners::DocumentName, TreyarchAnimOwners::Text(TreyarchAnimOwners::Read(V, L)));
        }

    protected:
        const char* Profile() const override { return "t9"; }
        // a hash field's name: the asset name index, else the string hashes
        std::string HashName(uint64_t Hash) const override
        {
            static const WraithNameIndex Strings = [] {
                WraithNameIndex I;
                I.LoadIndex((fs::path(FileSystems::GetApplicationPath()) / "package_index\\fnv1a_string.wni").string());
                return I;
            }();
            const auto& Assets = GameBlackOpsCW::AssetNameCache.NameDatabase;
            if (const auto It = Assets.find(Hash); It != Assets.end()) return It->second;
            if (const auto It = Strings.NameDatabase.find(Hash); It != Strings.NameDatabase.end()) return It->second;
            return std::string();
        }
        uint64_t HashBits() const override { return HashMask; }
        // localizeentry (0x10): the text at 0, encrypted as the zone's strings, the key's 60-bit
        // hash at 8 (named when an index knows it, else localize_<hex>). The cipher table is loaded
        // here too: a run of localized strings alone never serves the zone (Handlers).
        const char* LocalizeType() const override { return "localizeentry"; }
        void LocalizeEntry(int64_t Pos, std::string& Key, std::string& Text) const override
        {
            std::string Error;
            if (!CipherTable(Error) || !ColdWar::DecryptString(StringAt(PointerAt(Pos)), Text)) Text.clear();
            const uint64_t Hash = At<uint64_t>(Image(), Pos + 8) & HashMask;
            Key = HashName(Hash);
            if (Key.empty()) Key = std::format("localize_{:x}", (unsigned long long)Hash);
        }
        const ICoDGame& LiveGame() const override { return LiveGames::Get("T9"); }
        // bone and notetrack names: the zone's own string table, decrypted (the base's SlotString
        // reaches the zone served, opened or resumed)
        VirtualGameSession::Handlers Handlers() override
        {
            static std::once_flag Setup;
            std::call_once(Setup, []() { GameBlackOpsCW::PerformInitialSetup(); });
            if (!SlotsRead)
            {
                Slots = ZoneStrings(Zone());
                SlotsRead = true;
            }
            return { SlotString, GameBlackOpsCW::LoadXImage };
        }

        // the 60-bit hash at the asset's first field (an XAnim's at 0x70: its first field is 0),
        // named from the index when known
        std::string AssetName(Kind K, int64_t Pos) const override
        {
            const int64_t At0 = K == Kind::Anim ? (int64_t)offsetof(BOCWXAnim, NamePtr) : 0;
            const uint64_t Hash = At<uint64_t>(Image(), Pos + At0) & HashMask;
            const auto Found = GameBlackOpsCW::AssetNameCache.NameDatabase.find(Hash);
            // fnv1a_xanims.wni holds 8 strings that only collide with an anim's hash (material and
            // sound paths, "mc/mtl_uicon_/env/env_rain_drainage...rr75_grating_frame_o"): an anim
            // name is letters, digits and '_', anything else is not its name
            const auto AnimName = [](const std::string& S) {
                return std::all_of(S.begin(), S.end(), [](unsigned char C) { return std::isalnum(C) || C == '_'; });
            };
            if (Found != GameBlackOpsCW::AssetNameCache.NameDatabase.end() && (K != Kind::Anim || AnimName(Found->second)))
                return Found->second;
            const char* Prefix = K == Kind::Model ? "xmodel" : K == Kind::Anim ? "xanim" : K == Kind::Image ? "ximage"
                               : K == Kind::Script ? "script" : K == Kind::Fx ? "fx" : K == Kind::Rawfile ? "rawfile"
                               : K == Kind::Data ? "stringtable" : K == Kind::Sound ? "xsound" : "asset";
            return std::format("{}_{:x}", Prefix, (unsigned long long)Hash);
        }
        // a name-only header (a model with no skeleton and no LOD): another zone defines it
        bool IsReference(Kind K, const std::string& Name, int64_t Pos) const override
        {
            if (K == Kind::Model)
                return At<uint64_t>(Image(), Pos + offsetof(BOCWXModel, XSkeletonPtr)) == 0 &&
                       At<uint64_t>(Image(), Pos + offsetof(BOCWXModel, ModelLodPtrs)) == 0;
            // an anim with no bones, no data and no streamkey: only its name (4 745 of mp_common's 5 790)
            if (K == Kind::Anim)
                return At<uint64_t>(Image(), Pos + offsetof(BOCWXAnim, BoneIDsPtr)) == 0 &&
                       At<uint64_t>(Image(), Pos + offsetof(BOCWXAnim, DataBytePtr)) == 0 &&
                       At<uint64_t>(Image(), Pos + offsetof(BOCWXAnim, StreamKeyPtr)) == 0 &&
                       At<uint16_t>(Image(), Pos + offsetof(BOCWXAnim, TotalBoneCount)) == 0;
            if (K == Kind::Script) return Off(PointerAt(Pos + 8)) < 0 || At<uint32_t>(Image(), Pos + 0x10) == 0;
            // a rawfile or a string table whose data another zone holds
            if (K == Kind::Rawfile) return Name.empty() || (At<uint32_t>(Image(), Pos + 8) != 0 && Off(PointerAt(Pos + 0x10)) < 0);
            if (K == Kind::Data)
                return Name.empty() || ((uint64_t)At<uint32_t>(Image(), Pos + 8) * At<uint32_t>(Image(), Pos + 0xc) != 0 && Off(PointerAt(Pos + 0x18)) < 0);
            return Name.empty();
        }

        bool ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models) override;
        // the entitylist as the map_ents text ("" without one), a brush entity naming its brush model ("*N")
        std::string EntityString();
        // Each image of the zone with mip levels (a name-only header has none), named as the
        // materials name it, decoded by the live reader; identical names written once
        void ExportImages(const Context& C, Totals& T)
        {
            ImageList L;
            for (const auto& I : Walk().Instances)
                if (OwnerIs(I, "image") && At<uint8_t>(Image(), I.Pos + offsetof(BOCWGfxImage, GfxMipMaps)) != 0) L.Order.push_back(I.Pos);
            L.KeyOf = [this](int64_t Pos, const std::string& Extension) {
                ExportWriters::ContentKey Key;
                return Key.Mix(At<uint64_t>(Image(), Pos) & HashMask).Mix(Extension).Key();
            };
            L.Decode = [this](int64_t Pos, const std::string& Name) {
                return GameBlackOpsCW::LoadXImage(XImage_t(ImageUsageType::DiffuseMap, 0, Live().Addr((uint64_t)Pos), Name));
            };
            L.OnePerName = true;
            ExportImageList(C, T, L);
        }

        // Each sound_asset (0x58: frame count u32 8, rate index u8 0xc, channels u8 0xd, format u8 0xf)
        // with data: a package key at 0x38 or a streamkey at 0x40 (none: a name-only header, another
        // zone defines it), named by its 60-bit hash. Format 13 is Opus (the live ReadXSound); 11 is
        // an "ADM3" object (rev/ vehicle engine models, frame count 1), not audio: not written.
        void ExportSounds(const Context& C, Totals& T)
        {
            constexpr int64_t Frames = 8, Format = 0xf, PackageKey = 0x38, StreamKey = 0x40;
            constexpr uint8_t Opus = 13;
            std::vector<IwctxReaders::SoundEntry> Found;
            for (const auto& I : Walk().Instances)
            {
                if (!OwnerIs(I, "sound_asset")) continue;
                if (At<uint64_t>(Image(), I.Pos + PackageKey) == 0 && Off(PointerAt(I.Pos + StreamKey)) < 0)
                {
                    Counter("sounds_references")++;
                    continue;
                }
                if (At<uint8_t>(Image(), I.Pos + Format) != Opus)
                {
                    Counter(std::format("sounds_format_{}_unread", At<uint8_t>(Image(), I.Pos + Format)))++;
                    continue;
                }
                Found.push_back({ AssetName(Kind::Sound, I.Pos), Live().Addr((uint64_t)I.Pos), 0, At<uint32_t>(Image(), I.Pos + Frames) });
                if (Found.back().Full.rfind("xsound_", 0) != 0) Counter("sounds_named")++;
            }
            IwctxReaders::ExportSounds(C, T, Name(), Found);
        }

        // with the images it holds (the install index then finds a map's textures in the zone
        // defining them)
        void Defines(std::vector<DiskGame::AssetId>& Out) const override
        {
            ZonekitZone::Defines(Out);
            for (const auto& I : Walk().Instances)
                if (OwnerIs(I, "image") && At<uint8_t>(Image(), I.Pos + offsetof(BOCWGfxImage, GfxMipMaps)) != 0)
                    Out.push_back({ Kind::Image, AssetName(Kind::Image, I.Pos) });
        }
        std::string IndexKey() const override { return ZonekitZone::IndexKey() + "+images1+anims1"; }

        // The array an instance read inline (an ARRAY row the walk does not link): its read of
        // Count records of Size bytes, -1 when none
        int64_t InlineArray(int64_t Pos, uint64_t Count, int64_t Size) const;
        // a streamkey's data from the .xsub packages (the key at the streamkey's 8), decompressed
        static bool ReadStreamed(uint64_t Key, std::vector<uint8_t>& Out)
        {
            if (Key == 0 || CoDAssets::GamePackageCache == nullptr) return false;
            uint32_t Size = 0;
            auto Data = CoDAssets::GamePackageCache->ExtractPackageObject(Key, Size);
            if (Data == nullptr || Size == 0) return false;
            Out.assign(Data.get(), Data.get() + Size);
            return true;
        }
        // An image's largest mip decoded by the live reader: its pixels without the DDS header
        bool ImagePixels(int64_t Pos, std::vector<uint8_t>& Out, uint32_t& Width, uint32_t& Height)
        {
            std::unique_ptr<XImageDDS> Dds;
            try
            {
                Dds = GameBlackOpsCW::LoadXImage(XImage_t(ImageUsageType::DiffuseMap, 0, Live().Addr((uint64_t)Pos), std::string()));
            }
            catch (...)
            {
                return false;
            }
            if (!Dds || Dds->DataBuffer == nullptr || Dds->DataSize < 148) return false;
            const auto* B = reinterpret_cast<const uint8_t*>(Dds->DataBuffer);
            std::memcpy(&Height, B + 12, 4);
            std::memcpy(&Width, B + 16, 4);
            const size_t Header = std::memcmp(B + 84, "DX10", 4) == 0 ? 148 : 128;
            Out.assign(B + Header, B + Dds->DataSize);
            return true;
        }
        // the district's static models, from its streamed data when the zone holds it
        void AddDistrictModels(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models, const std::vector<uint8_t>& Img,
                               int64_t Data, uint32_t Size, const std::unordered_map<uint64_t, uint64_t>& ModelsByHash);

    private:
        bool SlotsRead = false;

        // the string cipher's table, in the t9 profile's folder, read once (StringCipher.h)
        bool CipherTable(std::string& Error) const
        {
            const auto* Tables = ExportWriters::ZoneProfile(Profile(), Error);
            return Tables != nullptr && ColdWar::LoadStringCipher(Tables->Profile.Dir, Error);
        }

        // The zone's string table: the count at 0, then after the 0x28 header
        // one 8-byte slot per string, 0 or -1, and the text of each -1 slot in order, encrypted
        // (StringCipher.h); one the table cannot decrypt is left empty, counted
        std::vector<std::string> ZoneStrings(const std::vector<uint8_t>& Zone)
        {
            if (Zone.size() < 0x28) return {};
            const int32_t Count = At<int32_t>(Zone, 0);
            if (At<uint64_t>(Zone, 8) == 0 || Count <= 0 || 0x28 + (size_t)Count * 8 > Zone.size()) return {};
            auto Strings = InlineStrings(Zone, 0x28, (uint64_t)Count, 8, ~0ull);
            std::string Error;
            if (!CipherTable(Error))
            {
                Counter("strings_no_cipher_table") += Strings.size();
                return {};
            }
            for (auto& S : Strings)
                if (!ColdWar::DecryptString(S, S))
                {
                    Counter("strings_undecrypted")++;
                    S.clear();
                }
            return Strings;
        }
    };

    // The level (the t9 tables). GfxWorld (gfx_map, 0x1f28): its name string at 8, the surface
    // count at 0x10; GfxWorldDraw at 0xd0 (vertex count +0x10, positions +0x18 (12 bytes), vertex
    // data +0x38 (20 bytes: Treyarch::WorldVertices), index count +0x58, u16 indices +0x60); the
    // brush models at 0x178/0x180 (80 bytes: surface count at 24, first surface at 28); at 0x1b0 the
    // static part (surfaces +0x200, 0x60 each as BO4's: first vertex 12, triangle count 38, first
    // index 40, the material 0x48). ComWorld (com_map): primary lights (0x2b0 each) count 0xc,
    // array 0x10. clip_map: dynamic models u16 0x58, list 0x60 (0x60 each: the definition at 0,
    // a quaternion at 8, the origin at 24; the definition's model at 0xc8). Checked on
    // mp_sm_central's records.
    constexpr int64_t WorldName = 8, WorldSurfaceCount = 0x10, Draw = 0xd0, WorldBrushCount = 0x178, WorldBrushes = 0x180;
    constexpr int64_t WorldSurfaces = 0x1b0 + 0x200;
    constexpr int64_t ComLightCount = 0xc, ComLights = 0x10, LightSize = 0x2b0;
    constexpr int64_t ClipDynCount = 0x58, ClipDyns = 0x60, DynSize = 0x60, DynDefModel = 0xc8;
    // districts (0x60): the district count u64 at 0x40, the districts inline (0x88 each), each
    // with ten streamkey assets from 0x38, its static models' at 0x68. A streamkey (0x38): its
    // data at 0x20, u32 size at 0x30, in the zone when its flags (u8 0x37) have bit 2; else in the
    // .xsub packages, the package key at 8 (the zone file holds it; checked on every streamed
    // district of mp_echelon, zm_silver, wz_forest: each key is in the packages). The data is the
    // same whether in the zone or streamed: 0x100 bytes the game fills at load, then the record.
    constexpr int64_t DistrictCount = 0x40, DistrictSize = 0x88, DistrictStaticModels = 0x68;
    constexpr int64_t KeyPackage = 8, KeyData = 0x20, KeySize = 0x30;
    // The static models' data (measured on mp_sm_central, 3 490 models): the first model's number
    // over the map's districts u32 at 0x108 (district 1 of mp_echelon: 35 901, district 0's count),
    // the count u32 at 0x10c, then from 0x130 a 64-byte record per model (its 60-bit model hash at
    // 0x10, a u32 at 0x18, at 0x1c mostly the number of a district without static models, -1 none: zm_silver's
    // records hold 0-10 for its eleven such districts) and after them a 64-byte placement per model,
    // in the same order (a quaternion, the origin at 16, the scale at 28, its bounds, its own index
    // at 0x38: the record pairs with it when it is the record's number, checked on every record of
    // mp_sm_central, mp_echelon, zm_silver, wz_forest)
    constexpr int64_t SModelDataCount = 0x10c, SModelRecords = 0x130, SModelRecordSize = 64, SModelHash = 0x10;
    constexpr int64_t PlacementSize = 64, PlacementOrigin = 16, PlacementScale = 28, PlacementIndex = 0x38;
    // entitylist (0x18): the entity count u32 at 8, the entities inline (Treyarch::EntityText), an
    // entity's brush model (the clip map's partition, -1: none) i32 at 0x10 (Treyarch::EntityText)
    constexpr int64_t EntityCount = 8, EntitySize = 0x30;

    int64_t ColdWarZone::InlineArray(int64_t Pos, uint64_t Count, int64_t Size) const
    {
        for (const auto& I : Walk().Instances)
        {
            if (I.Pos != Pos) continue;
            for (uint32_t R = I.FirstRead; R < I.EndRead && R < Walk().Reads.size(); R++)
                if (Walk().Reads[R] == (int64_t)Count * Size) return Walk().ReadPos[R];
            break;
        }
        return -1;
    }

    void ColdWarZone::AddDistrictModels(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models, const std::vector<uint8_t>& Img,
                                        int64_t Data, uint32_t Size, const std::unordered_map<uint64_t, uint64_t>& ModelsByHash)
    {
        const uint32_t Count = At<uint32_t>(Img, Data + SModelDataCount);
        const int64_t Places = SModelRecords + (int64_t)Count * SModelRecordSize;
        if (Count == 0 || Places + (int64_t)Count * PlacementSize > (int64_t)Size)
        {
            Counter("map_districts_unread")++;
            return;
        }
        for (uint32_t K = 0; K < Count; K++)
        {
            const int64_t Rec = Data + SModelRecords + (int64_t)K * SModelRecordSize, Place = Data + Places + (int64_t)K * PlacementSize;
            if (At<uint32_t>(Img, Place + PlacementIndex) != K)
            {
                Counter("map_smodels_unread")++;
                continue;
            }
            const auto Found = ModelsByHash.find(At<uint64_t>(Img, Rec + SModelHash) & HashMask);
            if (Found == ModelsByHash.end())
            {
                Counter("map_smodels_not_in_zone")++;
                continue;
            }
            Maps::Instance In;
            In.Name = "smodel_" + std::to_string(Scene.StaticInstances.size());
            In.Model = ModelAt(Found->second);
            float Q[4];
            for (int J = 0; J < 4; J++) Q[J] = At<float>(Img, Place + J * 4);
            Maps::AxisFromQuat(Q, In.Axis);
            for (int J = 0; J < 3; J++) In.Origin[J] = At<float>(Img, Place + PlacementOrigin + J * 4);
            In.Scale = At<float>(Img, Place + PlacementScale);
            Scene.StaticInstances.push_back(In);
            Models.emplace(In.Model, Found->second);
        }
    }

    std::string ColdWarZone::EntityString()
    {
        const int64_t List = Root("entitylist");
        if (List < 0) return std::string();
        const auto& Img = Image();
        const uint32_t Count = At<uint32_t>(Img, List + EntityCount);
        Treyarch::EntityCounts N;
        size_t BrushModels = 0;
        std::string Text = Treyarch::EntityText(Img, InlineArray(List, Count, EntitySize), Count, [this](uint64_t A) { return Off(A); },
                                                [this](uint64_t A) { return StringAt(A); }, [this](uint64_t H) { return HashName(H); }, N,
                                                [&BrushModels](uint64_t, int32_t Model) {
                                                    if (Model < 0) return std::string();
                                                    BrushModels++;
                                                    return "*" + std::to_string(Model);
                                                });
        Counter("map_ents") += Count;
        Counter("map_ents_hashes") += N.Hashes;
        Counter("map_ents_hashes_named") += N.HashesNamed;
        Counter("map_ents_brush_models") += BrushModels;
        return Text;
    }

    bool ColdWarZone::ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models)
    {
        const int64_t World = Root("gfx_map");
        if (World < 0) return false;
        const auto& Img = Image();
        Scene.Name = StringAt(PointerAt(World + WorldName));
        if (Scene.Name.empty()) Scene.Name = Name();

        // ---- the world: brush model 0 is the level, the others its moving parts
        const uint32_t VertexCount = At<uint32_t>(Img, World + Draw + 0x10);
        const int64_t Positions = Off(PointerAt(World + Draw + 0x18)), Data = Off(PointerAt(World + Draw + 0x38));
        Maps::WorldLayout L;
        L.Image = &Img;
        L.Surfaces = Off(PointerAt(World + WorldSurfaces));
        L.SurfaceCount = (uint32_t)std::max(At<int32_t>(Img, World + WorldSurfaceCount), 0);
        L.Brushes = Off(PointerAt(World + WorldBrushes));
        L.BrushCount = (uint32_t)std::max(At<int32_t>(Img, World + WorldBrushCount), 0);
        L.Indices = Off(PointerAt(World + Draw + 0x60));
        L.IndexCount = At<uint32_t>(Img, World + Draw + 0x58);
        L.SurfaceSize = Treyarch::SurfaceSize, L.FirstVertex = 12, L.TriCount = 38, L.BaseIndex = 40, L.Material = Treyarch::SurfaceMaterial;
        L.BrushSize = 80, L.FirstSurface = 28, L.SurfaceCountAt = 24;
        if (FILE* Dbg = Maps::DebugFile())
            std::fprintf(Dbg, "[map] %s vertices %u at %llx/%llx indices %u at %llx surfaces %u at %llx brushes %u at %llx\n",
                         Scene.Name.c_str(), VertexCount, (long long)Positions, (long long)Data, L.IndexCount, (long long)L.Indices,
                         L.SurfaceCount, (long long)L.Surfaces, L.BrushCount, (long long)L.Brushes);
        const auto MaterialOf = [&](uint64_t Address) {
            return Off(Address) < 0 ? std::string() : Maps::AddMaterial(Scene, GameBlackOpsCW::ReadXMaterial(Address));
        };
        if (const size_t Bad = Maps::AddWorld(Scene, L, Treyarch::WorldVertices(Img, Positions, Data, VertexCount), MaterialOf))
            Counter("map_triangles_unread") += Bad;

        // ---- static models: in the districts' streamed data, by model hash among the zone's models
        std::unordered_map<uint64_t, uint64_t> ModelsByHash;
        for (const auto& I : Walk().Instances)
            if (OwnerIs(I, ModelType())) ModelsByHash.emplace(At<uint64_t>(Img, I.Pos) & HashMask, Live().Origin() + (uint64_t)I.Pos);
        if (const int64_t Districts = Root("districts"); Districts >= 0)
        {
            const uint64_t Count = At<uint64_t>(Img, Districts + DistrictCount);
            const int64_t Array = InlineArray(Districts, Count, DistrictSize);
            for (uint64_t K = 0; Array >= 0 && K < Count; K++)
            {
                const int64_t Key = Off(PointerAt(Array + (int64_t)K * DistrictSize + DistrictStaticModels));
                if (Key < 0) continue;   // a district without static models (Warzone's, zombies' collision districts)
                const int64_t Blob = Off(PointerAt(Key + KeyData));
                if (Blob >= 0)
                {
                    AddDistrictModels(Scene, Models, Img, Blob, At<uint32_t>(Img, Key + KeySize), ModelsByHash);
                    continue;
                }
                // its data is in the packages, not the zone: the package key at the streamkey's 8
                std::vector<uint8_t> Streamed;
                if (ReadStreamed(At<uint64_t>(Img, Key + KeyPackage), Streamed))
                {
                    Counter("map_districts_streamed_read")++;
                    const size_t Before = Scene.StaticInstances.size();
                    AddDistrictModels(Scene, Models, Streamed, 0, (uint32_t)Streamed.size(), ModelsByHash);
                    Counter("map_smodels_streamed") += Scene.StaticInstances.size() - Before;
                }
                else
                    Counter("map_districts_not_in_packages")++;
            }
        }

        Counter("map_smodels") += Scene.StaticInstances.size();

        // ---- dynamic models and primary lights
        if (const int64_t Clip = Root("clip_map"); Clip >= 0)
        {
            const uint16_t Count = At<uint16_t>(Img, Clip + ClipDynCount);
            const int64_t List = Off(PointerAt(Clip + ClipDyns));
            for (uint16_t K = 0; List >= 0 && K < Count; K++)
            {
                const int64_t E = List + (int64_t)K * DynSize;
                const int64_t Def = Off(PointerAt(E));
                const uint64_t Model = Def >= 0 ? PointerAt(Def + DynDefModel) : 0;
                const std::string Name = ModelAt(Model);
                if (Name.empty()) continue;
                Maps::Instance In;
                In.Name = "dynmodel_" + std::to_string(K);
                In.Model = Name;
                float Q[4];
                for (int J = 0; J < 4; J++) Q[J] = At<float>(Img, E + 8 + J * 4);
                Maps::AxisFromQuat(Q, In.Axis);
                for (int J = 0; J < 3; J++) In.Origin[J] = At<float>(Img, E + 24 + J * 4);
                Scene.DynamicInstances.push_back(In);
                Models.emplace(Name, Model);
            }
        }
        // ComPrimaryLight (0x2b0): type 64, origin 104, direction 116, colour 200, cosHalfFov 220, radius 228
        if (const int64_t Com = Root("com_map"); Com >= 0)
            Treyarch::Lights(Img, Off(PointerAt(Com + ComLights)), At<uint32_t>(Img, Com + ComLightCount), { LightSize, 200, 116, 220, 228, 104 }, Scene);

        // ---- the entities as the map_ents text, a brush entity naming its brush model ("*N")
        Scene.MapEnts = EntityString();

        // ---- the collision (ClipMap.h): the world's brushes and triangles, the brush models'
        if (const int64_t Clip = Root("clip_map"); Clip >= 0)
        {
            Maps::ClipSource Src;
            Src.Image = &Img;
            Src.Off = [this](uint64_t Address) { return Off(Address); };
            Src.String = [this](uint64_t Address) { return StringAt(Address); };
            std::map<std::string, size_t> Unread;
            ColdWar::ReadClipMap(Scene.Collision, Src, Clip, Unread);
            for (const auto& [Key, Value] : Unread) Counter("map_" + Key) += Value;
        }

        // ---- the terrain (Terrain.h): each surface's heightmap grid, drawn and as collision triangles
        // (after the clip map, which starts the collision afresh)
        if (const int64_t Terrain = Root("terraingfx"); Terrain >= 0)
        {
            ColdWar::TerrainSource Src;
            Src.Image = &Img;
            Src.Off = [this](uint64_t Address) { return Off(Address); };
            Src.Pixels = [this](int64_t Pos, std::vector<uint8_t>& Out, uint32_t& W, uint32_t& H) { return ImagePixels(Pos, Out, W, H); };
            Src.ImageName = [this](int64_t Pos) { return AssetName(Kind::Image, Pos); };
            std::map<std::string, size_t> Read;
            ColdWar::ReadTerrain(Scene, Src, Terrain, ColdWar::TerrainOptions(), Read);
            for (const auto& [Key, Value] : Read) Counter("map_" + Key) += Value;
        }
        return true;
    }

    class ColdWarGame final : public DiskGame::IDiskGame
    {
    public:
        const char* Id() const override { return "coldwar"; }
        const char* Title() const override { return "Call of Duty: Black Ops Cold War"; }
        const char* Engine() const override { return "T9"; }
        std::vector<std::string> Aliases() const override { return { "coldwar-disk-export" }; }
        std::unique_ptr<DiskGame::IDiskZone> Unopened() const override { return std::make_unique<ColdWarZone>(std::string()); }
        // the language's core_common strings, decrypted, their keys hashed as BO4's
        // (Treyarch::HashedLocalizeKey): multiplayer maps by mpui/ (mp_black_sea "Armada"),
        // Fireteam maps by mpui/ or presence/ (wz_duga "Duga"), a ww_ copy of a level by its own;
        // the zombies maps' zmui/ keys have no text in this build, the campaign's keys are not known
        const LevelNaming* LevelNames() const override
        {
            static const LevelNaming Naming = [] {
                LevelNaming N;
                N.From = { "??_core_common" };
                N.Keys = { { "mp_*", { "MPUI/{REST}", "MPUI/{NAME}", "PRESENCE/{NAME}" } },
                           { "wz_*", { "MPUI/{NAME}", "PRESENCE/{NAME}" } },
                           { "zm_*", { "ZMUI/{REST}", "PRESENCE/{NAME}" } },
                           { "*_mp_*", { "MPUI/{REST2}", "MPUI/MP_{REST2}", "PRESENCE/MP_{REST2}" } },
                           { "*_wz_*", { "MPUI/WZ_{REST2}", "PRESENCE/WZ_{REST2}" } } };
                N.Hashed = Treyarch::HashedLocalizeKey;
                return N;
            }();
            return &Naming;
        }

        // a .ff, a folder of them, or casc:<pattern> pulled from the install (--install) with the .fd patches
        bool Zones(const std::string& Input, const ExportOptions& Options, std::vector<std::string>& Out,
                   std::string& Error) const override
        {
            if (Input.rfind("casc:", 0) != 0) return IDiskGame::Zones(Input, Options, Out, Error);
            CoDPackageCache* P = Packages(Options, Error);
            if (P != nullptr) Out = TreyarchPackages::ExtractFastFiles(P, Input.substr(5), FastFileDir(Options), Error);
            if (P != nullptr && Out.empty()) Error = "no fast file matches " + Input + " " + Error;
            return !Out.empty();
        }

        // the fast files in CASC
        bool ListZones(const std::string& Install, const ExportOptions& Options, std::vector<std::string>& Names,
                       std::string& Error) const override
        {
            CoDPackageCache* P = TreyarchPackages::Open<XSUBCache>(Install);
            if (P != nullptr) Names = TreyarchPackages::ListFastFiles(P);
            if (Names.empty()) Error = "no fast file in " + Install;
            return !Names.empty();
        }

        // each picked fast file pulled from CASC (with its .fd patch) into the cache
        bool ResolveZones(const std::string& Install, const std::vector<std::string>& Names, const ExportOptions& Options,
                          std::vector<std::string>& Out, std::string& Error) const override
        {
            return TreyarchPackages::ExtractZones(TreyarchPackages::Open<XSUBCache>(Install), Install, Names, FastFileDir(Options), Out, Error);
        }

        std::unique_ptr<DiskGame::IDiskZone> Open(const std::string& Path, const DiskGame::ExportOptions& Options, std::string& Error) const override
        {
            // the packages the live readers stream mesh, image and sound data from (--install)
            if (!Options.Install.empty() && TreyarchPackages::Open<XSUBCache>(Options.Install) == nullptr)
            {
                Error = "could not open the packages of " + Options.Install;
                return nullptr;
            }
            auto Zone = std::make_unique<ColdWarZone>(Path);
            if (!Zone->Open(Error)) return nullptr;
            return Zone;
        }

        // the AI types, characters, anim maps and weapons of the zone, then of the zones loaded with
        // it (zm_common holds the zombies' AI types; core_common the weapons)
        std::vector<std::string> AnimOwnerDocuments() const override { return { TreyarchAnimOwners::DocumentName }; }
        void AnimOwners(const ExportOptions& Options, const std::vector<AnimZone>& Zones, ::AnimOwners::Book& Out) const override
        {
            static const TreyarchAnimOwners::Family Shared = { { "zm_*", { "zm_common", "core_common" } },
                                                               { "mp_*", { "mp_common", "core_common" } },
                                                               { "wz_*", { "wz_common", "core_common" } },
                                                               { "cp_*", { "cp_common", "core_common" } },
                                                               { "core_*", { "core_common" } } };
            TreyarchAnimOwners::Fill(*this, Options, Shared, Zones, Out);
        }

    private:
        static std::string FastFileDir(const ExportOptions& Options)
        {
            return !Options.Cache.empty() ? Options.Cache : (fs::temp_directory_path() / "greyhound_t9ff").string();
        }
        static CoDPackageCache* Packages(const ExportOptions& Options, std::string& Error)
        {
            CoDPackageCache* P = Options.Install.empty() ? nullptr : TreyarchPackages::Open<XSUBCache>(Options.Install);
            if (P == nullptr) Error = "casc: zones need --install";
            return P;
        }
    };

    const ColdWarGame Game;
    const DiskGame::Registration Registered(Game);
}
