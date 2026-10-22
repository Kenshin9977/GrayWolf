#include "stdafx.h"

// Modern Warfare III (2023) multiplayer on the disk template: a ZonekitZone (profile mwiii) served
// to the live readers (GameModernWarfare6.cpp), streamed data from the install's .xsub packages.

#include "Games/Common/MeshDecode.h"
#include "CoDAssets.h"
#include "DiskGame.h"
#include "Games/MWIII/TechsetFormat.h"
#include "ExportWriters.h"
#include "FileSystems.h"
#include "GameModernWarfare6.h"
#include "GameModernWarfare6Structures.h"
#include "ScriptExport.h"
#include "SettingsManager.h"
#include "Strings.h"

#include "HalfFloats.h"
#include "MapLive.h"

#include "Games/Common/StringCipher.h"
#include "Games/IW/IWZone.h"
#include "Games/IW/IwctxReaders.h"
#include "Games/IW/IwEntityKeys.h"
#include "Games/IW/IwWeaponStats.h"
#include "Games/IW/IwTerrain.h"
#include "Games/Treyarch/TreyarchPackages.h"
#include "WraithNameIndex.h"
#include "XSUBCache.h"

#include <cstring>
#include <filesystem>
#include <format>
#include <mutex>
#include <set>
#include <unordered_map>

namespace
{
    namespace fs = std::filesystem;

    using DiskGame::Context;
    using DiskGame::Kind;
    using DiskGame::Totals;

    constexpr uint64_t HashMask = 0x0FFFFFFFFFFFFFFFull;   // the names' hashes, as the index keeps them

    // XModel (0xe8): name 8, LOD count u8 0x12, skeleton 0x70, LODs (0x48: xmodelsurfs at 0, the
    // surfaces the game copies from it at 8) behind 0x98; XModelSurfs keeps its surfaces at 8
    constexpr int64_t ModelName = 8, ModelLodCount = 0x12, ModelPacked = 0x70, ModelLods = 0x98, LodSize = 0x48;
    // the campaign build's XModel (0x190, MW6XModel): its bone names at 0xc8, the LODs behind 0x110
    constexpr int64_t SpModelBones = 0xc8, SpModelLods = 0x110;
    constexpr int64_t LodModelSurfs = 0, LodSurfs = 8, ModelSurfsSurfs = 8;
    // GfxImage (0x40): hash 0, mip count u8 0x2e (24-byte records at 0x30), pixels 0x38 (u32 0x18 bytes)
    constexpr int64_t ImageMipCount = 0x2e, ImagePixels = 0x38, ImagePixelsSize = 0x18;

    // The zone's script strings: count u32 at 0, 8-byte slots from 0x20 when the list's pointer at 8
    // is inline; a -2 or -1 slot's text follows the slots, in slot order (0x3b035b0)
    std::vector<std::string> ScriptStrings(const std::vector<uint8_t>& Z)
    {
        std::vector<std::string> Out;
        if (Z.size() < 0x20) return Out;
        const uint32_t Count = ExportNaming::At<uint32_t>(Z, 0);
        const uint64_t List = ExportNaming::At<uint64_t>(Z, 8);
        const auto Inline = [](uint64_t V) { return V == ~0ull || V == ~1ull; };
        if (!Inline(List) || Count == 0 || 0x20 + (uint64_t)Count * 8 > Z.size()) return Out;
        Out.resize(Count);
        size_t Text = 0x20 + (size_t)Count * 8;
        for (uint32_t K = 0; K < Count && Text < Z.size(); K++)
        {
            if (!Inline(ExportNaming::At<uint64_t>(Z, 0x20 + (int64_t)K * 8))) continue;
            const auto End = std::find(Z.begin() + (std::ptrdiff_t)Text, Z.end(), 0);
            Out[K].assign(Z.begin() + (std::ptrdiff_t)Text, End);
            Text = (size_t)(End - Z.begin()) + 1;
        }
        return Out;
    }

