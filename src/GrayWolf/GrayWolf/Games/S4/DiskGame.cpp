#include "stdafx.h"

// Vanguard (S4) on the disk template: a ZonekitZone with the s4 profile (wolff/profiles/s4, tables
// from the decrypted Vanguard.exe), its streamed data from the install's .xsub packages (XSUBCache,
// version 21). What reads (README.md beside this file): images, sounds, models with their materials,
// animations and the level (--type map).

#include "CoDAssets.h"
#include "CoDRawImageTranslator.h"
#include "DiskGame.h"
#include "ExportWriters.h"
#include "Games/IW/IWMaterialBooks.h"
#include "Games/IW/IWZone.h"
#include "Games/S4/S4Assets.h"
#include "Games/S4/S4Readers.h"
#include "Games/Common/MeshDecode.h"
#include "Games/Treyarch/TreyarchPackages.h"
#include "MapLive.h"
#include "MapScene.h"
#include "XSUBCache.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace
{
    namespace fs = std::filesystem;

    using DiskGame::Context;
    using DiskGame::Kind;
    using DiskGame::Totals;

    constexpr const char* S4Profile = "s4";

    // GfxImage (0x50, the s4 tables' image: 0x6c72ab0): name 0, flags u32 0x14, in-zone pixel size
    // u32 0x18, width u16 0x20, height u16 0x22, format u8 0x2a, mip record count u8 0x2e, the 24-byte
    // mip records at 0x38, the in-zone pixels at 0x48 (read when (flags >> 6) & 1 is 0: 0x6c72e30).
    // A mip record: {u64 .xsub key, u64, u32 cumulative size << 4, u16 width, u16 height}; with more
    // than one record the first holds its level and every smaller one, each padded to 16 bytes.
    // Proven on the sample's small zones (C:\tmp\vanguard\img_check.py): 20 752 of 20 757 levels'
    // sizes equal width x height in the format the table below gives (the 5 others: 123x123, 265x265).
    // Its depth u16 0x24 (and level count u16 0x26): the image description 0x42f31d5 copies width,
    // height, depth and levels from 0x20, 0x22, 0x24, 0x26 and the format from 0x2a; a grading CLUT's
    // in-zone size is width x height x depth x 8 (32 x 32 x 32, RGBA16: 262 144).
    constexpr int64_t ImageDepth = 0x24;
    constexpr int64_t ImageFlags = 0x14, ImagePixelsSize = 0x18, ImageWidth = 0x20, ImageHeight = 0x22, ImageFormat = 0x2a,
                      ImageMipCount = 0x2e, ImageMips = 0x38, ImagePixels = 0x48, ImageSize = 0x50;
    constexpr int64_t MipSize = 24;

    // The game's image format -> DXGI table (s4_flat.exe 0x7812a10, read by 0x265d0a0: movzx eax, cl;
    // mov eax, [table + rax*4]): 49 u32, the same values as Modern Warfare 2019's 0x73f2500
    constexpr uint8_t Dxgi[] = { 0,  61, 65, 61, 49, 49, 28, 29, 63, 51, 56, 35, 11, 58, 37, 13, 54, 34, 10, 41, 16, 2,  55, 40, 20,
                                 62, 57, 42, 17, 3,  25, 85, 24, 67, 26, 71, 72, 74, 75, 77, 78, 80, 83, 84, 95, 96, 98, 99, 31 };

    // Material (0x80, the s4 tables' 0x6c63950): image count u8 0x1c, the 16-byte slots {u32
    // semantic, image* 8} at 0x38, the techset at 0x30. A zone's materials may be name-only headers
    // (",name") that the techsets_ zones the game loads first define (S4Readers::ImagesElsewhere).
    IWMaterialBooks::Layout MaterialLayout()
    {
        IWMaterialBooks::Layout L;
        L.Profile = S4Profile;
        L.ImageCount = 0x1c, L.Images = 0x38, L.ImageName = 0, L.Techset = 0x30;
        return L;
    }

    // The techsets_ zones defining what a zone's headers name: the zone's own, then the common and
    // global ones, beside the .ff
    std::vector<fs::path> BookZones(const std::string& Path)
    {
        const fs::path Dir = fs::path(Path).parent_path();
        std::error_code Ec;
        std::vector<fs::path> Out;
        if (const fs::path Own = Dir / ("techsets_" + fs::path(Path).filename().string()); fs::exists(Own, Ec)) Out.push_back(Own);
        for (const fs::path& P : IWMaterialBooks::ZonesMatching(Dir, "techsets_(common|global)[a-z0-9_]*[.]ff")) Out.push_back(P);
        // the transient zones' (a weapon's, a body's: ",mo/mtl_c_s4_mp_rus_polina_pants_26") are
        // global_stream_mp's; then the other global zones
        for (const fs::path& P : IWMaterialBooks::ZonesMatching(Dir, "global_stream[a-z0-9_]*[.]ff")) Out.push_back(P);
        for (const fs::path& P : IWMaterialBooks::ZonesMatching(Dir, "global(_(mp|sp|cp|core_mp))?[.]ff")) Out.push_back(P);
        return Out;
    }

    class GameZone final : public IWZone
    {
    public:
        GameZone(const std::string& Path, std::string CacheDir)
            : IWZone(Path), Books(MaterialLayout(), std::move(CacheDir), [Path] { return BookZones(Path); })
        {
            Exports(Kind::Image, [this](const Context& C, Totals& T) { ExportImages(C, T); });
            Exports(Kind::Sound, [this](const Context& C, Totals& T) { ExportSounds(C, T); });
            Exports(Kind::Map, [this](const Context& C, Totals& T) { ExportMap(C, T); });
            // the models' records checked against the meshes before they are read (counters)
            if (const auto* Models = ExporterOf(Kind::Model))
                Exports(Kind::Model, [this, Read = *Models](const Context& C, Totals& T) {
                    CheckModels(C);
                    Read(C, T);
                });
        }

        bool ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models) override;

        void Measures(DiskGame::Totals& T) const override
        {
            IWZone::Measures(T);
            Books.Measure(T, "materials_from_techsets", "materials_in_no_techsets_zone");
        }

    protected:
        const char* Profile() const override { return S4Profile; }
        const ICoDGame& LiveGame() const override { return LiveGames::Get("S4"); }
        VirtualGameSession::Handlers Handlers() override { return { SlotString, DiskImage }; }
        // resolve_deref 0x5266690: an asset's back reference names the slot holding its pointer
        bool DerefAssetPointers() const override { return true; }
        void Fixups() override
        {
            Served(true);   // opened: the zone the handlers reach
            // the bone names and notetracks name the zone's script strings by slot, encrypted as Modern
            // Warfare 2019's (DecryptString 0x34658a0, the profile's strings.cipher.txt: 26 504 of 26 504
            // localize texts of the sample as the game's function gives them); one the table cannot
            // decrypt is left empty, named by its slot
            Slots = InlineScriptStrings(Zone());
            for (auto& S : Slots)
                if (std::string Text; CipherDecrypt(S, Text)) S = std::move(Text);
                else S.clear(), Counter("strings_not_decrypted")++;
        }
        // localize (0x10): the key at 0, the text at 8, encrypted with the same cipher
        void LocalizeEntry(int64_t Pos, std::string& Key, std::string& Text) const override
        {
            Key = StringAt(PointerAt(Pos));
            if (!CipherDecrypt(StringAt(PointerAt(Pos + 8)), Text)) Text.clear();
        }
        // its images too: a transient zone's textures are global_stream_mp's (the install index finds them)
        void DefinesAt(std::vector<std::pair<DiskGame::AssetId, int64_t>>& Out) const override
        {
            ZonekitZone::DefinesAt(Out);
            DefineImages(Out);
        }
        std::string IndexKey() const override { return ZonekitZone::IndexKey() + "_images1"; }
        void Served(bool On) override
        {
            ImagesServed.reset();
            if (On)
                ImagesServed.emplace(S4Readers::ImagesElsewhere,
                                     [this](const std::string& Name, IWMaterialBooks::ImageList& Out) { return Books.Find(Name, Out); });
        }

    private:
        IWMaterialBooks::Lookup Books;
        // the readers' lookup into Books while the zone is served (after Books: it ends first)
        std::optional<CoDAssets::Scoped<decltype(S4Readers::ImagesElsewhere)>> ImagesServed;

        // a material's texture, read by the image kind's reader over the served zone
        static std::unique_ptr<XImageDDS> DiskImage(const XImage_t& I)
        {
            auto* Z = ServedZone<GameZone>();
            if (Z == nullptr) return nullptr;
            const int64_t Pos = Z->Off(I.ImagePtr);
            return Pos < 0 ? nullptr : Z->LoadImage(Pos);
        }

        // ---- the model records (S4Assets.h) checked on this zone's instances: each fact a counter
        // (model_check_*), what does not hold counted beside it
        void CheckModels(const Context& C);

    private:
        // The largest level the install holds (its record index), -1 none; its bytes and size
        int BestMip(int64_t Pos, uint32_t& Size, uint16_t& W, uint16_t& H) const
        {
            const auto& Img = Image();
            const int Count = At<uint8_t>(Img, Pos + ImageMipCount);
            const int64_t Mips = Off(PointerAt(Pos + ImageMips));
            if (Count == 0 || Mips < 0 || Mips + Count * MipSize > (int64_t)Img.size() || CoDAssets::GamePackageCache == nullptr) return -1;
            for (int K = Count - 1; K >= 0; K--)
            {
                const int64_t R = Mips + K * MipSize;
                const uint64_t Key = At<uint64_t>(Img, R);
                if (Key == 0 || !CoDAssets::GamePackageCache->Exists(Key)) continue;
                const uint32_t End = At<uint32_t>(Img, R + 16) >> 4, Start = K == 0 ? 0 : At<uint32_t>(Img, R - MipSize + 16) >> 4;
                Size = End - Start;
                W = At<uint16_t>(Img, R + 20), H = At<uint16_t>(Img, R + 22);
                return K;
            }
            return -1;
        }

        bool InZone(int64_t Pos) const
        {
            return ((At<uint32_t>(Image(), Pos + ImageFlags) >> 6) & 1) == 0 && Off(PointerAt(Pos + ImagePixels)) >= 0;
        }

        std::unique_ptr<XImageDDS> LoadImage(int64_t Pos) const
        {
            const auto& Img = Image();
            const uint8_t Format = At<uint8_t>(Img, Pos + ImageFormat);
            if (Format >= std::size(Dxgi) || Dxgi[Format] == 0) return nullptr;
            std::unique_ptr<uint8_t[]> Data;
            uint32_t Size = 0;
            uint16_t W = 0, H = 0, D = 1;
            if (At<uint8_t>(Img, Pos + ImageMipCount) == 0)
            {
                if (!InZone(Pos)) return nullptr;
                const int64_t Pixels = Off(PointerAt(Pos + ImagePixels));
                Size = At<uint32_t>(Img, Pos + ImagePixelsSize);
                if (Size == 0 || Pixels + (int64_t)Size > (int64_t)Img.size()) return nullptr;
                Data = std::make_unique<uint8_t[]>(Size);
                std::memcpy(Data.get(), Img.data() + Pixels, Size);
                W = At<uint16_t>(Img, Pos + ImageWidth), H = At<uint16_t>(Img, Pos + ImageHeight);
                // a volume (the grading CLUTs: 32 x 32 x 32 of RGBA16, 262 144 bytes in the zone)
                D = std::max<uint16_t>(At<uint16_t>(Img, Pos + ImageDepth), 1);
            }
            else
            {
                const int K = BestMip(Pos, Size, W, H);
                if (K < 0) return nullptr;
                const uint64_t Key = At<uint64_t>(Img, Off(PointerAt(Pos + ImageMips)) + K * MipSize);
                uint32_t Got = 0;
                Data = CoDAssets::GamePackageCache->ExtractPackageObject(Key, (int32_t)Size, Got);
                if (Data == nullptr || Got < Size) return nullptr;
            }
            return CoDRawImageTranslator::TranslateBC(Data, Size, W, H, Dxgi[Format], 1, false, D);
        }

        void ExportImages(const Context& C, Totals& T)
        {
            ImageList L;
            for (const auto& I : Walk().Instances)
                if (OwnerIs(I, "image") && !IsReference(Kind::Image, AssetName(Kind::Image, I.Pos), I.Pos)) L.Order.push_back(I.Pos);
            // the record (format, sizes, the levels' keys) and the in-zone pixels, the extension
            L.KeyOf = [this](int64_t Pos, const std::string& Extension) {
                ExportWriters::ContentKey Key;
                const auto& Img = Image();
                for (int64_t K = 8; K < ImageSize && Pos + K < (int64_t)Img.size(); K++) Key.Mix(Img[(size_t)(Pos + K)]);
                const int Count = At<uint8_t>(Img, Pos + ImageMipCount);
                const int64_t Mips = Off(PointerAt(Pos + ImageMips));
                if (Mips >= 0 && Mips + Count * MipSize <= (int64_t)Img.size())
                    for (int64_t K = 0; K < Count * MipSize; K++) Key.Mix(Img[(size_t)(Mips + K)]);
                if (InZone(Pos))
                {
                    const int64_t Pixels = Off(PointerAt(Pos + ImagePixels));
                    const uint32_t Size = At<uint32_t>(Img, Pos + ImagePixelsSize);
                    if (Pixels + (int64_t)Size <= (int64_t)Img.size())
                        for (uint32_t K = 0; K < Size; K++) Key.Mix(Img[(size_t)Pixels + K]);
                }
                return Key.Mix(Extension).Key();
            };
            L.Decode = [this](int64_t Pos, const std::string&) { return LoadImage(Pos); };
            L.FailWhy = [this](int64_t Pos) {
                const uint8_t Format = At<uint8_t>(Image(), Pos + ImageFormat);
                return std::string(At<uint8_t>(Image(), Pos + ImageMipCount) == 0 ? "in zone" : "streamed") + ", format " +
                       std::to_string(Format) + " (DXGI " + std::to_string(Format < std::size(Dxgi) ? Dxgi[Format] : 0) + ")";
            };
            // a streamed image none of whose levels is in an .xsub of the install
            L.Absent = [this](int64_t Pos) {
                uint32_t Size = 0;
                uint16_t W = 0, H = 0;
                if (At<uint8_t>(Image(), Pos + ImageMipCount) == 0 || BestMip(Pos, Size, W, H) >= 0) return std::string();
                return std::string(CoDAssets::GamePackageCache == nullptr ? "streamed: no --install" : "streamed: in no .xsub of the install");
            };
            L.OnePerName = true;
            ExportImageList(C, T, L);
        }

        // A sound bank's SAB file (soundbank and soundbanktransient, 0x1f8: f6cb7ca0): the streamkey
        // asset at 0x1e0 names it; a streamkey (0x30, 0x6c5b860) holds its .xsub key at 8 and its size at
        // 0x28 (555 of 558 streamkeys of mp_shipment_s4, code_post_gfx and sp_tonga_house_tr are .xsub
        // objects of exactly that size: C:\tmp\vanguard\snd_check.py), its bytes in the zone at 0x18 when
        // u8 0x2d & 1 (0x6c5b900: read by 0x5ee22f0, or 0x5ee2310 when u8 0x2c is 2). Written once per key to %TEMP%\greyhound_vgsab, then
        // read by SABSupport like a bank file
        static constexpr int64_t BankStreamKey = 0x1e0, KeyXsub = 8, KeyData = 0x18, KeySize = 0x28, KeyFlags = 0x2c;

        bool BankFile(int64_t Pos, std::string& Path, std::string& Why)
        {
            const auto& Img = Image();
            const int64_t Key = Off(PointerAt(Pos + BankStreamKey));
            if (Key < 0 || Key + 0x30 > (int64_t)Img.size())
            {
                Why = "no streamkey";
                return false;
            }
            const uint64_t Xsub = At<uint64_t>(Img, Key + KeyXsub);
            const uint32_t Size = At<uint32_t>(Img, Key + KeySize);
            const bool Here = (At<uint8_t>(Img, Key + KeyFlags + 1) & 1) != 0;
            std::error_code Ec;
            const fs::path Dir = fs::temp_directory_path(Ec) / "greyhound_vgsab";
            fs::create_directories(Dir, Ec);
            // an .xsub bank by its key; one in the zone by the zone's path and the bank's place in it
            const fs::path File = Here ? Dir / std::format("zone_{}_{}.sabl", ExportNaming::Hex16(ExportNaming::Fnv1a(
                                                                                 ExportNaming::Lower(FastFilePath()) + "|" + std::to_string(Key))),
                                                           Size)
                                       : Dir / std::format("{:016x}_{}.sabl", Xsub, Size);
            Path = File.string();
            if (fs::is_regular_file(File, Ec) && fs::file_size(File, Ec) == Size) return true;
            std::unique_ptr<uint8_t[]> Data;
            uint32_t Got = 0;
            if (Here)
            {
                const int64_t At0 = Off(PointerAt(Key + KeyData));
                if (At0 < 0 || At0 + (int64_t)Size > (int64_t)Img.size())
                {
                    Why = "in-zone bank not linked";
                    return false;
                }
                Data = std::make_unique<uint8_t[]>(Size);
                std::memcpy(Data.get(), Img.data() + At0, Size);
                Got = Size;
                Counter("sound_banks_in_zone")++;
            }
            else
            {
                if (CoDAssets::GamePackageCache == nullptr || !CoDAssets::GamePackageCache->Exists(Xsub))
                {
                    std::string Rec;
                    for (int64_t K = 0; K < 0x30; K++) Rec += std::format("{:02x}", At<uint8_t>(Img, Key + K));
                    Why = CoDAssets::GamePackageCache == nullptr ? "no --install"
                                                                 : std::format("bank {:x} in no .xsub of the install (streamkey {})", Xsub, Rec);
                    return false;
                }
                Data = CoDAssets::GamePackageCache->ExtractPackageObject(Xsub, (int32_t)Size, Got);
                if (Data == nullptr || Got < Size)
                {
                    Why = std::format("bank {:x} not read", Xsub);
                    return false;
                }
                Counter("sound_banks_from_xsub")++;
            }
            const uint32_t Magic = Got >= 8 ? *(const uint32_t*)Data.get() : 0;
            const uint32_t Version = Got >= 8 ? *(const uint32_t*)(Data.get() + 4) : 0;
            Counter(std::format("sound_banks_magic_{:08x}_v{}", Magic, Version))++;
            const std::string Temp = Path + std::format(".{}.tmp", GetCurrentProcessId());
            if (!ExportWriters::WriteBytes(Temp, Data.get(), Size))
            {
                Why = "bank not written to " + Temp;
                return false;
            }
            fs::rename(Temp, File, Ec);
            if (Ec) fs::remove(Temp, Ec);
            return true;
        }

        // The names a bank file holds (u64 at 0x250: 128-byte entries, one per sound), by their FNV-1a
        // (lower case), the hash a SAB v17 entry's key is
        static void BankNames(const std::string& Path, std::unordered_map<uint64_t, std::string>& Out)
        {
            std::ifstream F(Path, std::ios::binary);
            std::vector<uint8_t> B((std::istreambuf_iterator<char>(F)), std::istreambuf_iterator<char>());
            if (B.size() < 0x258 || ExportNaming::At<uint32_t>(B, 0) != 0x23585532) return;
            const uint32_t Count = ExportNaming::At<uint32_t>(B, 0x14);
            const uint64_t Names = ExportNaming::At<uint64_t>(B, 0x250);
            if (Names == 0 || Names + (uint64_t)Count * 128 > B.size()) return;
            for (uint32_t K = 0; K < Count; K++)
            {
                const char* S = (const char*)B.data() + Names + (uint64_t)K * 128;
                const std::string N(S, strnlen(S, 128));
                if (!N.empty()) Out.emplace(ExportNaming::Fnv1a(ExportNaming::Lower(N)), N);
            }
        }

        // A bank's streamed sounds (0x1e8, u32 0x1f0 of them): 56-byte SAB v17 entries whose offset is the
        // .xsub object holding the entry's seek table and Opus packets (GameVanguardLive::ReadXSound)
        static constexpr int64_t BankStreamed = 0x1e8, BankStreamedCount = 0x1f0, StreamedSize = 0x38;

        void ExportStreamed(const std::vector<int64_t>& Banks, const std::unordered_map<uint64_t, std::string>& Names, const Context& C,
                            Totals& T)
        {
            auto& Kt = T.Kinds[Kind::Sound];
            ExportWriters::SoundSink Sink;
            const std::string Extension = ExportWriters::SoundExtension(Sink.Format());
            const std::string Dir = C.KindDir(Kind::Sound);
            std::set<uint64_t> Done;
            for (const int64_t Pos : Banks)
            {
                const uint32_t Count = At<uint32_t>(Image(), Pos + BankStreamedCount);
                const int64_t List = Off(PointerAt(Pos + BankStreamed));
                if (Count == 0 || List < 0 || List + (int64_t)Count * StreamedSize > (int64_t)Image().size()) continue;
                for (uint32_t K = 0; K < Count; K++)
                {
                    const int64_t E = List + (int64_t)K * StreamedSize;
                    const uint64_t Key = At<uint64_t>(Image(), E), Object = At<uint64_t>(Image(), E + 0x10);
                    if (!Done.insert(Key).second) continue;
                    const auto It = Names.find(Key);
                    const std::string Full = It != Names.end() ? It->second : std::format("xsound_{:x}", Key);
                    if (!C.Wants(Kind::Sound, Full)) continue;
                    if (It == Names.end()) Counter("sounds_streamed_unnamed")++;
                    if (CoDAssets::GamePackageCache == nullptr || !CoDAssets::GamePackageCache->Exists(Object))
                    {
                        Counter("sounds_streamed_not_installed")++;
                        T.Lacks(Name(), Kind::Sound, Full, "streamed: in no .xsub of the install");
                        continue;
                    }
                    Kt.Seen++;
                    Counter("sounds_streamed")++;
                    CoDSound_t Sound;
                    const fs::path P(Full);
                    Sound.AssetName = ExportNaming::SafeFileName(P.stem().string());
                    Sound.FullPath = ExportNaming::SafeSubPath(P.parent_path()).string();
                    Sound.AssetPointer = Live().Addr((uint64_t)E);
                    Sound.AssetSize = At<uint32_t>(Image(), E + 0x18);
                    Sound.FrameCount = At<uint32_t>(Image(), E + 0x20);
                    Sound.FrameRate = At<uint32_t>(Image(), E + 0x28);
                    Sound.ChannelsCount = At<uint8_t>(Image(), E + 0x31);
                    Sound.IsFileEntry = false;
                    Sink.Export(CoDAssets::Game, Sound, (fs::path(Dir) / Sound.FullPath).string(), Extension, SoundExported(T, Full));
                }
            }
            Sink.Finish();
        }

        void ExportSounds(const Context& C, Totals& T)
        {
            std::vector<std::string> Banks;
            std::vector<int64_t> WithStreamed;
            std::set<std::string> Seen;
            for (const auto& I : Walk().Instances)
            {
                if (!OwnerIs(I, "soundbank") && !OwnerIs(I, "soundbanktransient")) continue;
                const std::string Bank = AssetName(Kind::Sound, I.Pos);
                if (IsReference(Kind::Sound, Bank, I.Pos)) continue;
                if (At<uint32_t>(Image(), I.Pos + BankStreamedCount) != 0) WithStreamed.push_back(I.Pos);
                std::string Path, Why;
                if (!BankFile(I.Pos, Path, Why))
                {
                    Counter("sound_banks_not_read")++;
                    T.Lacks(Name(), Kind::Sound, Bank, Why);
                    continue;
                }
                if (Seen.insert(Path).second) Banks.push_back(Path);
            }
            Counter("sound_banks") += Banks.size();
            ExportSoundBanks(Banks, C, T);
            if (WithStreamed.empty()) return;
            std::unordered_map<uint64_t, std::string> Names;
            for (const auto& B : Banks) BankNames(B, Names);
            ExportStreamed(WithStreamed, Names, C, T);
        }
    };

    // Each fact the model reader takes for granted, counted on the zone's own models (those the run
    // wants): model_check_<fact> when it holds, model_check_<fact>_not when it does not
    void GameZone::CheckModels(const Context& C)
    {
        const auto& Img = Image();
        const int64_t Size = (int64_t)Img.size();
        const auto Fact = [this](const char* Name, bool Holds) { Counter(std::string("model_check_") + Name + (Holds ? "" : "_not"))++; };
        for (const auto& I : Walk().Instances)
        {
            if (!OwnerIs(I, "xmodel")) continue;
            const std::string Name = AssetName(Kind::Model, I.Pos);
            if (IsReference(Kind::Model, Name, I.Pos) || !C.Wants(Kind::Model, Name) || I.Pos + (int64_t)sizeof(S4XModel) > Size) continue;
            const auto M = At<S4XModel>(Img, I.Pos);
            const uint32_t Bones = M.NumBones + M.UnkBoneCount;
            // the skeleton: each parent (u16, relative: the bone's index less its parent's) inside
            if (const int64_t Parents = Off(PointerAt(I.Pos + offsetof(S4XModel, ParentListPtr))); Parents >= 0 && Bones > M.NumRootBones)
            {
                bool Inside = true;
                for (uint32_t B = M.NumRootBones; B < Bones; B++)
                    if (const uint16_t P = At<uint16_t>(Img, Parents + (int64_t)(B - M.NumRootBones) * 2); P == 0 || P > B) Inside = false;
                Fact("parents_relative", Inside);
            }
            // a base matrix (0x20): a unit quaternion first
            if (const int64_t Base = Off(PointerAt(I.Pos + offsetof(S4XModel, BaseMatriciesPtr))); Base >= 0 && Bones > 0)
            {
                const float Q[4] = { At<float>(Img, Base), At<float>(Img, Base + 4), At<float>(Img, Base + 8), At<float>(Img, Base + 12) };
                const float L = Q[0] * Q[0] + Q[1] * Q[1] + Q[2] * Q[2] + Q[3] * Q[3];
                Fact("base_quat_unit", std::fabs(L - 1.0f) < 1e-3f);
            }
            const int64_t Lods = Off(PointerAt(I.Pos + offsetof(S4XModel, ModelLodsPtr)));
            if (Lods < 0 || Lods + M.NumLods * (int64_t)sizeof(S4XModelLod) > Size)
            {
                Fact("lods_in_zone", false);
                continue;
            }
            uint32_t SurfaceSum = 0;
            for (uint32_t L = 0; L < M.NumLods; L++)
            {
                const auto Lod = At<S4XModelLod>(Img, Lods + L * (int64_t)sizeof(S4XModelLod));
                SurfaceSum += Lod.NumSurfs;
                const int64_t Ms = Off(Lod.MeshPtr);
                if (Ms < 0 || Ms + (int64_t)sizeof(S4XModelMesh) > Size)
                {
                    Counter("model_check_lod_mesh_not_in_zone")++;
                    continue;
                }
                const auto Mesh = At<S4XModelMesh>(Img, Ms);
                const int64_t Surfs = Off(Mesh.SurfsPtr), Buf = Off(Mesh.MeshBufferPointer);
                if (StringAt(Mesh.NamePtr).starts_with(","))
                {
                    Counter("model_check_lod_mesh_header")++;
                    continue;
                }
                Fact("lod_surfs_eq_mesh_surfs", Lod.NumSurfs == Mesh.NumSurfs);
                Fact("lod_surfs_copy_empty", Lod.SurfsPtr == 0);
                if (Surfs < 0 || Buf < 0) continue;
                const auto B = At<S4XModelMeshBufferInfo>(Img, Buf);
                const bool Streamed = (B.Streamed & 1) != 0;
                Fact(Streamed ? "mesh_streamed" : "mesh_in_zone", true);
                if (Streamed)
                {
                    Fact("mesh_key_in_xsub", CoDAssets::GamePackageCache != nullptr && CoDAssets::GamePackageCache->Exists(Mesh.LODStreamKey));
                    Fact("mesh_record_0_is_key", B.BufferPtr == Mesh.LODStreamKey);
                }
                else Fact("mesh_bytes_in_zone", Off(B.BufferPtr) >= 0 && Off(B.BufferPtr) + (int64_t)B.BufferSize <= Size);
                for (uint32_t S = 0; S < Mesh.NumSurfs && Surfs + (S + 1) * (int64_t)sizeof(S4XModelSurface) <= Size; S++)
                {
                    const int64_t Sp = Surfs + S * (int64_t)sizeof(S4XModelSurface);
                    const auto F = At<S4XModelSurface>(Img, Sp);
                    const uint64_t V = F.VertexCount, T = F.FacesCount, N = B.BufferSize;
                    Fact("surface_buffer_is_lods", F.MeshBufferPointer == Mesh.MeshBufferPointer);
                    Fact("positions_fit", F.Offsets[0] + 8 * V <= N);
                    Fact("uvs_fit", F.Offsets[1] + 4 * V <= N);
                    Fact("normals_fit", F.Offsets[2] + 4 * V <= N);
                    Fact("faces_fit", F.Offsets[3] + 6 * T <= N);
                    Fact(F.Offsets[6] == 0xffffffff ? "colors_none" : "colors_fit", F.Offsets[6] == 0xffffffff || F.Offsets[6] + 4 * V <= N);
                    // the streams back to back: the faces (3), the positions (0), a 4-byte stream (5),
                    // the UVs (1), the normals (2)
                    Fact("positions_after_faces", F.Offsets[0] == F.Offsets[3] + 6 * T);
                    Fact("stream5_after_positions", F.Offsets[5] == F.Offsets[0] + 8 * V);
                    Fact("uvs_after_stream5", F.Offsets[1] == F.Offsets[5] + 4 * V);
                    Fact("normals_after_uvs", F.Offsets[2] == F.Offsets[1] + 4 * V);
                    Fact(F.NewScale == -1 ? "scale_box" : "scale_own", true);
                    // every vertex rigid or skinned, the skinned weights' size the columns'
                    uint64_t Rigid = 0;
                    if (const int64_t R = Off(F.RigidWeightsPtr); R >= 0)
                        for (uint32_t K = 0; K < F.VertListCount; K++) Rigid += At<uint16_t>(Img, R + K * 8 + 2);
                    uint64_t Skinned = 0;
                    for (const uint16_t W : F.WeightCounts) Skinned += W;
                    if (F.VertListCount > 0 || Skinned > 0) Fact("weights_cover_vertices", Rigid + Skinned == V);
                    if (Skinned > 0) Fact("weights_size_is_columns", (F.WeightsSize & ~1u) == MeshDecode::SkinnedColumnsSize(F.WeightCounts));
                    // a face's corners inside the surface (the faces in the zone's copy only)
                    if (!Streamed && Off(B.BufferPtr) >= 0 && F.Offsets[3] + 6 * T <= N && T > 0)
                    {
                        uint16_t Max = 0;
                        for (uint64_t K = 0; K < 3 * T; K++) Max = std::max(Max, At<uint16_t>(Img, Off(B.BufferPtr) + F.Offsets[3] + (int64_t)K * 2));
                        Fact("faces_inside_vertices", Max < V);
                    }
                }
            }
            Fact("surfaces_sum_lods", SurfaceSum == M.NumSurfaces);
        }
    }

    // ---- the level (the s4 tables' gfx_map 0x526a890, gfx_map_trzone 0x526dc20; C2M's Vanguard.cs as a
    // second opinion). GfxWorld (0x7a80): its base name 8; GfxWorldDraw inline at 0x88 (0x6c74310:
    // surface count u32 0x88, surface data count u32 0x8c, material count u32 0x90; the 32-byte
    // surfaces at 0xe0, the material pointers at 0x178, the 128-byte surface data at 0x188);
    // GfxWorldDpvsStatic inline at 0x340 (0x6c73360: model count u32 0x344, 16-byte models at 0x3a8
    // (the XModel at 0); collection count u32 0x348, 16-byte collections at 0x3b8; placement count u32
    // 0x350, 24-byte placements at 0x4b8). Every surface is the world's: the brush models' list
    // (count i32 0x3de0, 0x60 each at 0x3de8) holds one model on mp_shipment_s4, its record not
    // Modern Warfare 2019's (no surface range at 84 / 88).
    constexpr int64_t WorldBaseName = 8, SurfaceCountAt = 0x88, DataCountAt = 0x8c, MaterialCountAt = 0x90;
    constexpr int64_t SurfacesAt = 0xe0, MaterialsAt = 0x178, SurfaceDataAt = 0x188;
    constexpr int64_t SModelCountAt = 0x344, SModelsAt = 0x3a8, CollectionCountAt = 0x348, CollectionsAt = 0x3b8;
    constexpr int64_t InstanceCountAt = 0x350, InstancesAt = 0x4b8;
    // A surface (32): the byte offset of its first vertex's position u32 0 (in its data's position
    // stream), its first index u32 4 (in its transient zone's u16 indices, counted from its first
    // vertex: surface 1 of mp_shipment_s4 starts at 36, surface 0's 12 triangles after 0), triangle
    // count u16 0xc, vertex count u16 0xe, surface data u32 0x10, material u32 0x14
    constexpr int64_t SurfaceSize = 32, SurfFirstVertex = 0, SurfFirstIndex = 4, SurfTris = 0xc, SurfVertices = 0xe, SurfData = 0x10, SurfMaterial = 0x14;
    // Its data (128): the positions' scale f32 0 and offset 3 x f32 4 (21-bit packed, as a model's),
    // its transient zone u32 0x10, its UV layer count u32 0x1c, its vertex count u32 0x20, then its
    // streams' byte offsets in the zone's vertex bytes (u32 0x20 + 4 k): positions 1 (8 bytes),
    // normals 2 (packed quaternion), 3 (8 bytes), colours 4 (0: none), UVs 5 (2 floats per layer):
    // back to back (data 0 of mp_shipment_s4: 172 vertices, positions 4, normals 0x564 = 4 + 8 x
    // 172, 0x814, UVs 0xd74, the next data's positions 0x12d4 = 0xd74 + 8 x 172)
    constexpr int64_t DataSize = 128, DataScale = 0, DataOffset = 4, DataZone = 0x10, DataLayers = 0x1c, DataStreams = 0x20;
    // GfxWorldTransientZone (0x88): its number u32 0x10, vertex byte count u32 0x18, index count u32
    // 0x1c, the vertex bytes 0x28, the u16 indices 0x30
    constexpr int64_t TrNumber = 0x10, TrVertexSize = 0x18, TrIndexCount = 0x1c, TrVertices = 0x28, TrIndices = 0x30;

    bool GameZone::ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models)
    {
        const int64_t World = Root("gfx_map");
        if (World < 0) return false;
        const auto& Img = Image();
        FILE* Dbg = Maps::DebugFile();
        Scene.Name = StringAt(PointerAt(World + WorldBaseName));
        if (Scene.Name.empty()) Scene.Name = Name();

        // the transient zones here, by number
        struct Tr
        {
            int64_t Vertices = -1, Indices = -1;
            uint32_t VertexSize = 0, IndexCount = 0;
        };
        std::unordered_map<uint32_t, Tr> Zones;
        for (const auto& I : Walk().Instances)
            if (I.Parent < 0 && OwnerIs(I, "gfx_map_trzone"))
            {
                const Tr Z{ Off(PointerAt(I.Pos + TrVertices)), Off(PointerAt(I.Pos + TrIndices)), At<uint32_t>(Img, I.Pos + TrVertexSize),
                            At<uint32_t>(Img, I.Pos + TrIndexCount) };
                Zones[At<uint32_t>(Img, I.Pos + TrNumber)] = Z;
                if (Dbg)
                    std::fprintf(Dbg, "[map] trzone %u '%s' vertex bytes %u at %llx indices %u at %llx\n", At<uint32_t>(Img, I.Pos + TrNumber),
                                 StringAt(PointerAt(I.Pos)).c_str(), Z.VertexSize, (long long)Z.Vertices, Z.IndexCount, (long long)Z.Indices);
            }
        const uint32_t SurfaceCount = At<uint32_t>(Img, World + SurfaceCountAt), DataCount = At<uint32_t>(Img, World + DataCountAt);
        const uint32_t MaterialCount = At<uint32_t>(Img, World + MaterialCountAt);
        const int64_t Surfaces = Off(PointerAt(World + SurfacesAt)), Data = Off(PointerAt(World + SurfaceDataAt));
        const int64_t MaterialList = Off(PointerAt(World + MaterialsAt));
        if (Dbg)
            std::fprintf(Dbg, "[map] %s surfaces %u at %llx data %u at %llx materials %u at %llx\n", Scene.Name.c_str(), SurfaceCount,
                         (long long)Surfaces, DataCount, (long long)Data, MaterialCount, (long long)MaterialList);
        if (Surfaces < 0 || Data < 0 || MaterialList < 0) return false;
        if (Dbg) std::fflush(Dbg);

        // a vertex key: its surface's first key plus its number in the surface, each surface's keys
        // naming its data
        struct KeyRange
        {
            uint32_t First, Data, Base;   // its first key, its data, its first vertex's number in the data
        };
        std::vector<KeyRange> Ranges;   // by first key
        uint32_t NextKey = 0;
        const auto DataOf = [&](uint32_t Key, uint32_t& Local) {
            auto It = std::upper_bound(Ranges.begin(), Ranges.end(), Key, [](uint32_t K, const KeyRange& R) { return K < R.First; });
            if (It == Ranges.begin()) return (int64_t)-1;
            --It;
            Local = Key - It->First + It->Base;
            return Data + (int64_t)It->Data * DataSize;
        };
        const auto Stream = [&](int64_t D, int K) { return (int64_t)At<uint32_t>(Img, D + DataStreams + K * 4); };
        const Maps::WorldBuilder::Fetch Fetch = [&](uint32_t Key, Maps::Vertex& V) {
            uint32_t Local = 0;
            const int64_t D = DataOf(Key, Local);
            if (D < 0) return false;
            const auto Z = Zones.find(At<uint32_t>(Img, D + DataZone));
            if (Z == Zones.end() || Z->second.Vertices < 0) return false;
            const Tr& T = Z->second;
            const int64_t P = Stream(D, 1) + (int64_t)Local * 8;
            if (P + 8 > (int64_t)T.VertexSize) return false;
            // three 21-bit unsigned fields, each x the scale plus the offset (a grid of 1/16 unit
            // from the data's corner, not a model's box about its centre)
            const uint64_t Packed = At<uint64_t>(Img, T.Vertices + P);
            const float Scale = At<float>(Img, D + DataScale);
            for (int K = 0; K < 3; K++) V.Pos[K] = (float)((Packed >> (21 * K)) & 0x1fffff) * Scale + At<float>(Img, D + DataOffset + K * 4);
            if (const int64_t N = Stream(D, 2) + (int64_t)Local * 4; N + 4 <= (int64_t)T.VertexSize)
            {
                const Vector3 Normal = MeshDecode::UnpackPackedQuat(At<uint32_t>(Img, T.Vertices + N));
                V.Normal[0] = Normal.X, V.Normal[1] = Normal.Y, V.Normal[2] = Normal.Z;
            }
            if (const int64_t C = Stream(D, 4) + (int64_t)Local * 4; Stream(D, 4) != 0 && C + 4 <= (int64_t)T.VertexSize) std::memcpy(V.Color, Img.data() + T.Vertices + C, 4);
            const int64_t Layers = std::max<int64_t>(At<uint32_t>(Img, D + DataLayers), 1);
            if (const int64_t U = Stream(D, 5) + (int64_t)Local * 8 * Layers; U + 8 <= (int64_t)T.VertexSize)
                for (int K = 0; K < 2; K++) V.Uv[K] = At<float>(Img, T.Vertices + U + K * 4);
            return true;
        };
        // A material's slots whose pointer lands on no image the walk read are left out, and a
        // material whose own name is no text is named by its address (both counted: none on
        // mp_shipment_s4 since the layout was read off the code; 2 449 slots before)
        std::unordered_set<int64_t> ImagesHere;
        for (const auto& I : Walk().Instances)
            if (OwnerIs(I, "image")) ImagesHere.insert(I.Pos);
        std::unordered_map<uint64_t, std::string> Materials;
        const auto MaterialOf = [&](uint64_t Address) {
            auto Found = Materials.find(Address);
            if (Found == Materials.end())
            {
                std::string Name;
                if (Off(Address) >= 0)
                {
                    XMaterial_t M = S4Readers::ReadXMaterial(Address);
                    const auto Text = [](const std::string& S) { return std::all_of(S.begin(), S.end(), [](char C) { return (uint8_t)C >= 0x20 && (uint8_t)C < 0x7f; }); };
                    if (!Text(M.MaterialName))
                    {
                        Counter("map_materials_named_by_address")++;
                        M.MaterialName = std::format("material_{:x}", Address);
                    }
                    const size_t Before = M.Images.size();
                    std::erase_if(M.Images, [&](const XImage_t& I) {
                        return I.ImagePtr != ExportWriters::ImageElsewhere && (!ImagesHere.count(Off(I.ImagePtr)) || !Text(I.ImageName));
                    });
                    Counter("map_material_slots_on_no_image") += Before - M.Images.size();
                    Name = Maps::AddMaterial(Scene, M);
                }
                Found = Materials.emplace(Address, Name).first;
            }
            return Found->second;
        };
        const auto Range = [&](uint32_t) { return std::make_pair(0u, SurfaceCount); };
        size_t NoZone = 0;
        Maps::BuildWorld(Scene, 1u, SurfaceCount, Range, Fetch, [&](uint32_t S, Maps::WorldBuilder& Mesh) {
            const int64_t Sf = Surfaces + (int64_t)S * SurfaceSize;
            const uint32_t D = At<uint32_t>(Img, Sf + SurfData), M = At<uint32_t>(Img, Sf + SurfMaterial);
            const uint16_t Vertices = At<uint16_t>(Img, Sf + SurfVertices), Tris = At<uint16_t>(Img, Sf + SurfTris);
            const uint32_t FirstIndex = At<uint32_t>(Img, Sf + SurfFirstIndex);
            if (D >= DataCount || M >= MaterialCount)
            {
                Counter("map_surfaces_unread")++;
                return;
            }
            const auto Z = Zones.find(At<uint32_t>(Img, Data + (int64_t)D * DataSize + DataZone));
            if (Z == Zones.end() || Z->second.Indices < 0)
            {
                NoZone++;
                return;
            }
            const uint32_t First = NextKey;
            const int64_t Pos0 = (int64_t)At<uint32_t>(Img, Sf + SurfFirstVertex) - (int64_t)At<uint32_t>(Img, Data + (int64_t)D * DataSize + DataStreams + 4);
            if (Pos0 < 0 || Pos0 % 8 != 0)
            {
                Counter("map_surfaces_unread")++;
                return;
            }
            Ranges.push_back({ First, D, (uint32_t)(Pos0 / 8) });
            NextKey += Vertices;
            const std::string& Material = MaterialOf(PointerAt(MaterialList + (int64_t)M * 8));
            for (uint32_t T = 0; T < Tris; T++)
            {
                const int64_t I = (int64_t)FirstIndex + (int64_t)T * 3;
                if (I + 2 >= (int64_t)Z->second.IndexCount) break;
                uint32_t Key[3];
                bool Inside = true;
                for (int K = 0; K < 3; K++)
                {
                    const uint16_t Local = At<uint16_t>(Img, Z->second.Indices + (I + K) * 2);
                    Inside = Inside && Local < Vertices;
                    Key[K] = First + Local;
                }
                if (!Inside || !Mesh.Triangle(Material, Key[0], Key[1], Key[2])) Counter("map_triangles_unread")++;
            }
        });
        Counter("map_surfaces_in_no_zone_here") += NoZone;

        // ---- the static models: collections of placements of one model each (first placement u32
        // 0, count u32 4, model u16 8), a placement packed as Modern Warfare 2019's (24 bytes)
        const uint32_t ModelCount = At<uint32_t>(Img, World + SModelCountAt);
        const uint32_t CollectionCount = At<uint32_t>(Img, World + CollectionCountAt);
        const uint32_t InstanceCount = At<uint32_t>(Img, World + InstanceCountAt);
        const int64_t SModels = Off(PointerAt(World + SModelsAt)), Collections = Off(PointerAt(World + CollectionsAt));
        const int64_t Instances = Off(PointerAt(World + InstancesAt));
        for (uint32_t K = 0; SModels >= 0 && Collections >= 0 && Instances >= 0 && K < CollectionCount; K++)
        {
            const int64_t Col = Collections + (int64_t)K * 16;
            const uint32_t FirstInstance = At<uint32_t>(Img, Col), Count = At<uint32_t>(Img, Col + 4);
            const uint16_t Model = At<uint16_t>(Img, Col + 8);
            if (Model >= ModelCount) continue;
            const uint64_t Address = PointerAt(SModels + (int64_t)Model * 16);
            const std::string ModelName = ModelAt(Address);
            if (ModelName.empty()) continue;
            Models.emplace(ModelName, Address);
            for (uint32_t J = FirstInstance; J < FirstInstance + Count && J < InstanceCount; J++)
            {
                Maps::Instance In = Maps::PackedPlacement(Img, Instances + (int64_t)J * 24, false);
                In.Name = "smodel_" + std::to_string(J);
                In.Model = ModelName;
                Scene.StaticInstances.push_back(In);
            }
        }
        if (Dbg)
            std::fprintf(Dbg, "[map] static models %u collections %u instances %u placed %zu\n", ModelCount, CollectionCount, InstanceCount,
                         Scene.StaticInstances.size());
        // ComWorld's primary lights (com_map, the tables' 0x6c1f020: count u32 0x30, 0xa0-byte records at
        // 0x38, each its definition's name at 0x98): Modern Warfare 2019's record, the type byte 1,
        // colour 32, direction 44, origin 68, radius 80, cosHalfFovOuter 108 (README.md: checked on
        // mp_shipment_s4's)
        LightLayout L;
        L.Count = 0x30, L.Array = 0x38, L.Size = 0xa0, L.TypeAt = 1;
        L.Color = 32, L.Dir = 44, L.Origin = 68, L.Radius = 80, L.CosHalfFov = 108, L.Def = 0x98;
        AddPrimaryLights(Scene, L);
        AddMapEnts(Scene);
        return true;
    }

    class Game final : public DiskGame::IDiskGame
    {
    public:
        const char* Id() const override { return "vanguard"; }
        const char* Title() const override { return "Call of Duty: Vanguard"; }
        const char* Engine() const override { return "S4"; }
        std::vector<std::string> Aliases() const override { return { "s4", "vg" }; }
        std::unique_ptr<DiskGame::IDiskZone> Unopened() const override { return std::make_unique<GameZone>(std::string(), std::string()); }

        std::unique_ptr<DiskGame::IDiskZone> Open(const std::string& Path, const DiskGame::ExportOptions& Options, std::string& Error) const override
        {
            // the packages images and sounds stream from (--install): the .xsub files beside the .ff
            auto Packages = Options.Install.empty() ? nullptr : TreyarchPackages::Open<XSUBCache>(Options.Install);
            if (!Options.Install.empty() && Packages == nullptr)
            {
                Error = "could not open the packages of " + Options.Install;
                return nullptr;
            }
            auto Zone = std::make_unique<GameZone>(Path, ((!Options.Cache.empty() ? fs::path(Options.Cache) : fs::temp_directory_path() / "greyhound_s4") / "s4_materials").string());
            Zone->ServesPackages(std::move(Packages));
            Zone->SetInstall(this, Options, Options.Install);
            if (!Zone->Open(Error)) return nullptr;
            return Zone;
        }
    };

    const Game TheGame;
    const DiskGame::Registration Registered(TheGame);
}
