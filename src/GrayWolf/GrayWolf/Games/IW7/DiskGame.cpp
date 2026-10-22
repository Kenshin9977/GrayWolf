#include "stdafx.h"

// Infinite Warfare (IW7) on the disk template: a ZonekitZone with the iw7 profile, served to the
// live Infinite Warfare readers (GameInfiniteWarfare.cpp). What is IW7's own: the streamed-image
// table in the .ff header, the LOD surfaces the game fills in at load time, in-zone pixels, and
// sounds in the zone's .sabl/.sabs banks.

#include "CoDAssets.h"
#include "CoDRawImageTranslator.h"
#include "Compression.h"
#include "Games/IW/IWSoundAliases.h"
#include "DiskGame.h"
#include "ExportNaming.h"
#include "ExportWriters.h"
#include "FileSystems.h"
#include "GameInfiniteWarfare.h"
#include "Games/IW/IWMaterialBooks.h"
#include "Games/IW/IWZone.h"
#include "Games/IW/IwWeaponDefs.h"
#include "Games/IW7/EntityKeys.h"
#include "Games/IW7/HavokShapes.h"
#include "MapRadiantWriter.h"
#include "PAKSupport.h"
#include "SABSupport.h"
#include "SettingsManager.h"
#include "Strings.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <tuple>
#include <unordered_map>

namespace fs = std::filesystem;

namespace
{
    // XModel (0x2e0): the LOD count at 0xb, six 0x40 LOD records from 0xe0, a LOD's surfaces at
    // +0x30; XModelSurfs keeps them at +8
    constexpr int64_t ModelSurfsSurfs = 8;
    IWZone::LodLayout ModelLods()
    {
        IWZone::LodLayout L;
        L.Lods = 0xe0, L.LodSize = 0x40, L.LodCount = 0xb, L.LodSurfs = 0x30;
        return L;
    }

    // GfxImage (0x70): DXGI format at 0x18, map type at 0x20, in-zone byte count at 0x28, width
    // and height at 0x30, streamed at 0x39, in-zone pixels at 0x40, four stream levels
    // {u16 width, u16 height, u32 size} from 0x48, the name at 0x68.
    constexpr int64_t ImageFormatAt = 0x18, ImageMapTypeAt = 0x20, ImageBytesAt = 0x28, ImageWidthAt = 0x30;
    constexpr int64_t ImageStreamedAt = 0x39, ImagePixelsAt = 0x40, ImageStreamsAt = 0x48;
    constexpr int64_t ImageNameAt = 0x68;
    constexpr uint8_t MapTypeCube = 5;
    constexpr int StreamLevels = 4;

    // Streamed data: the .ff header holds the shared-stream count at 0xc4 and the image-stream
    // count at 0xcc, then from 0xd0 that many 24-byte records {start, end, pak index in the low
    // 16 bits} (shared first) into assetfile<index>.pak / imagefile<index>.pak, four per streamed
    // image (its four levels) in the order the zone loads its streamed images. An empty range has
    // no data.
    struct StreamRecord
    {
        uint64_t Start = 0, End = 0, Pak = 0;
    };
    constexpr uint64_t SharedCountAt = 0xc4, ImageCountAt = 0xcc, RecordsAt = 0xd0;

    std::vector<StreamRecord> ReadImageStreams(const std::string& FastFilePath)
    {
        std::vector<StreamRecord> Out;
        std::ifstream F(FastFilePath, std::ios::binary);
        uint32_t Shared = 0, Images = 0;
        F.seekg(SharedCountAt);
        F.read(reinterpret_cast<char*>(&Shared), 4);
        F.seekg(ImageCountAt);
        F.read(reinterpret_cast<char*>(&Images), 4);
        if (!F || Shared > (1u << 20) || Images > (1u << 20)) return Out;
        Out.resize(Images);
        F.seekg((std::streamoff)(RecordsAt + (uint64_t)Shared * sizeof(StreamRecord)));
        F.read(reinterpret_cast<char*>(Out.data()), (std::streamsize)Images * sizeof(StreamRecord));
        if (!F) Out.clear();
        return Out;
    }

    // A level's bytes in a DXGI format TranslateBC writes, 0 for any other
    uint64_t TopLevelBytes(uint8_t Format, uint32_t Width, uint32_t Height)
    {
        const uint64_t Blocks = (uint64_t)((Width + 3) / 4) * ((Height + 3) / 4);
        switch (Format)
        {
        case 71: case 72: case 80: case 81: return Blocks * 8;
        case 74: case 75: case 77: case 78: case 83: case 84: case 95: case 96: case 98: case 99: return Blocks * 16;
        case 24: case 28: case 29: return (uint64_t)Width * Height * 4;
        case 61: case 62: return (uint64_t)Width * Height;
        default: return 0;
        }
    }