    class MWIIIZone final : public IWZone
    {
    public:
        // Campaign: a zone of sp23\ (the older build's layouts, the mwiii_sp profile)
        MWIIIZone(const std::string& Path, bool Campaign) : IWZone(Path), Campaign(Campaign)
        {
            Exports(Kind::Image, [this](const Context& C, Totals& T) { ExportImages(C, T); });
            Exports(Kind::Sound, [this](const Context& C, Totals& T) { ExportSounds(C, T); });
            Exports(Kind::Map, [this](const Context& C, Totals& T) { ExportMap(C, T); });
            // gscobj {hash, u32 len 8, bytes 0x10}: a GSCOBJ acts decompiles, its string literals
            // decrypted with the dumped exe
            Exports(Kind::Script, [this](const Context& C, Totals& T) {
                constexpr Scripts::Vm Vm{ Scripts::Tool::Acts, "", "pc", "c", "cod23-cod_dump.exe" };
                ExportScripts(C, T, Vm, { { "gscobj", [this](int64_t Pos, Scripts::Script& S) {
                                  S.Compiled = true;
                                  return BytesAt(PointerAt(Pos + 0x10), At<uint32_t>(Image(), Pos + 8), S.Bytes);
                              } } });
            });
            // rawfiles, luafile (0x18) {hash, i32 size 8, buffer 0x10} and string tables, their
            // strings encrypted as the localized ones
            ExportsHashedRawfiles(8, 0x10);
            ExportsMaterialLinks(Campaign ? MaterialLinkLayout{ 0x18, 0x18, -1, 0x30 } : MaterialLinkLayout{ 0x18, 0x1b, 0x28, 0x30 });
            // soundbank (0x170): aliases 0x28 (u32 count 0x20) of 0x30 {entries 0, hash 8, u16 count
            // 0x18}, an entry 0x40 with its sndasset at 0x30
            ExportsSoundLinks({ 0x28, 0x20, 0x30, 0x8, 0x0, 0x18, 0x40, 0x30 });
            // the campaign build's layouts (material 0x70...) are not read
            if (!Campaign) ExportsTechsets(MWIII::TechsetFormat());
            const auto Decrypt = [this](const std::string& Raw) { return Decrypted(Raw); };
            ExportsColumnTables(ColumnTypes::Mwiii, Decrypt);
            ExportsAssetRefs(Decrypt);
            // the zone's entities, one TSV (IwctxReaders::ExportMapEnts): map_ents' pairs at 8 (AddEntities)
            Exports(Kind::MapEnts, [this](const Context& C, Totals& T) {
                std::vector<IwctxReaders::MapEntity> Ents;
                if (const int64_t Ents0 = Root("map_ents"); Ents0 >= 0)
                    IwctxReaders::EntityPairs(Image(), Ents0 + 8, [this](uint64_t A) { return Off(A); }, [this](int64_t P) { return PointerAt(P); },
                                              Ents, IwEntityKeys::Scheme::Fields);
                IwctxReaders::ExportMapEnts(C, T, Name(), Ents);
            });
            // scriptable (0xb8) and scriptablevariant (0x60): no key/value table, dumped by offset
            ExportsStructDumps(Kind::Scriptables, { { "scriptable" }, { "scriptablevariant" } }, Decrypt);
            // the weapon assets as struct dumps: a weapon's name at 8, its definition inline behind 0x20
            // (no weapontunables asset), an attachment's name at 8; the fields the weapon stats prove
            // named (StatsLayout; the campaign's build by offset only)
            {
                const auto& L = StatsLayout();
                const auto Named = [&](const char* Type) { return Campaign ? std::map<std::string, DumpField>{} : IwWeaponStats::DumpFields(L, Type); };
                ExportsStructDumps(Kind::Weapons,
                                   { { "weapon", 8, Named("weapon") }, { "attachment", 8, Named("attachment") }, { "weaponaccuracy" },
                                     { "weapontrigger" }, { "weaponblueprint" }, { "attachmentblueprint" }, { "animpkg", -1, Named("animpkg") },
                                     { "suit" } },
                                   Decrypt);
            }
            if (Campaign) return;
            // the weapon stats table: this zone's facts, then the table rebuilt from every zone's
            Exports(Kind::WeaponStats, [this](const Context& C, Totals& T) {
                IwWeaponStats::ZoneView V;
                V.Image = &Image();
                V.Walk = &Walk();
                V.Off = [this](uint64_t A) { return Off(A); };
                V.Text = [this](int64_t Field) {
                    const std::string S = TextAt(Field);
                    return S.empty() ? S : Decrypted(S);
                };
                V.LocalizeText = [this](int64_t Pos) {
                    std::string Key, Text;
                    LocalizeEntry(Pos, Key, Text);
                    return Text;
                };
                IwWeaponStats::Export(StatsLayout(), V, fs::path(FastFilePath()).stem().string(), C, T);
            });
            // scriptbundle (0x30): its table at 0x10 {u32 count 0, keys' FNV-1a 64 8, script identifier
            // hashes 0x10, entries {u8 type, value at 8} 0x18}. The types as getscriptbundle's converter
            // 0x315a600 hands them to script: 1 and 2 an int (two adders with the same code), 3 a float,
            // 4 and 5 a string record (a localized string's, a string's), 6 a table, 7 a keyed table, 8 an
            // array {u32 count 0, 16-byte typed entries at 8}, 9 and 10 a record whose first field is its
            // name's string record, 11, 13 and 15 a 64-bit hash, 12 a 32-bit one (raw), 0 and 14 nothing
            ScriptBundleLayout Bundles;
            Bundles.Count = 0, Bundles.Keys = 8, Bundles.ScriptHashes = 0x10, Bundles.Entries = 0x18;
            Bundles.ArrayType = -1, Bundles.ArrayElements = 8, Bundles.ArrayCount = 0, Bundles.ArrayStride = 0x10;
            Bundles.Types = { BundleValue::Raw,    BundleValue::Int,  BundleValue::Int,         BundleValue::Float, BundleValue::String,
                              BundleValue::String, BundleValue::Struct, BundleValue::KeyedStruct, BundleValue::Array, BundleValue::Anim,
                              BundleValue::Anim,   BundleValue::Hash, BundleValue::Raw,         BundleValue::Hash,  BundleValue::Raw,
                              BundleValue::Hash };
            ExportsScriptBundles(Bundles, Decrypt);
            // mapinfo (0xa0, 0x3af4520): the scriptbundle table at 0x80; string fields 8 to 0x28 and
            // 0x38, an image at 0x30 (8 the map, 0x10 its name's localize key, 0x18 its description's,
            // 0x20 its game types, 0x28 its loading image's name: their values on the 106 mapinfos of
            // code_post_gfx). Named by the IW asset hash of the map (Black Ops 7's HashIWAsset), the
            // hash at 0 of each
            ExportsBundleTrees(Kind::MapInfo,
                               { { "mapinfo", 0x80, -1,
                                   { { "map", 0x08 }, { "display_name", 0x10 }, { "description", 0x18 }, { "gametypes", 0x20 },
                                     { "loading_image_name", 0x28 }, { "0x38", 0x38 } },
                                   { { "loading_image", 0x30 } },
                                   [](const std::string& Name) { return IwctxReaders::AssetHash(Name); } } },
                               Bundles, Decrypt);
            // aitype (0xf8, 0x3ad9cc0): the scriptbundle table at 0xa0, a string at 0x30
            ExportsBundleTrees(Kind::AiTypes, { { "aitype", 0xa0, -1, { { "0x30", 0x30 } }, {}, nullptr } }, Bundles, Decrypt);
        }

        // the weapon stats' layout (Games/IW/IwWeaponStats.h)
        static const IwWeaponStats::Layout& StatsLayout();

        // a zone string's text, decrypted with the profile's cipher table when it is encrypted
        std::string Decrypted(const std::string& Raw) const
        {
            std::string Error, Text;
            const auto* Tables = ExportWriters::ZoneProfile(Profile(), Error);
            const auto* Cipher = Tables != nullptr ? StringCipher::Load(Tables->Profile.Dir, Error) : nullptr;
            return StringCipher::Decrypt(Cipher, Raw, Text) ? Text : Raw;
        }

    protected:
        const char* Profile() const override { return Campaign ? "mwiii_sp" : "mwiii"; }
        const ICoDGame& LiveGame() const override { return LiveGames::Get("MWIII"); }
        VirtualGameSession::Handlers Handlers() override
        {
            static std::once_flag Setup;
            // fnv1a_localize.wni: localize key names (FNV-1a 64 of the lower-case key, the asset-name hash)
            std::call_once(Setup, [] {
                GameModernWarfare6::PerformInitialSetup();
                CoDAssets::AssetNameCache.LoadIndex((std::filesystem::path(FileSystems::GetApplicationPath()) / "package_index" / "fnv1a_localize.wni").string());
            });
            return { SlotString, DiskImage };
        }
        // resolve_deref 0x3ad90d0: an asset's back reference names the slot holding its pointer
        bool DerefAssetPointers() const override { return true; }
        void Fixups() override
        {
            Served(true);
            Slots = ScriptStrings(Zone());
            Counter("lods_without_surfaces") += LinkLods();
        }

