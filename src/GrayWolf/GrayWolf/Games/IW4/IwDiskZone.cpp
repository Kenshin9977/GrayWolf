#include "stdafx.h"

#include "Games/IW4/IwDiskZone.h"

#include "CoDIWITranslator.h"
#include "ExportNaming.h"
#include "ExportWriters.h"
#include "Games/IW/IWSoundAliases.h"
#include "Games/IwClassic/IwClassicAnimOwners.h"
#include "Games/Treyarch/TreyarchEffects.h"
#include "IWDCache.h"
#include <miniz/miniz.h>
#include "SettingsManager.h"
#include "Strings.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>

namespace fs = std::filesystem;

namespace Iw64
{
    namespace
    {
        std::mutex PackagesLock;
        std::map<std::string, std::unique_ptr<IWDCache>> PackagesByInstall;

        // the install's main folder: the first ancestor of the zone holding main/ with .iwd files
        std::string MainFolder(const std::string& FastFile)
        {
            std::error_code Ec;
            for (fs::path P = fs::path(FastFile).parent_path(); !P.empty() && P != P.parent_path(); P = P.parent_path())
            {
                const fs::path Main = P / "main";
                if (!fs::is_directory(Main, Ec)) continue;
                for (const auto& E : fs::directory_iterator(Main, Ec))
                    if (ExportNaming::Lower(E.path().extension().string()) == ".iwd") return Main.string();
            }
            return std::string();
        }

        uint64_t Mix(uint64_t H, uint64_t V)
        {
            H = (H ^ V) * 0xFF51AFD7ED558CCDull;
            return H ^ (H >> 32);
        }

        // an unsigned zone (IWffu100), or a signed one (IWff0100) whose auth chunk (IWffs100, at 21)
        // FastFile steps over; not the 32-bit originals' layout
        bool Readable(const fs::path& P)
        {
            std::ifstream F(P, std::ios::binary);
            char Head[29] = {};
            F.read(Head, sizeof(Head));
            if (!F) return false;
            return std::memcmp(Head, "IWffu100", 8) == 0 ||
                   (std::memcmp(Head, "IWff0100", 8) == 0 && std::memcmp(Head + 21, "IWffs100", 8) == 0);
        }
    }

    IwDiskZone::IwDiskZone(const std::string& Path, const GameInfo& G) : IWZone(Path), Game(G), Weapons(Path)
    {
        using namespace DiskGame;
        // the weapon files and their stats table, through OpenAssetTools (Games/OAT/OatWeapons.h)
        Exports(Kind::Weapons, [this](const Context& C, Totals& T) { OatWeapons::ExportWeapons(Weapons, Name(), C, T); });
        Exports(Kind::WeaponStats, [this](const Context& C, Totals& T) { OatWeapons::ExportWeaponStats(Weapons, Name(), C, T, Game.Title); });
        Exports(Kind::Image, [this](const Context& C, Totals& T) { ExportImages(C, T); });
        Exports(Kind::Sound, [this](const Context& C, Totals& T) { ExportSounds(C, T); });
        Exports(Kind::Rawfile, [this](const Context& C, Totals& T) { ExportRawfiles(C, T); });
        Exports(Kind::Data, [this](const Context& C, Totals& T) { ExportStringTables(C, T); });
        Exports(Kind::Map, [this](const Context& C, Totals& T) {
            BookMaterialsElsewhere();
            ExportMap(C, T);
        });
        // a model's material headers: the materials this zone or another defines
        if (const auto* Models = ExporterOf(Kind::Model))
            Exports(Kind::Model, [this, Read = *Models](const Context& C, Totals& T) {
                BookMaterialsElsewhere();
                Read(C, T);
            });
        // Material: its techset, images and blend factors as JSON, its images by the image kind,
        // with its techset's name (the pointer before the texture table: the iw4 table's material
        // 0x58, iw5's 0x60), what tells the preview how it blends; the model kind's reader leaves it
        // FxEffectDef (MW2 0x28, MW3 0x40; elements 0x120) as JSON, CoD4's with 8-byte pointers
        ExportsEffects(
            [this](uint64_t A) {
                // a header as it is: the fx and material kinds name it and need it from where it is
                // defined, whichever kinds ran before (MaterialDefinedAt, MaterialImagesElsewhere)
                auto Defined = std::exchange(MaterialDefinedAt, nullptr);
                auto Elsewhere = std::exchange(MaterialImagesElsewhere, nullptr);
                XMaterial_t M = ReadXMaterial(A, *Game.Readers);
                MaterialDefinedAt = std::move(Defined), MaterialImagesElsewhere = std::move(Elsewhere);
                const int64_t Techset = Off(At<uint64_t>(Image(), Off(A) + Game.Readers->MaterialTextureTable - 8));
                if (Techset >= 0) M.TechsetName = StringAt(At<uint64_t>(Image(), Techset));
                return M;
            },
            Game.Readers == &Mw3 ? Treyarch::EffectLayout::Iw5 : Treyarch::EffectLayout::Iw4);
        // the .gsc rawfiles (source), and MW3's scriptfiles (bytecode)
        Exports(Kind::Script, [this](const Context& C, Totals& T) {
            ExportScripts(C, T, Game.ScriptVm,
                          { { "rawfile", [this](int64_t Pos, Scripts::Script& S) {
                                 if (!Scripts::IsSourceName(S.Name)) S.Name.clear();
                                 return S.Name.empty() || RawfileBytes(Pos, S.Bytes);
                             } },
                            { "scriptfile", [this](int64_t Pos, Scripts::Script& S) { return ReadScriptFile(Pos, S); } } });
        });
    }