    // The shared stream (block 4) a level's zone keeps out of it: per streamed xmodelsurfs, a record
    // into assetfile<index>.pak holding a 0x8000-byte signed header, then the IWC compressor
    // ("\x02IWC", u64 size, then LZ4 blocks {u32 packed, u64 size} padded to 4) of a one-asset
    // stream: its XAsset {type, header} and the asset as the loader reads it.
    std::vector<uint8_t> ReadSharedRecord(const std::string& Pak, uint64_t Start, uint64_t End)
    {
        std::vector<uint8_t> Out;
        if (End <= Start || End - Start > (256u << 20)) return Out;
        std::ifstream F(Pak, std::ios::binary);
        std::vector<uint8_t> Raw((size_t)(End - Start));
        F.seekg((std::streamoff)Start);
        F.read(reinterpret_cast<char*>(Raw.data()), (std::streamsize)Raw.size());
        if (!F) return Out;
        constexpr uint32_t Compressor = 0x43574902;
        size_t At = 0;
        for (const size_t Candidate : { (size_t)0x8000, (size_t)0 })
            if (Candidate + 16 <= Raw.size() && ExportNaming::At<uint32_t>(Raw, (int64_t)Candidate) == Compressor) { At = Candidate + 16; break; }
        if (At == 0) return Out;
        const uint64_t Total = ExportNaming::At<uint64_t>(Raw, (int64_t)At - 12);
        if (Total > (512u << 20)) return Out;
        Out.reserve((size_t)Total);
        while (Out.size() < Total && At + 12 <= Raw.size())
        {
            const uint32_t Packed = ExportNaming::At<uint32_t>(Raw, (int64_t)At);
            const uint64_t Size = ExportNaming::At<uint64_t>(Raw, (int64_t)At + 4);
            At += 12;
            if (Packed == 0 || Size == 0 || At + Packed > Raw.size() || Out.size() + Size > Total) break;
            const size_t Was = Out.size();
            Out.resize(Was + (size_t)Size);
            if (Compression::DecompressLZ4Block((const int8_t*)Raw.data() + At, (int8_t*)Out.data() + Was, (int32_t)Packed, (int32_t)Size) != (uint32_t)Size)
                return {};
            At = (At + Packed + 3) & ~(size_t)3;
        }
        if (Out.size() != Total) Out.clear();
        return Out;
    }

    // The paks sit beside the base zones, a localized zone one folder down (french\fra_<zone>.ff)
    fs::path PakDir(const std::string& FastFilePath, const std::string& Pak)
    {
        const fs::path Dir = fs::path(FastFilePath).parent_path();
        std::error_code Ec;
        if (fs::exists(Dir / Pak, Ec)) return Dir;
        return Dir.parent_path();
    }

    // A name-only material header names a material of the zones the game loads first: the common
    // zones (common*, global*, code_pre_gfx, code_post_gfx and their patch_ zones) and each level's
    // techsets_ zone. Material (0x118): image count u8 at 0x30, image table at 0x40.
    IWMaterialBooks::Layout MaterialLayout()
    {
        IWMaterialBooks::Layout L;
        L.Profile = "iw7";
        L.ImageCount = 0x30, L.Images = 0x40, L.ImageName = ImageNameAt;
        return L;
    }
    constexpr const char* BookZones = "(patch_)?(techsets_[a-z0-9_]+|(common|global|code_pre_gfx|code_post_gfx)(_[a-z0-9_]+)?)[.]ff";

    // The weapon fields (IwWeaponDefs.h): WeaponCompleteDef (0x138, the weapon asset) and its WeaponDef
    // (0x1418, behind 8), at x64-zt's IW7 structs.hpp offsets (zonetool rebuilds these zones), the damage
    // stops read off the exe (iw7_flat: 0x746af6 picks damage 0x4d0, mid 0x8f0, 0x8f4, 0x8f8, min 0x8e8,
    // 0x742704 their ranges 0x908, 0x90c, 0x910, 0x914: damage up to the first range, each mid up to its
    // own (0: none), min beyond). weapType, fireType and weapClass are named by the exe's tables
    // (0x141ba8520, entries 0, 13, 26).
    const IwWeaponDefs::Layout& WeaponLayout()
    {
        static const IwWeaponDefs::Layout L = [] {
            IwWeaponDefs::Layout L;
            L.Name = 0, L.Def = 8, L.Display = 0x10;
            L.Types = { "none", "bullet", "grenade", "projectile", "riotshield", "script", "teleport",
                        "shield", "chargeshield", "locationselect", "rewind", "overdrive", "equipdeploy" };
            L.Classes = { "rifle", "sniper", "mg", "smg", "spread", "pistol", "grenade", "rocketlauncher",
                          "turret", "throwingknife", "non-player", "item", "beam", "ball", "none" };
            L.FireTypes = { "Full Auto", "Single Shot", "Burst", "Double Barrel", "Beam" };
            L.Fields = { { "adsTransInTime", 0xb4, false }, { "adsTransOutTime", 0xb8, false }, { "clipSize", 0xbc, false },
                         { "fireTime", 0x1dc }, { "weaponType", 0x22c }, { "weaponClass", 0x234 }, { "fireType", 0x244 },
                         { "startAmmo", 0x48c }, { "maxAmmo", 0x4a4 }, { "shotCount", 0x4a8 },
                         { "damage", 0x4d0 }, { "maxDamageRange", 0x908 }, { "damage2", 0x8f0 }, { "damageRange2", 0x90c },
                         { "damage3", 0x8f4 }, { "damageRange3", 0x910 }, { "damage4", 0x8f8 }, { "damageRange4", 0x914 },
                         { "minDamage", 0x8e8 },
                         { "explosionRadius", 0x670 }, { "explosionRadiusMin", 0x674 }, { "explosionInnerDamage", 0x67c },
                         { "explosionOuterDamage", 0x680 }, { "projectileSpeed", 0x690 }, { "locationDamageMultipliers", 0x940 } };
            // the animation package (WeaponDef 0x1c0, an anim_package asset) and its u16 timers at 0x10:
            // 0x744380 reads the time at the byte offset its callers pass (from 0x1d0 under a flag, the
            // package overrides of 0xe20 not applied); the offsets are x64-zt's WeaponAnimPackageStateTimers
            // fields, each read by its own caller: firstRaise 6 (0x73ad40), reload 0x26 (0x74d880, fast 0x28),
            // reloadEmpty 0x2c (0x74cce0, 0x2e), reloadAdd 0x30 (0x74c790, 0x32), reloadEmptyAdd 0x34
            // (0x74cb40, 0x36), reloadStart 0x38 (0x74d860), reloadEnd 0x42 (0x74ce30, 0x44), drop 0x46
            // (0x73a6e0), raise 0x48 (0x74c620), quickDrop 0x50 (0x74c5e0), quickRaise 0x52 (0x74c600),
            // sprintOut 0x5e (0x74ee80)
            L.AnimPkg = 0x1c0, L.AnimTimers = 0x10, L.AnimPkgType = "anim_package";
            L.AnimTimes = { { "firstRaiseTime", 0x6 }, { "reloadTime", 0x26 }, { "reloadEmptyTime", 0x2c }, { "reloadAddTime", 0x30 },
                            { "reloadEmptyAddTime", 0x34 }, { "reloadStartTime", 0x38 }, { "reloadEndTime", 0x42 }, { "dropTime", 0x46 },
                            { "raiseTime", 0x48 }, { "quickRaiseTime", 0x52 }, { "sprintOutTime", 0x5e } };
            L.Notes = IwWeaponDefs::GameNotes("Infinite Warfare", "french/fra_code_post_gfx", "global_core_mp",
                                              "the WeaponCompleteDef (the weapon asset: ADS times, clipSize) and its WeaponDef\n"
                                              "(behind 8), at the offsets of x64-zt's IW7 structs.hpp",
                                              "NV4 612 rpm, 30 rounds, 30 to 1600 inches then 23; KBAR-32 731 rpm;" "\n" "eRAD over 800 rpm, 30 rounds, 30-23-19");
            L.Notes.Source += "The damage is a step table (iw7_flat 0x746af6): damage_1 up to range_1_m, each next stop up to\n"
                              "its range (a range of 0: the stop is not used), the last one (minDamage) beyond: its range is\n"
                              "empty. Reload, raise, drop and sprint-out times: the weapon's animation package (WeaponDef\n"
                              "0x1c0), its u16 timers (package 0x10) at the offsets the exe's time getter (iw7_flat 0x744380)\n"
                              "is called with, named as x64-zt's WeaponAnimPackageStateTimers: firstRaise 6, reload 0x26,\n"
                              "reloadEmpty 0x2c, reloadAdd 0x30, reloadEmptyAdd 0x34, reloadStart 0x38, reloadEnd 0x42, drop\n"
                              "0x46, raise 0x48, quickRaise 0x52, sprintOut 0x5e (fast-reload perk variants and the\n"
                              "package overrides not applied; 0xffff, which the getter's override search skips (0x744524),\n"
                              "is no time: empty). A weapon naming a package another zone defines (global_core_mp's\n"
                              "guns: common_core_mp holds their packages) takes its times from that zone's facts, joined by the\n"
                              "package's name: export both into the same --out.\n"
                              "meleeDamage (0x4d8, 50 for every gun) is left out, not checked.\n";
            L.Notes.AnimPackages = true;
            return L;
        }();
        return L;
    }