        // a model by its name (at 8), an image, an animation or a sound by the hash at 0
        std::string AssetName(Kind K, int64_t Pos) const override
        {
            if (K == Kind::Model)
            {
                const std::string N = StringAt(PointerAt(Pos + ModelName));
                if (!N.empty()) return Strings::Replace(N, "::", "_");
            }
            const char* Prefix = K == Kind::Model ? "xmodel" : K == Kind::Anim ? "xanim" : K == Kind::Image ? "ximage"
                               : K == Kind::Sound ? "xsound" : K == Kind::Script ? "script" : K == Kind::Rawfile ? "rawfile"
                               : K == Kind::Data ? "stringtable" : "asset";
            return CoDAssets::GetHashedName(Prefix, At<uint64_t>(Image(), Pos) & HashMask);
        }
        // a name-only header, defined in another zone: a model with neither skeleton nor LODs, an
        // image with no levels and no pixels
        bool IsReference(Kind K, const std::string& Name, int64_t Pos) const override
        {
            if (Name.empty()) return true;
            if (K == Kind::Model)
                return PointerAt(Pos + (Campaign ? SpModelBones : ModelPacked)) == 0 && PointerAt(Pos + (Campaign ? SpModelLods : ModelLods)) == 0;
            if (K == Kind::Image) return At<uint8_t>(Image(), Pos + ImageMipCount) == 0 && PointerAt(Pos + ImagePixels) == 0;
            return false;
        }
        void Defines(std::vector<DiskGame::AssetId>& Out) const override
        {
            ZonekitZone::Defines(Out);
            for (const auto& I : Walk().Instances)
                if (OwnerIs(I, "image"))
                {
                    const std::string N = AssetName(Kind::Image, I.Pos);
                    if (!IsReference(Kind::Image, N, I.Pos)) Out.push_back({ Kind::Image, N });
                }
        }
        std::string IndexKey() const override { return ZonekitZone::IndexKey() + "_images1"; }
        // localize (0x10): the key's hash at 0 (named when fnv1a_localize.wni knows it), the text at 8
        // encrypted with the string cipher (strings.cipher.txt, DecryptString 0x46a9c70)
        void LocalizeEntry(int64_t Pos, std::string& Key, std::string& Text) const override
        {
            const uint64_t Hash = At<uint64_t>(Image(), Pos) & HashMask;
            const auto Found = CoDAssets::AssetNameCache.NameDatabase.find(Hash);
            Key = Found != CoDAssets::AssetNameCache.NameDatabase.end() ? Found->second : std::format("localize_{:x}", Hash);
            std::string Error;
            const auto* Tables = ExportWriters::ZoneProfile(Profile(), Error);
            const auto* Cipher = Tables != nullptr ? StringCipher::Load(Tables->Profile.Dir, Error) : nullptr;
            if (!StringCipher::Decrypt(Cipher, StringAt(PointerAt(Pos + 8)), Text)) Text.clear();
        }
        bool ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models) override;
        // the live readers read the campaign build's layouts while one of its zones is served
        void Served(bool On) override
        {
            if (On) CoDAssets::GameFlags = Campaign ? SupportedGameFlags::SP : SupportedGameFlags::MP;
        }
        void AddEntities(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models);

    private:
        const bool Campaign;

        // a material's texture: the live reader over the served zone, its levels from the packages
        static std::unique_ptr<XImageDDS> DiskImage(const XImage_t& Image)
        {
            return ServedZone<MWIIIZone>() == nullptr ? nullptr : GameModernWarfare6::LoadXImage(Image);
        }

        // Each LOD's surface pointer as the game's load-time fixup writes it (lod.surfs =
        // modelSurfs->surfs), the LODs behind the model's pointer at 0x98
        size_t LinkLods()
        {
            size_t Unlinked = 0;
            for (const auto& I : Walk().Instances)
            {
                if (!OwnerIs(I, "xmodel")) continue;
                const int64_t Lods = Off(PointerAt(I.Pos + (Campaign ? SpModelLods : ModelLods)));
                if (Lods < 0) continue;
                const int64_t N = At<uint8_t>(Image(), I.Pos + ModelLodCount);
                for (int64_t Lod = 0; Lod < N; Lod++)
                {
                    const int64_t Base = Lods + Lod * LodSize;
                    const uint64_t ModelSurfs = PointerAt(Base + LodModelSurfs);
                    if (!Live().Contains(ModelSurfs)) continue;
                    const uint64_t Surfs = PointerAt(Off(ModelSurfs) + ModelSurfsSurfs);
                    if (!Live().Contains(Surfs))
                    {
                        Unlinked++;
                        continue;
                    }
                    Live().Link((uint64_t)(Base + LodSurfs), (uint64_t)Off(Surfs));
                }
            }
            return Unlinked;
        }

        void ExportImages(const Context& C, Totals& T)
        {
            ImageList L;
            for (const auto& I : Walk().Instances)
                if (OwnerIs(I, "image") && !IsReference(Kind::Image, AssetName(Kind::Image, I.Pos), I.Pos)) L.Order.push_back(I.Pos);
            // the header past its hash (format, sizes, the levels' keys), the in-zone pixels, the extension
            L.KeyOf = [this](int64_t Pos, const std::string& Extension) {
                ExportWriters::ContentKey Key;
                const auto& Img = Image();
                for (int64_t K = 8; K < ImagePixels && Pos + K < (int64_t)Img.size(); K++) Key.Mix(Img[(size_t)(Pos + K)]);
                const int64_t Pixels = Off(PointerAt(Pos + ImagePixels));
                const uint32_t Size = At<uint32_t>(Img, Pos + ImagePixelsSize);
                if (Pixels >= 0 && Pixels + (int64_t)Size <= (int64_t)Img.size())
                    for (uint32_t K = 0; K < Size; K++) Key.Mix(Img[(size_t)Pixels + K]);
                return Key.Mix(Extension).Key();
            };
            L.Decode = [this](int64_t Pos, const std::string& Name) {
                return GameModernWarfare6::LoadXImage(XImage_t(ImageUsageType::DiffuseMap, 0, Live().Addr((uint64_t)Pos), Name));
            };
            L.FailWhy = [this](int64_t Pos) { return std::string(Off(PointerAt(Pos + ImagePixels)) >= 0 ? "in zone" : "streamed"); };
            L.OnePerName = true;
            ExportImageList(C, T, L);
        }