    IwDiskZone::~IwDiskZone()
    {
        if (!IsServed()) return;
        MaterialDefinedAt = nullptr;
        MaterialImagesElsewhere = nullptr;
    }

    void IwDiskZone::ReadMaterials()
    {
        if (MaterialsRead) return;
        MaterialsRead = true;
        // a material's name pointer is its first field; a header's name starts with ','
        for (const auto& I : Walk().Instances)
        {
            if (!OwnerIs(I, "material")) continue;
            const std::string N = StringAt(At<uint64_t>(Image(), I.Pos));
            if (!IsReference(DiskGame::Kind::Material, N, I.Pos)) Materials.emplace(ExportNaming::Lower(N), I.Pos);
        }
    }

    void IwDiskZone::Served(bool On)
    {
        MaterialDefinedAt = nullptr;
        MaterialImagesElsewhere = nullptr;
        if (!On) return;
        MaterialDefinedAt = [this](const std::string& Name) -> uint64_t {
            ReadMaterials();
            const auto It = Materials.find(ExportNaming::Lower(Name));
            return It == Materials.end() ? 0 : Live().Addr((uint64_t)It->second);
        };
        MaterialImagesElsewhere = [this](const std::string& Name, IWMaterialBooks::ImageList& Out) {
            return MaterialsElsewhere != nullptr && MaterialsElsewhere->Find(Name, Out);
        };
    }

    void IwDiskZone::BookMaterialsElsewhere()
    {
        if (BooksTried || Owner == nullptr || Install.empty()) return;
        BooksTried = true;
        // Material: the texture count u8 and table, slots {u32 name hash, pad, image*} of 0x10; GfxImage's name
        IWMaterialBooks::Layout L;
        L.Profile = Game.Profile;
        L.ImageCount = Game.Readers->MaterialTextureCount, L.Images = Game.Readers->MaterialTextureTable;
        L.SlotSize = 0x10, L.SlotImage = 8, L.ImageName = ImageName;
        L.WithSlotsOnly = true;
        // each header's first defining zone (the install's order), this one left out
        const auto Headers = MaterialHeaders();
        IWZone::BookMaterialsElsewhere(
            Headers.size(), [&](const DiskGame::ZoneIndex& Index, size_t K) { return Index.DefinedIn({ DiskGame::Kind::Material, Headers[K] }); },
            true, L);
    }

    void IwDiskZone::Fixups()
    {
        Served(true);   // opened: the zone the handlers reach
        // the script strings: count and pointer in the XAssetList just before the
        // stream, then one u64 slot each, then the text of every inline slot
        const auto& Z = Zone();
        const int64_t Start = Rules().HeaderSize, List = Start - 0x20;
        const uint32_t Count = At<uint32_t>(Z, List);
        if (At<uint64_t>(Z, List + 8) == 0 || (int64_t)Count * 8 > (int64_t)Z.size()) return;
        Slots = InlineStrings(Z, Start, Count, 8, Rules().Marker);
    }