    class IW7Zone final : public IWZone
    {
    public:
        IW7Zone(const std::string& Path, std::string CacheDir)
            : IWZone(Path), Books(MaterialLayout(), std::move(CacheDir), [Path] {
                  return IWMaterialBooks::ZonesMatching(IWMaterialBooks::InstallDir(Path, "common.ff"), BookZones);
              })
        {
            using namespace DiskGame;
            ExportsIwWeapons(WeaponLayout());
            Exports(Kind::Image, [this](const Context& C, Totals& T) { ExportImages(C, T); });
            Exports(Kind::Sound, [this](const Context& C, Totals& T) { ExportSounds(C, T); });
            Exports(Kind::Map, [this](const Context& C, Totals& T) { ExportMap(C, T); });
            ExportsScriptFiles(Scripts::GscToolBin("iw7"));
            ExportsIwRawfiles("lua_file");
            ExportsIwStringTables();
        }
        ~IW7Zone() override
        {
            if (IsServed()) GameInfiniteWarfare::ImagesElsewhere = nullptr;
        }

        // the material headers the common zones' book filled, and those it did not
        void Measures(DiskGame::Totals& T) const override
        {
            ZonekitZone::Measures(T);
            Books.Measure(T, "materials_from_common", "materials_in_no_common_zone");
        }

    protected:
        const char* Profile() const override { return "iw7"; }
        const ICoDGame& LiveGame() const override { return LiveGames::Get("IW7"); }
        VirtualGameSession::Handlers Handlers() override { return { SlotString, DiskImage }; }
        const char* AnimType() const override { return "xanimparts"; }
        const char* LocalizeType() const override { return "localize_entry"; }   // {text, key}, 0x10
        // a rawfile's name read in place (-1 or -2: the string follows), not a back reference: those
        // the relocation lands inside other strings ("sion" of a .vision), measured on the sample
        bool NameInPlace(DiskGame::Kind K, int64_t Pos) const override
        {
            if (K != DiskGame::Kind::Rawfile) return true;
            const uint64_t Raw = At<uint64_t>(Zone(), Pos);
            return Raw == ~0ull || Raw == ~1ull;
        }
        bool DerefAssetPointers() const override { return true; }   // resolve 0x9e9a20 reads the pointer it names
        bool LoadBytes(std::vector<uint8_t>& Zone, std::string& Error) override;   // with the shared stream
        int64_t LayoutOffset() override;   // after the shared stream
        bool WalkedToEnd() const override;
        void Fixups() override;
        void Served(bool On) override
        {
            if (!On)
            {
                GameInfiniteWarfare::ImagesElsewhere = nullptr;
                return;
            }
            GameInfiniteWarfare::ImagesElsewhere = [this](const std::string& Name, IWMaterialBooks::ImageList& Out) { return Books.Find(Name, Out); };
        }
        void Defines(std::vector<DiskGame::AssetId>& Out) const override;   // with the images
        bool ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models) override;
        void ReadCollision(Maps::MapScene& Scene);   // Havok's shapes as brushes and triangles
        // A model whose name pointer, a back reference into block 8, lands on no text: a few of a
        // level's (mp_flip: 9 of 251) once its block 8 is rebased; left out, counted
        const char* Unwritable(DiskGame::Kind K, int64_t Pos) const override
        {
            if (K != DiskGame::Kind::Model) return nullptr;
            const std::string N = AssetName(K, Pos);
            bool Text = !N.empty() && N.size() < 256;
            for (const char C : N) Text = Text && (unsigned char)C >= 32 && (unsigned char)C < 127;
            return Text ? nullptr : "models_name_unread";
        }