        // Each sound asset (sndasset) through the live reader, its Opus data from the packages
        void ExportSounds(const Context& C, Totals& T)
        {
            auto& Kt = T.Kinds[Kind::Sound];
            const std::string Extension = ExportWriters::SoundExtension(ExportWriters::SoundFormatSetting());
            const std::string Dir = C.KindDir(Kind::Sound);
            std::vector<int64_t> Found;
            for (const auto& I : Walk().Instances)
                if (OwnerIs(I, "sndasset")) Found.push_back(I.Pos);
            std::set<std::string> Done;
            for (size_t N = 0; N < Found.size(); N++)
            {
                const int64_t Pos = Found[N];
                const std::string Full = AssetName(Kind::Sound, Pos);
                if (!C.Advance(N, Found.size(), Full)) break;
                if (!C.Wants(Kind::Sound, Full) || !Done.insert(Full).second) continue;
                const auto S = CoDAssets::GameInstance->Read<MW6SndAsset>(Live().Addr((uint64_t)Pos));
                if (S.StreamKey == 0 && S.StreamKeyEx == 0)
                {
                    Counter("sounds_without_data")++;
                    continue;
                }
                Kt.Seen++;
                CoDSound_t Sound;
                Sound.AssetName = ExportNaming::SafeFileName(fs::path(Full).stem().string());
                Sound.FullPath = fs::path(Full).parent_path().string();
                Sound.AssetPointer = Live().Addr((uint64_t)Pos);
                Sound.FrameRate = S.FrameRate;
                Sound.FrameCount = S.FrameCount;
                Sound.ChannelsCount = S.ChannelCount;
                Sound.IsFileEntry = false;
                std::string Crash;
                const int Code = ExportWriters::ExportSound(Sound, Dir, Extension, Crash);
                if (Code == static_cast<int>(ExportGameResult::Success)) Kt.Written++;
                else T.Fail(Name(), Kind::Sound, Full, "code " + std::to_string(Code) + " " + Crash);
            }
        }
    };

    // The weapon stats' layout: offsets read off mwiii_flat.exe's code (RVAs of cod23-cod_dump.exe),
    // its GSC builtins (jup hashes) and the weapon stats UI 0x6eecfc0 calling each getter, the
    // getters matched to Black Ops 6's by their code. No weapontunables asset: the weapon (0x578, the
    // table 0x14de7e50) and its definition (behind 0x20, the table 0x14de9370) hold the fields.
    const IwWeaponStats::Layout& MWIIIZone::StatsLayout()
    {
        static const IwWeaponStats::Layout L = [] {
            using F = IwWeaponStats::Field;
            constexpr auto Def = F::In::Def, Weapon = F::In::Weapon;
            IwWeaponStats::Layout L;
            // weapon: name 8, definition 0x20, display name key 0x40 (a string; hashed as the localize
            // keys are); 16 slots at 0x58 + slot * 0x10 {u32 count 0, attachment ** 8} (0x640d390)
            L.WeaponName = 8, L.WeaponDef = 0x20, L.WeaponDisplay = 0x40, L.Slots = 0x58, L.SlotList = 8, L.SlotCountAt = 0, L.SlotCount = 16;
            L.DisplayIsKey = true, L.StringIdMask = HashMask;
            L.LocalizeHash = [](const std::string& Key) {
                uint64_t H = 0xCBF29CE484222325ull;
                for (const char C : Key) H = (H ^ (uint8_t)std::tolower((unsigned char)C)) * 0x100000001B3ull;
                return H & HashMask;
            };
            // definition: class i32 0x39c (weaponclass, 0x640d890), base animation package 0xb8 (0x640e260)
            L.DefClass = 0x39c, L.DefAnimPkg = 0xb8;
            // weaponfiretime (0x6aa5cc0 -> 0x63fbbe0): definition 0x31c (0x334 under u8 0x3f3a and a player
            // state); weaponburstcount (0x640d6f0) 0x3d0, fire type i32 0x3c8 (0x640e5a0, an attachment's
            // +0x2c0 overrides it); weaponclipsize (0x63f9fe0) the weapon's 0x4d8; weaponstartammo
            // (0x64275a0) 0x5dc; weaponmaxammo (0x641d5c0 <- the table 0x64e0940 filled by 0x64e0320 from
            // 0x5fc; game-mode overrides by class, 0x64de2d0, not applied); ADS times the weapon's 0x4b8
            // (in) and 0x4bc (out) (0x63f2e70: 4 times, 0x4c8.. under u8 0x3f35); reload type 0x3f8
            // (0x6407230); getweaponexplosionradius (0x81f77b0 -> 0x63f09e0) 0x1adc, 0x1b74 when u8 0x27e1;
            // sprint-out scale 0x330 (0x63e8ba0, 0x344 under u8 0x3f3a)
            L.FireTime = F(0x31c, Def), L.BurstCount = F(0x3d0, Def), L.FireType = F(0x3c8, Def), L.ClipSize = F(0x4d8, Weapon),
            L.StartAmmo = F(0x5dc, Def), L.MaxAmmo = F(0x5fc, Def), L.AdsTime = F(0x4b8, Weapon), L.AdsOutTime = F(0x4bc, Weapon),
            L.ReloadType = F(0x3f8, Def), L.ExplosionRadius = F(0x1adc, Def), L.ExplosionRadiusAlt = F(0x1b74, Def), L.ExplosionAlt = F(0x27e1, Def),
            L.SprintOutScale = F(0x330, Def);
            // the damage table: records of 10 dwords at D 0x108 by profile (0x63fa700: i32 damages 0-0x10,
            // f32 ranges 0x14-0x20, as it reads the attachments' records; 0x180 for mode 1, 0x1f8 under u8
            // 0x27c); the hit location multipliers inline at D 0x2808, 22 f32 (0x6402500 returns it by
            // default, 0x2860 and the 0x2878.. sets under its flags; its caller 0x880f650 indexes it by
            // the hit location, x 4)
            L.Damage = F(0x108, Def), L.Hit = F(0x2808, Def);
            // attachment: name 8, clip scale 0x7f0 and override block 0x3a8 (i32 at 8; start ammo's at 4,
            // max ammo's at 0), explosion block 0x4b8 (i32 0, alternate 0x2c)
            L.AttachmentName = 8, L.AttClipScale = 0x7f0, L.AttClipOverride = 0x3a8, L.AttExplosion = 0x4b8, L.AttExplosionAlt = 0x2c;
            // animation package: block 0x10 (0x640e260); reload_empty 0x6c (0x6422650, 0x70 under trait
            // 0x2df), reload 0x6e (0x6424e80, 0x72 under trait 0x2dc), per-round start 0x84 (0x6424d80)
            // and end 0xac (0x6422a30, 0xb8 under a flag), sprint-out 0x2a2 (0x6427330, 0x292 under a
            // flag), super sprint-out 0x2a4 (0x6427c00)
            L.PkgBlock = 0x10, L.PkgReloadEmpty = 0x6c, L.PkgReload = 0x6e, L.PkgReloadStart = 0x84, L.PkgReloadEnd = 0xac, L.PkgSprintOut = 0x2a2,
            L.PkgSuperSprintOut = 0x2a4;
            // 0xffff is the package's "no time", as in Black Ops 7: left out (empty), not written as 65535
            L.PkgFfffIsNone = true;
            L.Title = "Modern Warfare III weapon stats";
            L.Columns =
                "\nA value its zone was not read for stays empty: the guns are in global_stream_mp, their animation\n"
                "packages in ingame_shared_mp, the display names in the language's global zones. Export those zones into\n"
                "the same --out (a .txt list of zone paths works) and the table is rebuilt from every facts file.\n\n"
                "Columns and units (RVAs of mwiii_flat.exe). This game keeps no weapontunables asset: one set per\n"
                "weapon, its fields in the weapon (W) and its definition (D, behind weapon 0x20).\n"
                "- fire_time_ms: D 0x31c (weaponfiretime 0x6aa5cc0 -> 0x63fbbe0; 0x334 in some states). rpm = 60000 /\n"
                "  fire_time_ms.\n"
                "- burst_count: D 0x3d0 (0x640d6f0). fire_type: D i32 0x3c8 (0x640e5a0; an attachment's +0x2c0\n"
                "  overrides it): 0 auto (weaponisauto), 2 burst, 5 beam (weaponisbeam), 1, 3, 4 semi-auto.\n"
                "- clip_size: W 0x4d8 (weaponclipsize 0x6a9fce0 -> 0x63f9fe0). start_ammo: D 0x5dc (weaponstartammo ->\n"
                "  0x64275a0). max_ammo: D 0x5fc (weaponmaxammo -> 0x641d5c0 reads the table 0x64e0320 fills; game-mode\n"
                "  overrides by class, 0x64de2d0, not applied).\n"
                "- reload_ms, reload_empty_ms: the base animation package (D 0xb8), u16 0x6e and 0x6c of its time block\n"
                "  (0x10; 0x6424e80, 0x6422650), before attachments and perks. A package time of 0xffff is none, as in\n"
                "  Black Ops 7 (its override search skips it): its cell, and the sprint-to-fire times computed from it,\n"
                "  stay empty (the base getter returns it unchanged).\n"
                "- reload_type: D 0x3f8 (0x6407230). For 1 (per round) the stats UI (0x6eecfc0) adds reload_start_ms\n"
                "  (u16 0x84, 0x6424d80) and reload_end_ms (u16 0xac, 0x6422a30).\n"
                "- sprint_to_fire_ms = max(1, (int)(u16 0x2a2 x D 0x330)) (0x6427330, 0x63e8ba0), super_sprint_to_fire_ms\n"
                "  the same with u16 0x2a4 (0x6427c00).\n"
                "- ads_time_ms: W 0x4b8 (aiming in), ads_out_time_ms: W 0x4bc (aiming out) (0x63f2e70 reads 4 times there).\n"
                "- explosion_radius_m: getweaponexplosionradius (0x81f77b0 -> 0x63f09e0): D 0x1adc, or 0x1b74 when u8\n"
                "  D 0x27e1 is set; an attachment's block 0x4b8 overrides it. Inches / 39.37. Empty when 0.\n"
                "- damage_max .. damage_min at range_*: D 0x108, the record of profile 0 (0x28: i32 damages 0-0x10, f32\n"
                "  ranges 0x14-0x20; 0x63fa700 returns the five damages, the ranges and the f32 0x24 the same way for an\n"
                "  attachment's record); i32 0 is the min, 0x10 the max (the record Modern Warfare 2019's min and max\n"
                "  damage builtins read). Ranges in inches / 39.37, a range of 0 unused. The mode 1 record (D 0x180),\n"
                "  the one under u8 D 0x27c (0x1f8), the other profiles and the attachments' records are not applied.\n"
                "- hit_*: 22 f32 inline at D 0x2808 (0x6402500 returns it when its flags are clear: 0x2860, 0x2878,\n"
                "  0x28d0, 0x28e8, 0x2940, 0x2958, 0x29b0 are the other sets; an attachment's 0x7c0 / 0x7c8 first: not\n"
                "  applied), indexed by the hit location (0x880f650: table + location x 4), in the family's order.\n"
                "- Not proven for this game (empty): muzzle velocity, movement scales and speeds, first raise, raise,\n"
                "  drop and reload add times, the tunables sets and tags.\n";
            L.AttachmentNotes =
                "\nweapon_attachments.csv: each weapon with one attachment (every slot's list, weapon 0x58 + slot *\n"
                "0x10 {u32 count, list at 8}, slot and index as 0x640d390 reads them). weapon_loadouts.csv: the lines of\n"
                "loadouts.txt in this folder, each loadout. How the attachments combine (the code):\n"
                "- clip_size: the i32 at 8 of an attachment's block 0x3a8 when >= 0, else (int)(clip x (1 + sum(attachment\n"
                "  0x7f0 - 1))) (0x63f9fe0).\n"
                "- The animation package overrides, the modifier table and the ADS overrides are not read for this game.\n";
            return L;
        }();
        return L;
    }

    // ---- the level (C2M's ModernWarfare6.cs, its offsets checked against the loaders' tables)
    // GfxWorld (gfx_map, 0x4fd8): name 8; GfxWorldSurfaces inline at 0xc0 (count u32 0, brush
    // models 0x58 (count u32 0x10, 56 bytes: first surface u32 48, end 52), 40-byte surfaces 0x70,
    // material slots 0x98, 128-byte vertex records 0xa8); GfxWorldDpvsStatic inline at 0x230.
    constexpr int64_t WorldName = 8, Surfs = 0xc0, Dpvs = 0x230;
    constexpr int64_t SurfCount = 0, BrushCount = 0x10, Brushes = 0x58, SurfArray = 0x70, SurfMaterials = 0x98, SurfData = 0xa8;
    constexpr int64_t BrushSize = 56, BrushFirst = 48, BrushEnd = 52;
    // A surface (40): first index u32 4, table u32 8, vertex record u32 12, material u32 16, triangle
    // count u16 20, table count u16 22, vertex count u16 24, packed index offset u32 36
    constexpr int64_t SurfSize = 40, SfFaceIndex = 4, SfTable = 8, SfData = 12, SfMaterial = 16, SfTris = 20, SfTables = 22,
                      SfVertices = 24, SfPacked = 36;
    // A vertex record (128): offset x y z and scale (floats), its transient zone u32 16, uv layers
    // u32 28, byte offsets in that zone's buffer: positions (21-bit packed u64) 40, normals 44,
    // colours 52, uvs 56
    constexpr int64_t DataSize = 128, DtZone = 16, DtLayers = 28, DtXyz = 40, DtNormals = 44, DtColors = 52, DtUvs = 56;
    // gfx_map_trzone (0x178): its number u32 0x14; buffer size u32 0x18, index count 0x1c, the
    // buffer 0x28, u16 indices 0x30, 40-byte index tables 0x38, packed indices 0x40
    constexpr int64_t TrNumber = 0x14, TrBufferSize = 0x18, TrIndexCount = 0x1c, TrBuffer = 0x28, TrIndices = 0x30, TrTables = 0x38,
                      TrPacked = 0x40;
    // GfxWorldDpvsStatic: model count u32 4 (16-byte models at 0x58), collection count u32 8
    // (16 bytes at 0x68: first i32, count i32, model u16), placement count u32 0xc (24 bytes at
    // 0xa8: origin 3 x i32 / 4096, rotation 4 x u16 over [-1, 1] x y z w, scale half 20)
    constexpr int64_t DpModelCount = 4, DpModels = 0x58, DpColCount = 8, DpCols = 0x68, DpInstCount = 0xc, DpInsts = 0xa8;

    // MapEnts' entities (map_ents, their pairs at 8, C2M's ModernWarfare5.ReadMapEnts): pair count u32
    // 0, key count u32 4, vector count u32 0xc, 4-byte pairs at 0x18, the keys' 64-bit hashes at 0x20,
    // vectors (3 floats) at 0x30, the string pool at 0x38. A pair is read as Black Ops 6's: {u16
    // offset, u8 low key bits << 4, u8 type << 4 | high key bits}; type 0 a string in the pool, 2 a
    // vector, 9 a new entity; the key an index into the hashes. A key is named by IwEntityKeys
    // (FNV-1a 64 of its text, acts' "fields" basis), else key_<hash>.
    void MWIIIZone::AddEntities(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models)
    {
        const int64_t Ents = Root("map_ents");
        if (Ents < 0) return;
        const auto& Img = Image();
        const int64_t G = Ents + 8;
        const uint32_t Count = At<uint32_t>(Img, G), Vectors = At<uint32_t>(Img, G + 0xc), PoolSize = At<uint32_t>(Img, G + 0x10);
        const int64_t Pairs = Off(PointerAt(G + 0x18)), Hashes = Off(PointerAt(G + 0x20)), Vecs = Off(PointerAt(G + 0x30)),
                      Pool = Off(PointerAt(G + 0x38));
        const uint32_t KeyCount = At<uint32_t>(Img, G + 4);
        if (Pairs < 0) return;
        std::string Text = "{\n";
        for (uint32_t K = 0; K < Count; K++)
        {
            const int64_t P = Pairs + (int64_t)K * 4;
            const uint16_t Offset = At<uint16_t>(Img, P);
            const uint8_t Lo = At<uint8_t>(Img, P + 2), Hi = At<uint8_t>(Img, P + 3);
            const uint32_t Type = Hi >> 4, Index = (Lo >> 4) | (uint32_t)(Hi & 15) << 4;
            if (Type == 9)
            {
                Text += "}\n{\n";
                continue;
            }
            const uint64_t Hash = Hashes >= 0 && Index < KeyCount ? At<uint64_t>(Img, Hashes + Index * 8) : 0;
            const std::string Key = IwEntityKeys::Name(Hash, IwEntityKeys::Scheme::Fields);
            std::string Value;
            if (Type == 0 && Pool >= 0 && Offset < PoolSize)
                Value = StringAt(Live().Addr((uint64_t)(Pool + Offset)));
            else if (Type == 2 && Vecs >= 0 && Offset < Vectors)
                Value = std::format("{} {} {}", At<float>(Img, Vecs + Offset * 12), At<float>(Img, Vecs + Offset * 12 + 4),
                                    At<float>(Img, Vecs + Offset * 12 + 8));
            else
                continue;
            Text += "\"" + Key + "\" \"" + Value + "\"\n";
        }
        Text += "}\n";
        Scene.MapEnts = Text;

        // the entities naming a model this zone holds whole (script_model...), placed by their
        // origin and angles
        std::vector<Maps::Entity> List;
        if (!Maps::ParseEntities(Text, List)) return;
        std::unordered_map<std::string, uint64_t> Held;
        for (const auto& I : Walk().Instances)
            if (OwnerIs(I, "xmodel"))
            {
                const std::string N = AssetName(Kind::Model, I.Pos);
                if (!IsReference(Kind::Model, N, I.Pos)) Held.emplace(N, Live().Addr((uint64_t)I.Pos));
            }
        size_t K = 0;
        for (const auto& E : List)
        {
            const auto M = E.find("model");
            if (M == E.end() || M->second.empty() || M->second[0] == '*') continue;
            const auto Found = Held.find(Strings::Replace(M->second, "::", "_"));
            if (Found == Held.end())
            {
                Counter("map_entity_models_elsewhere")++;
                continue;
            }
            Maps::Instance In;
            In.Name = "entity_" + std::to_string(K++);
            In.Model = Found->first;
            float Angles[3] = { 0, 0, 0 };
            Maps::EntityFloats(E, "origin", In.Origin, 3);
            if (Maps::EntityFloats(E, "angles", Angles, 3)) Maps::AnglesToAxis(Angles, In.Axis);
            Scene.DynamicInstances.push_back(In);
            Models.emplace(Found->first, Found->second);
        }
    }

    bool MWIIIZone::ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models)
    {
        const int64_t World = Root("gfx_map");
        if (World < 0) return false;
        const auto& Img = Image();
        const int64_t Size = (int64_t)Img.size();
        FILE* Dbg = Maps::DebugFile();
        Scene.Name = StringAt(PointerAt(World + WorldName));
        if (Scene.Name.empty()) Scene.Name = Name();

        // the transient zones here, by number
        struct TrZone
        {
            int64_t Buffer = -1, Indices = -1, Tables = -1, Packed = -1;
            uint32_t BufferSize = 0, IndexCount = 0;
        };
        std::unordered_map<uint32_t, TrZone> Zones;
        for (const auto& I : Walk().Instances)
        {
            if (!OwnerIs(I, "gfx_map_trzone")) continue;
            TrZone Z;
            Z.Buffer = Off(PointerAt(I.Pos + TrBuffer)), Z.Indices = Off(PointerAt(I.Pos + TrIndices));
            Z.Tables = Off(PointerAt(I.Pos + TrTables)), Z.Packed = Off(PointerAt(I.Pos + TrPacked));
            Z.BufferSize = At<uint32_t>(Img, I.Pos + TrBufferSize), Z.IndexCount = At<uint32_t>(Img, I.Pos + TrIndexCount);
            Zones[At<uint32_t>(Img, I.Pos + TrNumber)] = Z;
        }
        const int64_t S = World + Surfs;
        const uint32_t SurfaceCount = At<uint32_t>(Img, S + SurfCount);
        const int64_t Surfaces = Off(PointerAt(S + SurfArray)), Data = Off(PointerAt(S + SurfData));
        const int64_t Materials = Off(PointerAt(S + SurfMaterials)), BrushArray = Off(PointerAt(S + Brushes));
        const uint32_t Brushcount = At<uint32_t>(Img, S + BrushCount);
        if (Dbg)
            std::fprintf(Dbg, "[map] %s surfaces %u at %llx data %llx materials %llx brushes %u at %llx trzones %zu\n", Scene.Name.c_str(),
                         SurfaceCount, (long long)Surfaces, (long long)Data, (long long)Materials, Brushcount, (long long)BrushArray, Zones.size());
        if (Surfaces < 0 || Data < 0) return true;

        std::unordered_map<uint64_t, std::string> MaterialNames;
        const auto MaterialOf = [&](uint32_t Index) -> std::string {
            if (Materials < 0) return std::string();
            const uint64_t Address = PointerAt(Materials + (int64_t)Index * 8);
            auto Found = MaterialNames.find(Address);
            if (Found == MaterialNames.end())
                Found = MaterialNames.emplace(Address, Off(Address) < 0 ? std::string()
                                                                        : Maps::AddMaterial(Scene, GameModernWarfare6::ReadXMaterial(Address))).first;
            return Found->second;
        };
        // a vertex key: its surface in the high bits, its index in the surface in the low 16
        std::vector<int64_t> SurfData(SurfaceCount, -1);
        const Maps::WorldBuilder::Fetch Fetch = [&](uint32_t Key, Maps::Vertex& V) {
            const uint32_t Sf = Key >> 16, K = Key & 0xffff;
            if (Sf >= SurfaceCount || SurfData[Sf] < 0) return false;
            const int64_t D = SurfData[Sf];
            const auto Z = Zones.find(At<uint32_t>(Img, D + DtZone));
            if (Z == Zones.end() || Z->second.Buffer < 0) return false;
            const int64_t B = Z->second.Buffer, End = Z->second.BufferSize;
            const int64_t P = (int64_t)At<uint32_t>(Img, D + DtXyz) + K * 8;
            if (P + 8 > End) return false;
            const uint64_t Packed = At<uint64_t>(Img, B + P);
            const float Scale = At<float>(Img, D + 12);
            for (int A = 0; A < 3; A++) V.Pos[A] = (float)((Packed >> (21 * A)) & 0x1fffff) * Scale + At<float>(Img, D + A * 4);
            const int64_t N = (int64_t)At<uint32_t>(Img, D + DtNormals) + K * 4;
            if (N + 4 <= End)
            {
                const Vector3 Normal = MeshDecode::UnpackPackedQuat(At<uint32_t>(Img, B + N));
                V.Normal[0] = Normal.X, V.Normal[1] = Normal.Y, V.Normal[2] = Normal.Z;
            }
            const uint32_t Layers = std::max<uint32_t>(At<uint32_t>(Img, D + DtLayers), 1);
            const int64_t Uv = (int64_t)At<uint32_t>(Img, D + DtUvs) + K * 8 * Layers;
            if (Uv + 8 <= End)
            {
                V.Uv[0] = At<float>(Img, B + Uv);
                V.Uv[1] = At<float>(Img, B + Uv + 4);
            }
            const uint32_t Colors = At<uint32_t>(Img, D + DtColors);
            if (Colors != 0 && (int64_t)Colors + K * 4 + 4 <= End) std::memcpy(V.Color, Img.data() + B + Colors + K * 4, 4);
            return true;
        };
        // brush model 0 is the level, the others its movers (no brushes: every surface the level)
        const bool NoBrushes = BrushArray < 0 || Brushcount == 0;
        const auto Range = [&](uint32_t B) {
            if (NoBrushes) return std::make_pair(0u, SurfaceCount);
            const int64_t P = BrushArray + (int64_t)B * BrushSize;
            return std::make_pair(At<uint32_t>(Img, P + BrushFirst), At<uint32_t>(Img, P + BrushEnd));
        };
        Maps::BuildWorld(Scene, NoBrushes ? 1 : Brushcount, SurfaceCount, Range, Fetch, [&](uint32_t Sf, Maps::WorldBuilder& Mesh) {
            const int64_t P = Surfaces + (int64_t)Sf * SurfSize;
            const int64_t D = Data + (int64_t)At<uint32_t>(Img, P + SfData) * DataSize;
            if (D + DataSize > Size) return;
            SurfData[Sf] = D;
            const auto Z = Zones.find(At<uint32_t>(Img, D + DtZone));
            const uint16_t Tris = At<uint16_t>(Img, P + SfTris), Vertices = At<uint16_t>(Img, P + SfVertices);
            if (Z == Zones.end() || Z->second.Indices < 0 || Z->second.Tables < 0 || Z->second.Packed < 0)
            {
                Counter("map_surfaces_unread") += 1;
                return;
            }
            const std::string Material = MaterialOf(At<uint32_t>(Img, P + SfMaterial));
            const uint8_t* Tables = Img.data() + Z->second.Tables + (int64_t)At<int32_t>(Img, P + SfTable) * 40;
            const uint8_t* Packed = Img.data() + Z->second.Packed + At<uint32_t>(Img, P + SfPacked);
            const auto* Indices = reinterpret_cast<const uint16_t*>(Img.data() + Z->second.Indices + (int64_t)At<uint32_t>(Img, P + SfFaceIndex) * 2);
            for (uint32_t T = 0; T < Tris; T++)
            {
                uint32_t X[3] = {};
                MeshDecode::UnpackTable40Face(Tables, At<uint16_t>(Img, P + SfTables), Packed, Indices, T, X);
                if (X[0] >= Vertices || X[1] >= Vertices || X[2] >= Vertices ||
                    !Mesh.Triangle(Material, Sf << 16 | X[2], Sf << 16 | X[1], Sf << 16 | X[0]))
                    Counter("map_triangles_unread") += 1;
            }
        });

        // the static models: collections of placements of one model each
        const int64_t Dp = World + Dpvs;
        const uint32_t ModelCount = At<uint32_t>(Img, Dp + DpModelCount), CollectionCount = At<uint32_t>(Img, Dp + DpColCount);
        const uint32_t InstanceCount = At<uint32_t>(Img, Dp + DpInstCount);
        const int64_t SModels = Off(PointerAt(Dp + DpModels)), Collections = Off(PointerAt(Dp + DpCols)), Instances = Off(PointerAt(Dp + DpInsts));
        for (uint32_t K = 0; SModels >= 0 && Collections >= 0 && Instances >= 0 && K < CollectionCount; K++)
        {
            const int64_t Col = Collections + (int64_t)K * 16;
            const int32_t FirstInstance = At<int32_t>(Img, Col), Count = At<int32_t>(Img, Col + 4);
            const uint16_t Model = At<uint16_t>(Img, Col + 8);
            if (Model >= ModelCount || FirstInstance < 0 || Count < 0) continue;
            const uint64_t Address = PointerAt(SModels + (int64_t)Model * 16);
            const std::string ModelName = ModelAt(Address);
            if (ModelName.empty()) continue;
            Models.emplace(ModelName, Address);
            for (int64_t J = FirstInstance; J < (int64_t)FirstInstance + Count && J < (int64_t)InstanceCount; J++)
            {
                const int64_t P = Instances + J * 24;
                Maps::Instance In;
                In.Name = "smodel_" + std::to_string(J);
                In.Model = ModelName;
                for (int A = 0; A < 3; A++) In.Origin[A] = (float)At<int32_t>(Img, P + A * 4) / 4096.0f;
                float Q[4];
                for (int A = 0; A < 4; A++) Q[A] = (float)At<uint16_t>(Img, P + 12 + A * 2) / 65535.0f * 2.0f - 1.0f;
                Maps::AxisFromQuat(Q, In.Axis);
                In.Scale = HalfFloats::ToFloat(At<uint16_t>(Img, P + 20));
                Scene.StaticInstances.push_back(In);
            }
        }
        if (Dbg)
            std::fprintf(Dbg, "[map] static models %u collections %u instances %u placed %zu\n", ModelCount, CollectionCount, InstanceCount,
                         Scene.StaticInstances.size());

        // the terrain: vhmdata's heightmap (Games/IW/IwTerrain.h; the level 0 tile, in the zone)
        if (const int64_t Vhm = Root("vhmdata"); Vhm >= 0)
        {
            IwTerrain::Source Src;
            Src.Locate = [this](uint64_t A, uint64_t N) -> const uint8_t* {
                const int64_t P = Off(A);
                return P >= 0 && (uint64_t)P + N <= Image().size() ? Image().data() + P : nullptr;
            };
            std::map<std::string, size_t> Read;
            IwTerrain::Read(Scene, Src, Live().Addr((uint64_t)Vhm), 0, IwTerrain::ModernWarfare3, IwTerrain::Options(), Read);
            for (const auto& [K, N] : Read) Counter("map_" + K) += N;
        }

        // ComWorld's lights (com_map: count u32 0x6c, 0xc8-byte records at 0x70: type byte 0,
        // colour 28, direction 40, origin 64, radius 76, cosHalfFovOuter 104)
        LightLayout L;
        L.Count = 0x6c, L.Array = 0x70, L.Size = 0xc8, L.TypeAt = 0;
        L.Color = 28, L.Dir = 40, L.Origin = 64, L.Radius = 76, L.CosHalfFov = 104;
        AddPrimaryLights(Scene, L);
        AddEntities(Scene, Models);
        if (Dbg) std::fprintf(Dbg, "[map] meshes %zu lights %zu materials %zu\n", Scene.Objects.size(), Scene.Lights.size(), MaterialNames.size());
        return true;
    }

    // "mwiii" the multiplayer zones (cod23\*.ff), "mwiii_sp" the campaign's (sp23\*.ff: another
    // build, the mwiii_sp profile). A zone under sp23\ reads as the campaign's whichever is asked;
    // the game picks the install index (each indexes its own folder).
    class ModernWarfare3Game final : public DiskGame::IDiskGame
    {
    public:
        explicit ModernWarfare3Game(bool Campaign) : Campaign(Campaign) {}
        const char* Id() const override { return Campaign ? "mwiii_sp" : "mwiii"; }
        const char* Title() const override
        {
            return Campaign ? "Call of Duty: Modern Warfare III (2023) (campaign)" : "Call of Duty: Modern Warfare III (2023)";
        }
        const char* Engine() const override { return "MWIII"; }
        std::unique_ptr<DiskGame::IDiskZone> Unopened() const override { return std::make_unique<MWIIIZone>(std::string(), Campaign); }

        bool ListZones(const std::string& Install, const DiskGame::ExportOptions& Options, std::vector<std::string>& Names,
                       std::string& Error) const override
        {
            if (!IDiskGame::ListZones(Install, Options, Names, Error)) return false;
            const char* Folder = Campaign ? "sp23" : "cod23";
            std::erase_if(Names, [Folder](const std::string& N) { return Strings::ToLower(N).rfind(Folder, 0) != 0; });
            if (Names.empty()) Error = std::string("no ") + Folder + " fast file in " + Install;
            return !Names.empty();
        }
        // the index opens the zones that define shared assets: not the transient (_tr) zones
        // (14 000 of cod23's, 4 666 of sp23's), the localized or server ones, nor the techsets_
        // ones (no material there)
        bool IndexedZones(const std::string& Install, const DiskGame::ExportOptions& Options, std::vector<std::string>& Names,
                          std::string& Error) const override
        {
            if (!ListZones(Install, Options, Names, Error)) return false;
            std::erase_if(Names, [](const std::string& N) {
                const std::string F = Strings::ToLower(fs::path(N).filename().string());
                return F.ends_with("_tr.ff") || F.rfind("eng_", 0) == 0 || F.rfind("fra_", 0) == 0 || F.rfind("srv_", 0) == 0 ||
                       F.rfind("techsets_", 0) == 0;
            });
            return !Names.empty();
        }

        // its zones open on the index's workers: their decompression and walk share nothing, its
        // handlers and fixups run under the session lock, ResolveZones is serialized
        bool IndexInParallel() const override { return true; }

        std::unique_ptr<DiskGame::IDiskZone> Open(const std::string& Path, const DiskGame::ExportOptions& Options,
                                                  std::string& Error) const override
        {
            // the packages the live readers stream meshes, image levels and sounds from (--install)
            if (!Options.Install.empty() && TreyarchPackages::Open<XSUBCache>(Options.Install) == nullptr)
            {
                Error = "could not open the packages of " + Options.Install;
                return nullptr;
            }
            // a campaign zone: one under sp23\ (a copy elsewhere reads as multiplayer)
            const std::string Folder = Strings::ToLower(fs::path(Path).parent_path().filename().string());
            auto Zone = std::make_unique<MWIIIZone>(Path, Folder == "sp23");
            if (!Zone->Open(Error)) return nullptr;
            return Zone;
        }

    private:
        bool Campaign;
    };

    const ModernWarfare3Game Game(false), CampaignGame(true);
    const DiskGame::Registration Registered(Game), RegisteredCampaign(CampaignGame);
}