    void IwDiskZone::Defines(std::vector<DiskGame::AssetId>& Out) const
    {
        ZonekitZone::Defines(Out);
        std::set<std::string> Seen;
        for (const auto& I : Walk().Instances)
        {
            if (!OwnerIs(I, "image")) continue;
            const std::string N = StringAt(At<uint64_t>(Image(), I.Pos + ImageName));
            if (!IsReference(DiskGame::Kind::Image, N, I.Pos) && Seen.insert(ExportNaming::Lower(N)).second)
                Out.push_back({ DiskGame::Kind::Image, N });
        }
    }

    IWDCache* IwDiskZone::Packages()
    {
        const std::string Main = MainFolder(FastFilePath());
        if (Main.empty()) return nullptr;
        std::lock_guard<std::mutex> Lock(PackagesLock);
        auto& P = PackagesByInstall[ExportNaming::Lower(Main)];
        if (!P)
        {
            P = std::make_unique<IWDCache>();
            P->LoadPackageCache(Main);
        }
        return P.get();
    }

    std::unique_ptr<XImageDDS> IwDiskZone::Decode(int64_t Pos, uint64_t& Key)
    {
        const uint64_t Address = VirtualGameSession::Address((uint64_t)Pos);
        const std::string Name = StringAt(At<uint64_t>(Image(), Pos + ImageName));
        Key = 0;
        if (auto Dds = ZoneImage(Address))
        {
            // the pixels' own bytes: the same image in another zone links to this file
            uint64_t H = 0x9E3779B97F4A7C15ull;
            for (uint32_t K = 0; K < Dds->DataSize; K += 8)
            {
                uint64_t V = 0;
                std::memcpy(&V, Dds->DataBuffer + K, std::min<uint32_t>(8, Dds->DataSize - K));
                H = Mix(H, V);
            }
            Key = H ? H : 1;
            Counter("images_in_zone")++;
            return Dds;
        }
        IWDCache* Iwd = Packages();
        if (Iwd == nullptr || Name.empty()) return nullptr;
        uint32_t Size = 0;
        const auto Data = Iwd->ExtractPackageObject(Iwd->HashPackageID(Name), Size);
        if (Data == nullptr) return nullptr;
        auto Dds = CoDIWITranslator::TranslateIWI(Data, Size);
        if (Dds)
        {
            uint64_t H = 0x51ED270B27A3A7E1ull;
            for (const char Ch : ExportNaming::Lower(Name)) H = Mix(H, (uint8_t)Ch);
            Key = H ? H : 1;
            Counter("images_iwd")++;
        }
        return Dds;
    }

    std::unique_ptr<XImageDDS> IwDiskZone::DiskImage(const XImage_t& Image)
    {
        IwDiskZone* Current = ServedZone<IwDiskZone>();
        if (Current == nullptr || !Current->Live().Contains(Image.ImagePtr)) return nullptr;
        uint64_t Key = 0;
        return Current->Decode((int64_t)(Image.ImagePtr - VirtualGameSession::Base), Key);
    }

    void IwDiskZone::ExportImages(const DiskGame::Context& C, DiskGame::Totals& T)
    {
        auto& Kt = T.Kinds[DiskGame::Kind::Image];
        std::vector<std::pair<int64_t, std::string>> Order;
        std::set<std::string> Seen, ReferencedHere, DefinedHere;
        for (const auto& I : Walk().Instances)
        {
            if (!OwnerIs(I, "image")) continue;
            std::string Name = StringAt(At<uint64_t>(Image(), I.Pos + ImageName));
            const bool Reference = IsReference(DiskGame::Kind::Image, Name, I.Pos);
            if (Reference) Name = Referenced(DiskGame::Kind::Image, Name);
            if (Name.empty() || !C.Wants(DiskGame::Kind::Image, Name)) continue;
            if (Reference)
            {
                Kt.References++;
                ReferencedHere.insert(Name);
                continue;
            }
            DefinedHere.insert(Name);
            if (Seen.insert(ExportNaming::Lower(Name)).second) Order.emplace_back(I.Pos, Name);
        }
        ExportWriters::ImageSink Sink;
        const fs::path Dir = C.KindDir(DiskGame::Kind::Image);
        std::map<size_t, uint64_t> Keys;
        for (size_t N = 0; N < Order.size(); N++)
        {
            const auto& [Pos, ImageNameText] = Order[N];
            if (!C.Advance(N, Order.size(), ImageNameText)) break;
            Kt.Seen++;
            const std::string Path = (Dir / (ExportNaming::SafeFileName(ImageNameText) + Sink.Extension())).string();
            uint64_t Key = 0;
            auto Dds = Decode(Pos, Key);
            if (!Dds)
            {
                T.Fail(Name(), DiskGame::Kind::Image, ImageNameText, "no pixels in the zone or the .iwd files");
                continue;
            }
            Keys[(size_t)Pos] = Key;
            if (Sink.LinkIfKnown(Key, Path, (size_t)Pos))
            {
                Kt.Known++;
                continue;
            }
            Sink.Write(std::move(Dds), Path);
        }
        size_t Missing = 0;
        const auto W = Sink.Finish([&](size_t Pos) { uint64_t K = 0; return Decode((int64_t)Pos, K); }, Missing,
                                   [&](size_t Pos) { return Keys[Pos]; });
        Kt.Written += W.Written;
        Kt.Linked += W.Linked;
        if (W.Failed + Missing) T.Fail(Name(), DiskGame::Kind::Image, "", std::to_string(W.Failed + Missing) + " not written");
        for (const auto& N : ReferencedHere)
            if (!DefinedHere.count(N)) C.Needs(DiskGame::Kind::Image, N);
    }