    private:
        static std::unique_ptr<XImageDDS> DiskImage(const XImage_t& Image);

        std::unique_ptr<XImageDDS> Decode(int64_t Pos, ImageUsageType Usage);
        uint64_t ContentKey(int64_t Pos, const std::string& Extension) const;
        void ExportImages(const DiskGame::Context& C, DiskGame::Totals& T);
        void ExportSounds(const DiskGame::Context& C, DiskGame::Totals& T);
        bool Holds(int64_t Pos) const;   // an image with pixels here (in the zone or streamed)
        void LinkShared(std::unordered_map<std::string, uint64_t>& SurfsByName);
        std::vector<uint8_t> SharedAsset(int64_t From, int64_t Length) const;
        int64_t ZoneEnd = 0;                                    // the zone's own bytes, before the shared stream
        std::vector<std::pair<int64_t, int64_t>> SharedAssets;  // appended one-asset streams: where, how long
        std::unordered_map<int64_t, std::array<StreamRecord, StreamLevels>> Levels;   // image position -> levels
        IWMaterialBooks::Lookup Books;
    };

    std::unique_ptr<XImageDDS> IW7Zone::DiskImage(const XImage_t& Image)
    {
        IW7Zone* Current = ServedZone<IW7Zone>();
        if (Current == nullptr || !Current->Live().Contains(Image.ImagePtr)) return nullptr;
        return Current->Decode((int64_t)(Image.ImagePtr - Current->Live().Origin()), Image.ImageUsage);
    }

    bool IW7Zone::Holds(int64_t Pos) const
    {
        if (Levels.count(Pos)) return true;
        return At<uint8_t>(Zone(), Pos + ImageStreamedAt) == 0 && At<uint32_t>(Zone(), Pos + ImageBytesAt) > 0 &&
               Off(PointerAt(Pos + ImagePixelsAt)) >= 0;
    }

    std::unique_ptr<XImageDDS> IW7Zone::Decode(int64_t Pos, ImageUsageType Usage)
    {
        const auto& Img = Image();
        const uint8_t Format = At<uint8_t>(Img, Pos + ImageFormatAt);
        const bool Cube = At<uint8_t>(Img, Pos + ImageMapTypeAt) == MapTypeCube;
        uint32_t Width = At<uint16_t>(Img, Pos + ImageWidthAt), Height = At<uint16_t>(Img, Pos + ImageWidthAt + 2);
        std::unique_ptr<uint8_t[]> Pixels;
        uint32_t Size = 0;
        if (const auto It = Levels.find(Pos); It != Levels.end())
        {
            // the largest level with data
            int Level = -1;
            for (int K = 0; K < StreamLevels; K++)
            {
                const StreamRecord& R = It->second[K];
                const int64_t L = Pos + ImageStreamsAt + K * 8;
                const uint32_t W = At<uint16_t>(Img, L), H = At<uint16_t>(Img, L + 2);
                if (R.End > R.Start && W * H > 0 && (Level < 0 || W > Width)) Level = K, Width = W, Height = H;
            }
            if (Level < 0) return nullptr;
            const StreamRecord& R = It->second[Level];
            const std::string Pak = std::format("imagefile{}.pak", (unsigned long long)(R.Pak & 0xffff));
            Pixels = PAKSupport::IWExtractImagePackage((PakDir(FastFilePath(), Pak) / Pak).string(), R.Start, R.End - R.Start, Size);
        }
        else
        {
            const int64_t From = Off(PointerAt(Pos + ImagePixelsAt));
            Size = At<uint32_t>(Img, Pos + ImageBytesAt);
            if (From < 0 || Size == 0 || From + Size > (int64_t)Img.size()) return nullptr;
            Pixels = std::make_unique<uint8_t[]>(Size);
            std::memcpy(Pixels.get(), Img.data() + From, Size);
        }
        if (Pixels == nullptr || Size == 0 || Width == 0 || Height == 0) return nullptr;
        // the top level's bytes: a cube holds six of them, anything else (a volume too) is written
        // as its first one
        const uint64_t Top = TopLevelBytes(Format, Width, Height);
        if (Top == 0 || Size < Top) return nullptr;
        auto Result = CoDRawImageTranslator::TranslateBC(Pixels, Size, Width, Height, Format, 1, Cube && Size >= 6 * Top);
        if (Result != nullptr && Usage == ImageUsageType::NormalMap && SettingsManager::GetSetting("patchnormals", "true") == "true")
            Result->ImagePatchType = ImagePatch::Normal_COD_NOG;
        else if (Result != nullptr && Usage == ImageUsageType::DiffuseMap && SettingsManager::GetSetting("patchcolor", "true") == "true")
            Result->ImagePatchType = ImagePatch::Color_StripAlpha;
        return Result;
    }

    // The shared stream's records appended after the zone, each a one-asset stream walked on its own
    // (LinkShared): the zone walk ends where the zone does.
    bool IW7Zone::LoadBytes(std::vector<uint8_t>& Zone, std::string& Error)
    {
        if (!ZonekitZone::LoadBytes(Zone, Error)) return false;
        ZoneEnd = (int64_t)Zone.size();
        std::ifstream F(FastFilePath(), std::ios::binary);
        uint32_t Shared = 0;
        F.seekg(SharedCountAt);
        F.read(reinterpret_cast<char*>(&Shared), 4);
        if (!F || Shared > (1u << 20)) return true;
        std::vector<StreamRecord> Records(Shared);
        F.seekg(RecordsAt);
        F.read(reinterpret_cast<char*>(Records.data()), (std::streamsize)Shared * sizeof(StreamRecord));
        if (!F) return true;
        for (const StreamRecord& R : Records)
        {
            const std::string Pak = std::format("assetfile{}.pak", (unsigned long long)(R.Pak & 0xffff));
            std::vector<uint8_t> One = ReadSharedRecord((PakDir(FastFilePath(), Pak) / Pak).string(), R.Start, R.End);
            if (One.size() <= 16) continue;
            SharedAssets.emplace_back((int64_t)Zone.size(), (int64_t)One.size());
            Zone.insert(Zone.end(), One.begin(), One.end());
        }
        return true;
    }

    // The shared stream's xmodelsurfs come into block 8 before the zone (0x9e78e0, from DB_LoadXFile
    // 0x9e7490): under push(8), per record of the .ff header an XAsset entry allocated (16 bytes)
    // and loaded by Load_XAsset, then every block's cursor aligned (0xa7c4c0: block 8 to 16). The
    // zone's own reads begin where that leaves block 8. Each record is walked here as that load
    // places it; the emulator agrees read for read on the 510 records of five zones, and the back
    // references then land on their targets (weapon names: global_cp 152/152).
    int64_t IW7Zone::LayoutOffset()
    {
        if (SharedAssets.empty()) return 0;
        std::string Error;
        const auto* P = ExportWriters::ZoneProfile(Profile(), Error);
        if (P == nullptr) return 0;
        constexpr int Block = 8;
        constexpr int64_t Align = 16;
        ZoneTables::StreamRules Rs = P->Profile.Rules;
        int64_t Cursor = 0;
        // GRAYWOLF_IW7_SHARED_DUMP=<dir>: each record written there as its one-asset zone, for
        // graywolf-tools' scripts/iw7_shared_records.py (the emulator's placement against this one)
        std::string DumpDir;
        {
            char* Env = nullptr;
            size_t Len = 0;
            if (_dupenv_s(&Env, &Len, "GRAYWOLF_IW7_SHARED_DUMP") == 0 && Env != nullptr) DumpDir = Env;
            free(Env);
        }
        size_t Record = 0;
        for (const auto& [From, Length] : SharedAssets)
        {
            if (!DumpDir.empty())
            {
                const auto One = SharedAsset(From, Length);
                (void)ExportWriters::WriteBytes(fs::path(DumpDir) / std::format("{}_{:04}.bin", fs::path(FastFilePath()).stem().string(), Record++),
                                          One.data(), One.size());
            }
            Rs.StartOffset = Cursor;
            const auto R = ZoneTables::WalkZone(SharedAsset(From, Length), P->Tables, P->Profile.TypeNames, Rs, true);
            const auto End = R.BlockEnds.find(Block);
            if (R.ParseFailed || R.Why != "walked to the end" || End == R.BlockEnds.end())
            {
                Counter("shared_layout_unread")++;
                return 0;
            }
            Cursor = (End->second + Align - 1) / Align * Align;
        }
        Counter("block8_base") = (size_t)Cursor;
        return Cursor;
    }

    // A shared record as a zone of one asset: an XAssetList naming one inline asset, then the record
    std::vector<uint8_t> IW7Zone::SharedAsset(int64_t From, int64_t Length) const
    {
        constexpr int64_t ListSize = 0x28;
        std::vector<uint8_t> One(ListSize, 0);
        One[0x10] = 1;
        std::memcpy(One.data() + 0x18, &Rules().Marker, 8);
        One.insert(One.end(), Zone().begin() + From, Zone().begin() + From + Length);
        return One;
    }

    bool IW7Zone::WalkedToEnd() const
    {
        if (SharedAssets.empty()) return ZonekitZone::WalkedToEnd();
        return Walk().Why == "stopped " + std::to_string((int64_t)Zone().size() - ZoneEnd) + " bytes short";
    }

    // Each appended xmodelsurfs stream walked as a zone of one asset (SharedAsset), its links moved to
    // where the record sits in the served zone
    void IW7Zone::LinkShared(std::unordered_map<std::string, uint64_t>& SurfsByName)
    {
        if (SharedAssets.empty()) return;
        std::string Error;
        const auto* P = ExportWriters::ZoneProfile(Profile(), Error);
        if (P == nullptr) return;
        constexpr int64_t ListSize = 0x28, Entry = 16;
        for (const auto& [From, Length] : SharedAssets)
        {
            const auto R = ZoneTables::WalkZone(SharedAsset(From, Length), P->Tables, P->Profile.TypeNames, P->Profile.Rules, true);
            if (R.ParseFailed || R.Why != "walked to the end")
            {
                Counter("shared_assets_unread")++;
                continue;
            }
            const int64_t Delta = From - ListSize;
            for (const auto& [Field, Target] : R.Links) Live().Link((uint64_t)(Field + Delta), (uint64_t)(Target + Delta));
            Counter("shared_assets")++;
            const int64_t Pos = From + Entry;
            if (At<int32_t>(Zone(), From) != 7) continue;   // xmodelsurfs
            const std::string Name = StringAt(PointerAt(Pos));
            const uint64_t Surfs = PointerAt(Pos + ModelSurfsSurfs);
            if (!Name.empty() && Live().Contains(Surfs)) SurfsByName.emplace(Name, Surfs);
        }
    }

    void IW7Zone::Fixups()
    {
        Served(true);   // opened: the zone the handlers reach
        // the script strings: count u32 at 0, marker-tested slots from 0x28, then their text
        const auto& Z = Zone();
        const uint32_t Count = At<uint32_t>(Z, 0);
        if (At<uint64_t>(Z, 8) == Rules().Marker && Count != 0 && Count <= Z.size() / 8)
            Slots = InlineStrings(Z, 0x28, Count, 8, Rules().Marker);

        // LOD surfaces: what the game's load-time fixup writes, from the relocated pointers. A
        // name-only xmodelsurfs header (",name") stands for the one of that name the zone loads
        // later (a level's models come before their surfaces): the game finds it by name.
        std::unordered_map<std::string, uint64_t> SurfsByName;
        for (const auto& I : Walk().Instances)
        {
            if (!OwnerIs(I, "xmodel_surfs")) continue;
            const std::string Name = StringAt(PointerAt(I.Pos));
            const uint64_t Surfs = PointerAt(I.Pos + ModelSurfsSurfs);
            if (!Name.empty() && Name[0] != ',' && Live().Contains(Surfs)) SurfsByName.emplace(Name, Surfs);
        }
        LinkShared(SurfsByName);
        LinkLodSurfaces(ModelLods(), [&](int64_t Header) -> uint64_t {
            const std::string Name = StringAt(PointerAt(Header));
            const auto It = Name.size() > 1 && Name[0] == ',' ? SurfsByName.find(Name.substr(1)) : SurfsByName.end();
            if (It == SurfsByName.end()) return 0;
            Counter("lod_surfs_by_name")++;
            return It->second;
        });

        // streamed images, in walk order, take the stream table's records four by four
        std::vector<int64_t> Streamed;
        for (const auto& I : Walk().Instances)
            if (OwnerIs(I, "image") && At<uint8_t>(Z, I.Pos + ImageStreamedAt) > 0) Streamed.push_back(I.Pos);
        const std::vector<StreamRecord> Table = ReadImageStreams(FastFilePath());
        Counter("images_streamed") += Streamed.size();
        if (Table.size() == Streamed.size() * StreamLevels)
        {
            for (size_t K = 0; K < Streamed.size(); K++)
                for (int L = 0; L < StreamLevels; L++) Levels[Streamed[K]][L] = Table[StreamLevels * K + L];
        }
        else
            Counter("image_tables_mismatched")++;
    }

    uint64_t IW7Zone::ContentKey(int64_t Pos, const std::string& Extension) const
    {
        // what the output file is made of: the stream records, or the in-zone pixels, with the
        // format and the extension
        uint64_t H = 0xcbf29ce484222325ull;
        const auto Mix = [&H](uint64_t V) { H = (H ^ V) * 0x100000001b3ull; };
        if (const auto It = Levels.find(Pos); It != Levels.end())
            for (const StreamRecord& R : It->second) { Mix(R.Pak); Mix(R.Start); Mix(R.End); }
        else
        {
            const int64_t Pixels = Off(PointerAt(Pos + ImagePixelsAt));
            const uint32_t Size = At<uint32_t>(Image(), Pos + ImageBytesAt);
            if (Pixels < 0 || Pixels + Size > (int64_t)Image().size()) return 0;
            for (uint32_t K = 0; K < Size; K++) Mix(Image()[(size_t)(Pixels + K)]);
            Mix(At<uint32_t>(Image(), Pos + ImageWidthAt));
        }
        Mix(At<uint8_t>(Image(), Pos + ImageFormatAt));
        for (const char Ch : Extension) Mix((uint8_t)Ch);
        return H ? H : 1;
    }

    void IW7Zone::ExportImages(const DiskGame::Context& C, DiskGame::Totals& T)
    {
        ImageList L;
        // streamed ones in pak order (the game lays its streamed data out to be read front to
        // back), then the in-zone ones
        for (const auto& I : Walk().Instances)
            if (OwnerIs(I, "image")) L.Order.push_back(I.Pos);
        const auto PakKey = [&](int64_t Pos) {
            const auto It = Levels.find(Pos);
            return It == Levels.end() ? std::make_tuple(~0ull, 0ull, Pos) : std::make_tuple(It->second[0].Pak, It->second[0].Start, Pos);
        };
        std::sort(L.Order.begin(), L.Order.end(), [&](int64_t A, int64_t B) { return PakKey(A) < PakKey(B); });
        L.NameAt = ImageNameAt;
        L.KeyOf = [this](int64_t Pos, const std::string& Extension) { return ContentKey(Pos, Extension); };
        L.Decode = [this](int64_t Pos, const std::string&) { return Decode(Pos, ImageUsageType::Unknown); };
        L.Held = [this](int64_t Pos) { return Holds(Pos); };
        L.FailWhy = [this](int64_t Pos) { return std::string(Levels.count(Pos) ? "streamed" : "in zone"); };
        L.OnePerName = true;
        L.NamesChecked = true;
        ExportImageList(C, T, L);
    }

    // The level (the iw7 tables): GfxWorld (gfxworld, 0x11a8) with GfxWorldDraw at 0xa8 (its u16
    // indices: count 0x1f0, array 0x1f8) and GfxWorldDpvsStatic at 0xba0 (0x30-byte surfaces at
    // +0x348: first vertex 4, vertex count u16 0xc, triangle count u16 0xe, first index 0x10,
    // material 0x18; static models 0xb8 each at +0x358, the model at 0x38), 60-byte brush models at
    // 0xa00 (count 0x9f8); the 44-byte vertices in the GfxWorld's transient zone
    // (gfxworld_transient_zone, 0x50: count 0xc, array 0x10); ComWorld's (comworld) primary lights
    // 0x90 each (count 0x14, array 0x18; colour 24, direction 36, origin 60, radius 72, cosine 76,
    // the definition's name 0x88); the dynamic entities in map_ents (counts 0x170, lists 0x178),
    // 0x70 each: the pose at 32 (quaternion) and 48 (origin), the model at 64. Checked on mp_flip's
    // records: an MP map's one transient zone holds every vertex its surfaces name.
    IWZone::MapLayout Level()
    {
        IWZone::MapLayout M;
        M.World = "gfxworld", M.Com = "comworld";
        M.VertexAsset = "gfxworld_transient_zone", M.VertexCount = 0xc, M.Vertices = 0x10;
        M.IndexCount = 0xa8 + 0x1f0, M.Indices = 0xa8 + 0x1f8;
        M.BrushCount = 0x9f8, M.Brushes = 0xa00, M.BrushSize = 60, M.BrushFirstSurface = 52, M.BrushSurfaceCount = 56;
        M.SModelCount = 0xba0, M.Surfaces = 0xba0 + 0x348, M.SModels = 0xba0 + 0x358, M.SModelSize = 0xb8, M.SModelModel = 0x38;
        M.SurfaceSize = 0x30, M.SurfaceMaterial = 0x18, M.SurfaceFirstVertex = 4, M.SurfaceTris = 0xe, M.SurfaceBaseIndex = 0x10;
        M.Clip = "map_ents", M.DynCounts = 0x170, M.DynLists = 0x178, M.DynSize = 0x70;
        M.DynPose.Quat = 32, M.DynPose.Origin = 48, M.DynPose.Model = 64;
        M.Lights.Count = 0x14, M.Lights.Array = 0x18, M.Lights.Size = 0x90;
        M.Lights.Color = 24, M.Lights.Dir = 36, M.Lights.Origin = 60, M.Lights.Radius = 72, M.Lights.CosHalfFov = 76, M.Lights.Def = 0x88;
        M.ImageNameAt = ImageNameAt;
        M.EntityKeys = IW7Keys::EntityKeys;
        return M;
    }

    bool IW7Zone::ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models)
    {
        if (!ReadLevel(Scene, Models, Level(), GameInfiniteWarfare::ReadXMaterial)) return false;
        ReadCollision(Scene);
        Maps::TriggerLayout Triggers;   // MapEnts' MapTriggers at 0x18, its models 0x20 bytes
        Triggers.ModelSize = 0x20;
        AddTriggers(Scene, 0x18, "map_ents", Triggers);
        return true;
    }

    // The level's collision, Havok's shapes (HavokShapes.h): the world's in the clip map's packfile
    // (clipmap, 0x180: its size u32 0xf8, its bytes 0x100), the brush models' in MapEnts' (size
    // 0x150, bytes 0x158), each brush model (cmodel, 0x38: count u32 0x160, array 0x168) naming its
    // shape by a u16 at 0x30 (0xffff: none). Convex pieces are brushes, the world's triangles its
    // collision triangles, a brush model's prisms of its own.
    void IW7Zone::ReadCollision(Maps::MapScene& Scene)
    {
        const auto& Img = Image();
        std::map<std::string, size_t> Unread;
        const auto Shapes = [&](int64_t Asset, int64_t SizeAt, int64_t BytesAt, std::vector<IW7Havok::Shape>& Out) {
            const uint32_t Size = At<uint32_t>(Img, Asset + SizeAt);
            const int64_t Bytes = Off(At<uint64_t>(Img, Asset + BytesAt));
            if (Size == 0 || Bytes < 0 || Bytes + (int64_t)Size > (int64_t)Img.size()) return false;
            return IW7Havok::ReadShapeList(Img.data() + Bytes, Size, Out, Unread);
        };
        Maps::Collision& C = Scene.Collision;
        std::vector<IW7Havok::Shape> World, Entities;
        if (const int64_t Clip = Root("clipmap"); Clip >= 0 && Shapes(Clip, 0xf8, 0x100, World))
            for (IW7Havok::Shape& S : World)
            {
                C.ClipBrushes += S.Brushes.size();
                std::move(S.Brushes.begin(), S.Brushes.end(), std::back_inserter(C.Brushes));
                const uint32_t First = (uint32_t)(C.Vertices.size() / 3);
                C.Vertices.insert(C.Vertices.end(), S.Vertices.begin(), S.Vertices.end());
                for (const uint32_t V : S.Triangles) C.Triangles.push_back(First + V);
            }
        const int64_t Ents = Root("map_ents");
        if (Ents >= 0 && Shapes(Ents, 0x150, 0x158, Entities))
        {
            const uint32_t Count = At<uint32_t>(Img, Ents + 0x160);
            const int64_t Models = Off(At<uint64_t>(Img, Ents + 0x168));
            if (Models >= 0 && Count < 65536) C.Models.resize(Count);
            for (uint32_t M = 1; Models >= 0 && M < Count && M < 65536; M++)
            {
                const uint16_t Shape = At<uint16_t>(Img, Models + (int64_t)M * 0x38 + 0x30);
                if (Shape == 0xffff) continue;
                if (Shape >= Entities.size())
                {
                    Unread["havok_models_unread"]++;
                    continue;
                }
                const IW7Havok::Shape& S = Entities[Shape];
                std::vector<Maps::Brush>& Into = C.Models[M];
                C.ClipBrushes += S.Brushes.size();
                Into.insert(Into.end(), S.Brushes.begin(), S.Brushes.end());
                // its triangles as prisms 2 units deep, as the world's are written
                for (size_t K = 0; K + 2 < S.Triangles.size(); K += 3)
                {
                    std::array<double, 3> P[3];
                    for (int J = 0; J < 3; J++)
                        for (int A = 0; A < 3; A++) P[J][A] = S.Vertices[(size_t)S.Triangles[K + J] * 3 + A];
                    Maps::Brush B;
                    B.Contents = S.TriangleContents[K / 3];
                    if (Maps::TrianglePrism(P, 2.0, B))
                        Into.push_back(std::move(B));
                    else
                        Unread["havok_model_triangles_thin"]++;
                }
            }
        }
        for (const auto& [Key, N] : Unread) Counter("map_" + Key) += N;
    }

    void IW7Zone::Defines(std::vector<DiskGame::AssetId>& Out) const
    {
        ZonekitZone::Defines(Out);
        for (const auto& I : Walk().Instances)
        {
            if (!OwnerIs(I, "image") || !Holds(I.Pos)) continue;
            const std::string N = AssetName(DiskGame::Kind::Image, I.Pos + ImageNameAt);
            if (!IsReference(DiskGame::Kind::Image, N, I.Pos)) Out.push_back({ DiskGame::Kind::Image, N });
        }
    }

    // SndBank (0x160): alias lists at 0x28, their count at 0x20; SndAliasList (0x20): name 0, head
    // 0x10, count 0x18; SndAlias (200, iw7-mod's database.hpp, GPL-3.0): subtitle 8, secondary 0x10,
    // asset file name 0x20, volumes 0x48/0x4c, pitches 0x54/0x58, distances 0x60/0x64, flags 0x6c
    // (looping 0-2, type 7-8, channel 9-16), probability 0x7c, center 0x84, start delay 0x88,
    // envelope 0x90/0x94/0x98. A channel's name ends in _2D or _3D (SndChannel, 134 of them).
    IWSoundAliases::Layout AliasLayout()
    {
        IWSoundAliases::Layout L;
        L.ListHead = 0x10, L.ListCount = 0x18, L.ListCountBytes = 4, L.Size = 200;
        L.Subtitle = 8, L.Secondary = 0x10, L.VolMin = 0x48, L.VolMax = 0x4c, L.PitchMin = 0x54, L.PitchMax = 0x58;
        L.DistMin = 0x60, L.DistMax = 0x64, L.Flags = 0x6c, L.LoopingBits = 3, L.FlagsType = 7, L.ChannelShift = 9;
        L.ChannelPan = "332232323222222222222222222222233333333322233233333333333---333333333233333333333333222333222323332233333333333222233----32223333232--";
        L.Probability = 0x7c, L.CenterPercentage = 0x84, L.StartDelay = 0x88;
        L.EnvelopMin = 0x90, L.EnvelopMax = 0x94, L.EnvelopPercentage = 0x98;
        return L;
    }

    void IW7Zone::ExportSounds(const DiskGame::Context& C, DiskGame::Totals& T)
    {
        // the zone's own banks beside it: <zone>.sabl (loaded) and <zone>.sabs (streamed)
        std::vector<std::string> Banks;
        for (const char* Ext : { ".sabl", ".sabs" })
        {
            const fs::path Bank = fs::path(FastFilePath()).replace_extension(Ext);
            std::error_code Ec;
            if (fs::exists(Bank, Ec)) Banks.push_back(Bank.string());
        }
        ExportSoundBanks(Banks, C, T);
        if (!SoundAliasCsv::Wanted(C)) return;
        // an alias's asset file as ExportSoundBanks names it: under its folder, without its extensions
        const SoundAliasCsv::ZoneView Z = AliasView();
        const std::string Extension = ExportWriters::SoundExtension(ExportWriters::SoundFormatSetting());
        const IWSoundAliases::Layout L = AliasLayout();
        SoundAliasCsv::Rows Rows;
        for (const auto& I : Walk().Instances)
        {
            if (I.Parent >= 0 || !OwnerIs(I, "sound_bank")) continue;
            const uint32_t Count = At<uint32_t>(Image(), I.Pos + 0x20);
            const int64_t Lists = Off(PointerAt(I.Pos + 0x28));
            for (uint32_t K = 0; Lists >= 0 && K < Count; K++)
            {
                const int64_t List = Lists + (int64_t)K * 0x20;
                const int64_t Head = Off(PointerAt(List + 0x10));
                const auto Asset = [&](const std::string&, uint32_t J, int64_t) -> std::string {
                    const std::string F = Head < 0 ? std::string() : StringAt(PointerAt(Head + (int64_t)J * 200 + 0x20));
                    return F.empty() ? F : IWZone::SoundBankFile(F, Extension);
                };
                IWSoundAliases::Read(Z, List, StringAt(PointerAt(List)), L, Asset, Rows);
            }
        }
        SoundAliasCsv::Write(C, T, Name(), Rows);
    }

    class IW7Game final : public DiskGame::IDiskGame
    {
    public:
        const char* Id() const override { return "iw"; }
        const char* Title() const override { return "Call of Duty: Infinite Warfare"; }
        const char* Engine() const override { return "IW7"; }
        std::vector<std::string> Aliases() const override { return {}; }
        std::unique_ptr<DiskGame::IDiskZone> Unopened() const override { return std::make_unique<IW7Zone>(std::string(), std::string()); }
        // the rich presence strings of code_post_gfx, the language's own (fra_) after the
        // install's: PRESENCE_MP_BREAKNECK "Mayday", PRESENCE_CP_DISCO, PRESENCE_SP_EUROPA; the
        // campaign's other levels by their menu title
        const LevelNaming* LevelNames() const override
        {
            static const LevelNaming Naming{ { "code_post_gfx", "???_code_post_gfx" },
                                             { { "mp_*", { "PRESENCE_{NAME}", "LUA_MENU_MAPNAME_{REST}" } },
                                               { "cp_*", { "PRESENCE_{NAME}", "LUA_MENU_MP_{NAME}" } },
                                               { "shipcrib_*", { "MENU_SP_{NAME}_TITLE", "PRESENCE_SP_SC_{REST}" } },
                                               { "*", { "PRESENCE_SP_{NAME}", "MENU_SP_{NAME}_TITLE" } } } };
            return &Naming;
        }

        std::unique_ptr<DiskGame::IDiskZone> Open(const std::string& Path, const DiskGame::ExportOptions& Options, std::string& Error) const override
        {
            const std::string Cache = !Options.Cache.empty() ? Options.Cache : (fs::temp_directory_path() / "greyhound_iw7").string();
            auto Zone = std::make_unique<IW7Zone>(Path, (fs::path(Cache) / "iw7_materials").string());
            if (!Zone->Open(Error)) return nullptr;
            return Zone;
        }
    };

    const IW7Game Game;
    const DiskGame::Registration Registered(Game);
}