    void IwDiskZone::ExportSounds(const DiskGame::Context& C, DiskGame::Totals& T)
    {
        auto& Kt = T.Kinds[DiskGame::Kind::Sound];
        std::vector<int64_t> Found;
        std::set<std::string> Seen;
        for (const auto& I : Walk().Instances)
        {
            if (!OwnerIs(I, "loaded_sound")) continue;
            const std::string N = AssetName(DiskGame::Kind::Sound, I.Pos);
            if (IsReference(DiskGame::Kind::Sound, N, I.Pos))
            {
                Kt.References++;
                continue;
            }
            if (C.Wants(DiskGame::Kind::Sound, N) && Seen.insert(ExportNaming::Lower(N)).second) Found.push_back(I.Pos);
        }
        const std::string Extension = ExportWriters::SoundExtension(ExportWriters::SoundFormatSetting());
        const std::string Dir = C.KindDir(DiskGame::Kind::Sound);
        for (size_t N = 0; N < Found.size(); N++)
        {
            const int64_t Pos = Found[N];
            const std::string Full = AssetName(DiskGame::Kind::Sound, Pos);
            if (!C.Advance(N, Found.size(), Full)) break;
            Kt.Seen++;
            CoDSound_t Sound;
            Sound.AssetName = ExportNaming::SafeFileName(fs::path(Full).replace_extension().string());
            if (!LoadedSound(VirtualGameSession::Address((uint64_t)Pos), Sound))
            {
                Counter("sounds_not_pcm16")++;
                T.Fail(Name(), DiskGame::Kind::Sound, Full, "not 16-bit PCM");
                continue;
            }
            std::string Crash;
            const int Code = ExportWriters::ExportSound(Sound, Dir, Extension, Crash);
            if (Code == static_cast<int>(ExportGameResult::Success)) Kt.Written++;
            else T.Fail(Name(), DiskGame::Kind::Sound, Full, std::format("code {} {}", Code, Crash.c_str()));
        }
        ExportStreamedSounds(C, T);
        if (SoundAliasCsv::Wanted(C)) ExportSoundAliases(C, T);
    }

    // snd_alias_t (OAT's IW4/IW5 headers, 8-byte pointers; the iw4/iw5 tables agree on its strings,
    // SoundFile 0x28 and curve): MW2 0x88 (volumes 0x34/0x38, pitches 0x3c/0x40, distances 0x44/0x48,
    // flags 0x50, probability 0x58, center 0x60, start delay 0x64, envelope 0x70/0x74/0x78); MW3 0x98
    // (volModIndex after the volumes: +4; a master priority after the flags: +0xc; the curve 0x78: +0x10). Flags:
    // looping 0, type 7-8 (matches the SoundFile on every alias); 2D/3D is not a flag here (the 6th,
    // spatialize, is set on 2 of 279 variants of so_ied_berlin): left empty.
    void IwDiskZone::ExportSoundAliases(const DiskGame::Context& C, DiskGame::Totals& T)
    {
        IWSoundAliases::Layout L;
        const bool Mw3 = Game.Readers->AliasSize == 0x98;
        const int64_t A = Mw3 ? 4 : 0, B = Mw3 ? 0xc : 0, E = Mw3 ? 0x10 : 0;
        L.ListCount = 0x10, L.ListCountBytes = 4, L.Size = Game.Readers->AliasSize;
        L.Subtitle = 8, L.Secondary = 0x10, L.SoundFile = 0x28, L.VolMin = 0x34, L.VolMax = 0x38;
        L.PitchMin = 0x3c + A, L.PitchMax = 0x40 + A, L.DistMin = 0x44 + A, L.DistMax = 0x48 + A, L.Flags = 0x50 + A;
        L.Probability = 0x58 + B, L.CenterPercentage = 0x60 + B, L.StartDelay = 0x64 + B;
        L.EnvelopMin = 0x70 + E, L.EnvelopMax = 0x74 + E, L.EnvelopPercentage = 0x78 + E;
        // the files ExportSounds wrote: a loaded sound flat, its path without extension made one
        // name; a streamed one under its folder
        const std::string Extension = ExportWriters::SoundExtension(ExportWriters::SoundFormatSetting());
        const auto File = [&](const std::string&, uint32_t, int64_t Spec) -> std::string {
            if (Spec < 0) return std::string();
            if (At<uint8_t>(Image(), Spec) == 1)
            {
                const int64_t Loaded = Off(At<uint64_t>(Image(), Spec + 8));
                std::string N = Loaded < 0 ? std::string() : StringAt(At<uint64_t>(Image(), Loaded));
                if (!N.empty() && N[0] == ',') N.erase(0, 1);
                return N.empty() ? N : ExportNaming::SafeFileName(fs::path(N).replace_extension().string()) + Extension;
            }
            const std::string Dir = StringAt(At<uint64_t>(Image(), Spec + 8)), Leaf = StringAt(At<uint64_t>(Image(), Spec + 0x10));
            return Leaf.empty() ? Leaf : ExportWriters::SoundFileName(Dir.empty() ? Leaf : Dir + "/" + Leaf, ExportWriters::SoundFormatSetting());
        };
        const SoundAliasCsv::ZoneView Z = AliasView();
        SoundAliasCsv::Rows Rows;
        for (const auto& I : Walk().Instances)
        {
            if (!OwnerIs(I, "sound")) continue;
            const std::string N = AssetName(DiskGame::Kind::Sound, I.Pos);
            if (IsReference(DiskGame::Kind::Sound, N, I.Pos)) continue;
            IWSoundAliases::Read(Z, I.Pos, N, L, File, Rows);
        }
        SoundAliasCsv::Write(C, T, Name(), Rows);
    }

    namespace
    {
        // the streamed sound files of the install's .iwd archives: "sound/<dir>/<name>", lowered,
        // to (archive, entry); built once per main folder
        struct StreamIndex
        {
            std::vector<std::string> Archives;
            std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> Entries;
        };
        std::mutex StreamsLock;
        std::map<std::string, std::unique_ptr<StreamIndex>> StreamsByMain;

        const StreamIndex* StreamIndexOf(const std::string& Main)
        {
            std::lock_guard<std::mutex> Lock(StreamsLock);
            auto& S = StreamsByMain[ExportNaming::Lower(Main)];
            if (S) return S.get();
            S = std::make_unique<StreamIndex>();
            std::error_code Ec;
            std::vector<std::string> Files;
            for (const auto& E : fs::directory_iterator(Main, Ec))
                if (ExportNaming::Lower(E.path().extension().string()) == ".iwd") Files.push_back(E.path().string());
            std::sort(Files.begin(), Files.end());
            for (const auto& F : Files)
            {
                mz_zip_archive Zip;
                std::memset(&Zip, 0, sizeof(Zip));
                if (!mz_zip_reader_init_file(&Zip, F.c_str(), 0)) continue;
                const uint32_t Index = (uint32_t)S->Archives.size();
                S->Archives.push_back(F);
                const mz_uint Count = mz_zip_reader_get_num_files(&Zip);
                for (mz_uint K = 0; K < Count; K++)
                {
                    mz_zip_archive_file_stat St;
                    if (!mz_zip_reader_file_stat(&Zip, K, &St)) continue;
                    const std::string N = ExportNaming::Lower(St.m_filename);
                    if (N.rfind("sound/", 0) == 0) S->Entries[N] = { Index, K };   // a later archive wins
                }
                mz_zip_reader_end(&Zip);
            }
            return S.get();
        }
    }

    // Streamed aliases (SoundFile type != 1: dir at +8, name at +0x10): the whole .wav or .mp3
    // file from the .iwd archives, written as it is under sounds/<dir>/<name>
    void IwDiskZone::ExportStreamedSounds(const DiskGame::Context& C, DiskGame::Totals& T)
    {
        auto& Kt = T.Kinds[DiskGame::Kind::Sound];
        std::set<std::string> Wanted;
        for (const auto& I : Walk().Instances)
        {
            if (!OwnerIs(I, "sound")) continue;
            const int64_t Head = Off(At<uint64_t>(Image(), I.Pos + 8));
            const uint32_t Count = At<uint32_t>(Image(), I.Pos + 0x10);
            for (uint32_t K = 0; Head >= 0 && K < Count && K < 4096; K++)
            {
                const int64_t File = Off(At<uint64_t>(Image(), Head + (int64_t)K * Game.Readers->AliasSize + 0x28));
                if (File < 0 || At<uint8_t>(Image(), File) == 1) continue;
                const std::string Dir = StringAt(At<uint64_t>(Image(), File + 8));
                const std::string FileName = StringAt(At<uint64_t>(Image(), File + 0x10));
                if (FileName.empty()) continue;
                const std::string Rel = Dir.empty() ? FileName : Dir + "/" + FileName;
                if (C.Wants(DiskGame::Kind::Sound, fs::path(Rel).replace_extension().generic_string())) Wanted.insert(Rel);
            }
        }
        if (Wanted.empty()) return;
        const std::string Main = MainFolder(FastFilePath());
        const StreamIndex* Index = Main.empty() ? nullptr : StreamIndexOf(Main);
        const fs::path Out = C.KindDir(DiskGame::Kind::Sound);
        std::map<uint32_t, std::vector<std::pair<uint32_t, std::string>>> ByArchive;
        for (const auto& Rel : Wanted)
        {
            const auto It = Index ? Index->Entries.find(ExportNaming::Lower("sound/" + Rel)) : decltype(Index->Entries.end()){};
            if (Index == nullptr || It == Index->Entries.end())
            {
                Kt.Seen++;
                T.Fail(Name(), DiskGame::Kind::Sound, Rel, "streamed file in no .iwd");
                continue;
            }
            ByArchive[It->second.first].emplace_back(It->second.second, Rel);
        }
        size_t Done = 0;
        const std::string Format = ExportWriters::SoundFormatSetting();
        for (auto& [Archive, Files] : ByArchive)
        {
            mz_zip_archive Zip;
            std::memset(&Zip, 0, sizeof(Zip));
            if (!mz_zip_reader_init_file(&Zip, Index->Archives[Archive].c_str(), 0)) continue;
            std::sort(Files.begin(), Files.end());
            for (const auto& [Entry, Rel] : Files)
            {
                if (!C.Advance(Done++, Wanted.size(), Rel)) break;
                Kt.Seen++;
                const fs::path P = Out / fs::path(ExportWriters::SoundFileName(Rel, Format)).make_preferred();
                std::error_code Ec;
                if (fs::exists(P, Ec)) { Kt.Known++; continue; }
                size_t Size = 0;
                void* Data = mz_zip_reader_extract_to_heap(&Zip, Entry, &Size, 0);
                std::string Why = "streamed file not extracted";
                const bool Ok = Data != nullptr &&
                                ExportWriters::WriteSound(ExportWriters::SoundSource::OfFile(static_cast<const uint8_t*>(Data), Size, Rel), P, Format, &Why);
                if (Data != nullptr) mz_free(Data);
                if (Ok) { Kt.Written++; Counter("sounds_streamed")++; }
                else T.Fail(Name(), DiskGame::Kind::Sound, Rel, Why);
            }
            mz_zip_reader_end(&Zip);
        }
    }

    bool IwDiskZone::Payload(uint64_t Pointer, uint32_t N, bool Packed, uint32_t Size, std::vector<uint8_t>& Out) const
    {
        const int64_t From = Off(Pointer);
        if (From < 0 || N == 0 || (uint64_t)From + N > Image().size()) return false;
        const uint8_t* Data = Image().data() + From;
        if (!Packed)
        {
            Out.assign(Data, Data + N);
            return true;
        }
        Out.resize(Size);
        mz_ulong Got = Size;
        if (mz_uncompress(Out.data(), &Got, Data, N) != MZ_OK) return false;
        Out.resize(Got);
        return true;
    }

    namespace
    {
        bool WriteFile(const fs::path& Dir, const std::string& File, const std::vector<uint8_t>& Bytes)
        {
            return ExportWriters::WriteBytes(Dir / fs::path(File).make_preferred(), Bytes.data(), Bytes.size());
        }
    }

    bool IwDiskZone::RawfileBytes(int64_t Pos, std::vector<uint8_t>& Bytes) const
    {
        const uint32_t Packed = At<uint32_t>(Image(), Pos + 8), Len = At<uint32_t>(Image(), Pos + 0xc);
        if (Packed == 0 && Len == 0) return true;   // an empty file (code_post_gfx's own)
        if (!Payload(At<uint64_t>(Image(), Pos + 0x10), Packed ? Packed : Len, Packed != 0, Len, Bytes)) return false;
        if (Bytes.size() > Len) Bytes.resize(Len);
        return true;
    }

    void IwDiskZone::Documents(std::vector<std::pair<std::string, std::string>>& Out) const
    {
        ZonekitZone::Documents(Out);
        for (const auto& I : Walk().Instances)
        {
            if (I.Parent >= 0) continue;
            const bool Script = OwnerIs(I, "scriptfile");
            if (!Script && !OwnerIs(I, "rawfile")) continue;
            const std::string Nm = StringAt(At<uint64_t>(Image(), I.Pos)), L = ExportNaming::Lower(Nm);
            std::vector<uint8_t> Bytes;
            if (Script)
            {
                // MW3's compiled script: its strings (the buffer the rawfile kind writes as <name>.gscsource)
                const uint32_t Packed = At<uint32_t>(Image(), I.Pos + 8), Len = At<uint32_t>(Image(), I.Pos + 0xc);
                if (Packed != 0 && !IsReference(DiskGame::Kind::Rawfile, Nm, I.Pos) &&
                    Payload(At<uint64_t>(Image(), I.Pos + 0x18), Packed, true, Len, Bytes))
                    Out.emplace_back(Nm + ".gscsource", std::string(Bytes.begin(), Bytes.end()));
                continue;
            }
            // the map list (.arena), the GSC source and animtrees the anim owners are read from
            // (Games/IwClassic/IwClassicAnimOwners.h)
            const bool Wanted = L.ends_with(".arena") || L.ends_with(".gsc") || L.ends_with(".csc") || L.ends_with(".atr");
            if (Wanted && !IsReference(DiskGame::Kind::Rawfile, Nm, I.Pos) && RawfileBytes(I.Pos, Bytes))
                Out.emplace_back(Nm, std::string(Bytes.begin(), Bytes.end()));
        }
    }

    // rawfile: name 0, compressedLen 8, len 0xc, buffer 0x10 (zlib when compressedLen != 0); MW3's
    // scriptfile: name 0, compressedLen 8, len 0xc, bytecodeLen 0x10, buffer 0x18, bytecode 0x20
    void IwDiskZone::ExportRawfiles(const DiskGame::Context& C, DiskGame::Totals& T)
    {
        auto& Kt = T.Kinds[DiskGame::Kind::Rawfile];
        const fs::path Dir = C.KindDir(DiskGame::Kind::Rawfile);
        std::vector<std::pair<int64_t, bool>> Found;
        for (const auto& I : Walk().Instances)
            if (I.Parent < 0 && (OwnerIs(I, "rawfile") || OwnerIs(I, "scriptfile")))
                Found.emplace_back(I.Pos, OwnerIs(I, "scriptfile"));
        for (size_t N = 0; N < Found.size(); N++)
        {
            const auto [Pos, Script] = Found[N];
            const std::string Nm = StringAt(At<uint64_t>(Image(), Pos));
            if (!C.Advance(N, Found.size(), Nm)) break;
            if (IsReference(DiskGame::Kind::Rawfile, Nm, Pos)) { Kt.References++; continue; }
            if (!C.Wants(DiskGame::Kind::Rawfile, Nm)) continue;
            Kt.Seen++;
            const uint32_t Packed = At<uint32_t>(Image(), Pos + 8), Len = At<uint32_t>(Image(), Pos + 0xc);
            std::vector<uint8_t> Bytes;
            bool Ok = false;
            if (!Script)
                Ok = RawfileBytes(Pos, Bytes) && WriteFile(Dir, Nm, Bytes);
            else
            {
                std::vector<uint8_t> Code;
                const uint32_t CodeLen = At<uint32_t>(Image(), Pos + 0x10);
                Ok = Packed == 0 || (Payload(At<uint64_t>(Image(), Pos + 0x18), Packed, true, Len, Bytes) &&
                                     WriteFile(Dir, Nm + ".gscsource", Bytes));
                Ok = Ok && (CodeLen == 0 || (Payload(At<uint64_t>(Image(), Pos + 0x20), CodeLen, false, 0, Code) &&
                                             WriteFile(Dir, Nm + ".gscbin", Code)));
            }
            if (Ok) Kt.Written++;
            else T.Fail(Name(), DiskGame::Kind::Rawfile, Nm, "payload not read");
        }
    }

    // stringtable: name 0, columnCount 8, rowCount 0xc, values 0x10 (16 bytes: string, hash): a CSV
    void IwDiskZone::ExportStringTables(const DiskGame::Context& C, DiskGame::Totals& T)
    {
        auto& Kt = T.Kinds[DiskGame::Kind::Data];
        const fs::path Dir = C.KindDir(DiskGame::Kind::Data);
        std::vector<int64_t> Found;
        for (const auto& I : Walk().Instances)
            if (OwnerIs(I, "stringtable")) Found.push_back(I.Pos);
        for (size_t N = 0; N < Found.size(); N++)
        {
            const int64_t Pos = Found[N];
            const std::string Nm = StringAt(At<uint64_t>(Image(), Pos));
            if (!C.Advance(N, Found.size(), Nm)) break;
            if (IsReference(DiskGame::Kind::Data, Nm, Pos)) { Kt.References++; continue; }
            if (!C.Wants(DiskGame::Kind::Data, Nm)) continue;
            Kt.Seen++;
            const int32_t Cols = At<int32_t>(Image(), Pos + 8), Rows = At<int32_t>(Image(), Pos + 0xc);
            const int64_t Values = Off(At<uint64_t>(Image(), Pos + 0x10));
            std::string Csv;
            for (int32_t R = 0; Values >= 0 && R < Rows; R++)
            {
                for (int32_t K = 0; K < Cols; K++)
                {
                    if (K) Csv += ',';
                    Csv += StringAt(At<uint64_t>(Image(), Values + ((int64_t)R * Cols + K) * 16));
                }
                Csv += '\n';
            }
            if (WriteFile(Dir, Nm, std::vector<uint8_t>(Csv.begin(), Csv.end()))) Kt.Written++;
            else T.Fail(Name(), DiskGame::Kind::Data, Nm, "write failed");
        }
    }


    bool IwDiskGame::Zones(const std::string& Input, const DiskGame::ExportOptions& Options, std::vector<std::string>& Out,
                           std::string& Error) const
    {
        std::vector<std::string> All;
        if (!IDiskGame::Zones(Input, Options, All, Error)) return false;
        for (const auto& P : All)
            if (Readable(P)) Out.push_back(P);
        if (Out.empty()) Error = "no zone of this game in " + Input;
        return !Out.empty();
    }

    bool IwDiskGame::ListZones(const std::string& Install, const DiskGame::ExportOptions& Options, std::vector<std::string>& Names,
                               std::string& Error) const
    {
        std::vector<std::string> All;
        if (!IDiskGame::ListZones(Install, Options, All, Error)) return false;
        for (const auto& N : All)
            if (Readable(fs::path(Install) / N)) Names.push_back(N);
        if (Names.empty()) Error = "no zone of this game in " + Install;
        return !Names.empty();
    }

    std::unique_ptr<DiskGame::IDiskZone> IwDiskGame::Open(const std::string& Path, const DiskGame::ExportOptions& Options,
                                                          std::string& Error) const
    {
        auto Zone = std::make_unique<IwDiskZone>(Path, Game);
        if (!Options.Profile.empty()) Zone->SetProfile(Options.Profile);
        Zone->SetInstall(this, Options, !Options.Install.empty() ? Options.Install : InstallOfZone(Path));
        if (!Zone->Open(Error)) return nullptr;
        return Zone;
    }

    std::vector<std::string> IwDiskGame::AnimOwnerDocuments() const
    {
        return IwClassicAnimOwners::Documents();
    }

    void IwDiskGame::AnimOwners(const DiskGame::ExportOptions& Options, const std::vector<AnimZone>& Zones, ::AnimOwners::Book& Out) const
    {
        IwClassicAnimOwners::Fill(*this, Options, Zones, Out);
    }
}
