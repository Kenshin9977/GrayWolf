#include "stdafx.h"

// Modern Warfare 4 on the disk template: a ZonekitZone with the mw4 profile, served to its readers
// (GameMW4.h). A zone's models, images, sounds and anims on their own; the state and
// what is left: graywolf-tools/docs/games/MW4.md.

#include "CoDAssets.h"
#include "DiskGame.h"
#include "ExportWriters.h"
#include "FileSystems.h"
#include "Strings.h"
#include "XSUBCache.h"

#include "GameMW4.h"
#include "Games/BO7/GameBlackOps7.h"
#include "Games/IW/IWZone.h"
#include "Games/IW/IwctxReaders.h"
#include "Games/IW/IwLanguageNames.h"
#include "Games/Treyarch/TreyarchPackages.h"

#include <cstring>

namespace
{
    // XModel (0xd8): its name at 8, the LODs' pointer 0x90 (u8 0x12 records of 0x48: the
    // xmodelsurfs at 0, the surfaces the game copies from it at 8); XModelSurfs keeps them at 8.
    // Black Ops 7's offsets: the mw4 tables read the same fields (xmodel 0x211f430, lods 0x48)
    constexpr int64_t ModelName = 8, ModelLodCount = 0x12, ModelLods = 0x90, LodSize = 0x48, LodSurfs = 8, ModelSurfsSurfs = 8;

    class MW4Zone final : public IWZone
    {
    public:
        explicit MW4Zone(const std::string& Path) : IWZone(Path)
        {
            using namespace DiskGame;
            Exports(Kind::Image, [this](const Context& C, Totals& T) { ExportImages(C, T); });
            Exports(Kind::Sound, [this](const Context& C, Totals& T) { ExportSounds(C, T); });
        }

    protected:
        const char* Profile() const override { return "mw4"; }
        bool OpensPartially() const override { return true; }
        // its images' records (IwctxReaders' layout, Black Ops 7's) served from the zone's heads
        HeadSpec Heads() const override { return ModernIwHeads(); }
        void Weigh(DiskGame::Kind K, int64_t Pos, DiskGame::Weight& W) const override
        {
            if (K == DiskGame::Kind::Image) W = ModernIwImageWeight(Pos, GameBlackOps7::DxgiFormat);
        }
        const ICoDGame& LiveGame() const override { return LiveGames::Get("MW4"); }
        VirtualGameSession::Handlers Handlers() override
        {
            // the package index's names: HashIWAsset is Black Ops 7's FNV-1a 64 (the xmodels' hash at 0
            // is it of the name at 8, 130 of 130 on ingame_mp), so the Black Ops 7 and 6 packages name
            // the assets this game shares with them (its sounds most: ingame_mp 538 of 6 919)
            IwctxReaders::LoadNameIndexes();
            IwctxReaders::LoadGameNames({ "bo7_names.wni", "bo6_names.wni" });
            return { SlotString, DiskImage };
        }

        void Fixups() override
        {
            // the zone's script strings (notetrack names), decrypted with the cipher fitted from the
            // install's zones (CipherStrings)
            std::string Error;
            if (const auto* Tables = ExportWriters::ZoneProfile(Profile(), Error))
                IwctxReaders::RegisterScriptStrings(IwctxReaders::ZoneStrings(Zone(), Tables->Profile.Dir));
            Counter("lods_without_surfaces") += LinkLods();
        }

        // a model by the name at 8 ("a::b" as a_b), anything else by its hash at 0
        std::string AssetName(DiskGame::Kind K, int64_t Pos) const override
        {
            const uint64_t Hash = At<uint64_t>(Image(), Pos);
            if (K == DiskGame::Kind::Model)
            {
                const std::string N = At<uint64_t>(Image(), Pos + ModelName) != 0 ? StringAt(PointerAt(Pos + ModelName)) : std::string();
                if (N.empty()) return GameBlackOps7::HashName("xmodel", Hash);
                return Strings::Replace(N, "::", "_");
            }
            if (K == DiskGame::Kind::Image) return GameBlackOps7::HashName("ximage", Hash);
            if (K == DiskGame::Kind::Anim) return GameBlackOps7::HashName("xanim", Hash);
            return GameBlackOps7::HashName("xasset", Hash);
        }
        // every asset of a zone's list is inline here: references are by hash, never a header
        bool IsReference(DiskGame::Kind, const std::string&, int64_t) const override { return false; }
        void DefinesAt(std::vector<std::pair<DiskGame::AssetId, int64_t>>& Out) const override
        {
            ZonekitZone::DefinesAt(Out);
            DefineImages(Out);
        }

    private:
        VirtualGameSession& Live() const { return const_cast<MW4Zone*>(this)->ZonekitZone::Live(); }
        // the bytes at a served address of this zone, null when N bytes are not all here
        const uint8_t* Locate(uint64_t Address, uint64_t N) const
        {
            if (Live().Contains(Address) && Address - Live().Origin() + N <= Image().size()) return Image().data() + (Address - Live().Origin());
            return nullptr;
        }
        template <class T>
        T Get(uint64_t Address) const
        {
            T V{};
            if (const uint8_t* P = Locate(Address, sizeof(T))) std::memcpy(&V, P, sizeof(T));
            return V;
        }

        // the game's load-time copy lod.surfs = lod.modelSurfs->surfs (DB_ModelFixup), for every
        // LOD whose xmodelsurfs this zone holds; the others are left without surfaces (counted)
        size_t LinkLods()
        {
            size_t Unlinked = 0;
            for (const auto& I : Walk().Instances)
            {
                if (!OwnerIs(I, "xmodel")) continue;
                const uint64_t Model = Live().Addr((uint64_t)I.Pos);
                const uint64_t Lods = Get<uint64_t>(Model + ModelLods);
                if (Locate(Lods, LodSize) == nullptr) continue;
                const int64_t N = Get<uint8_t>(Model + ModelLodCount);
                for (int64_t L = 0; L < N; L++)
                {
                    const uint64_t Lod = Lods + (uint64_t)(L * LodSize);
                    const uint64_t Surfs = Get<uint64_t>(Lod);
                    const uint64_t Data = Locate(Surfs, 0x48) ? Get<uint64_t>(Surfs + ModelSurfsSurfs) : 0;
                    if (Locate(Data, 1) == nullptr)
                    {
                        Unlinked++;
                        continue;
                    }
                    Live().Poke(Lod + LodSurfs - Live().Origin(), Data);
                }
            }
            return Unlinked;
        }

        static std::unique_ptr<XImageDDS> DiskImage(const XImage_t& Image)
        {
            return ServedZone<MW4Zone>() == nullptr ? nullptr : GameBlackOps7::LoadXImage(Image);
        }

        // image (0x40, 0x1c8f000): Black Ops 7's GfxImage (format u8 0x2a, mips u8 0x2e & 0xf, 24-byte
        // mips 0x30, in-zone pixels 0x38 of u32 0x18 bytes)
        void ExportImages(const DiskGame::Context& C, DiskGame::Totals& T)
        {
            ImageList L;
            for (const auto& I : Walk().Instances)
                if (OwnerIs(I, "image")) L.Order.push_back(I.Pos);
            L.NameAt = 0;
            // the header past its hash (format, sizes, the mips' package keys), the in-zone pixels, the extension
            L.KeyOf = [this](int64_t Pos, const std::string& Extension) {
                uint64_t H = 0xcbf29ce484222325ull;
                const auto Mix = [&H](const uint8_t* P, uint64_t N) {
                    for (uint64_t K = 0; P != nullptr && K < N; K++) H = (H ^ P[K]) * 0x100000001b3ull;
                };
                const uint64_t Address = Live().Addr((uint64_t)Pos);
                Mix(Locate(Address + 8, 0x38), 0x38);
                const uint64_t MipCount = Get<uint8_t>(Address + 0x2e) & 0xF;
                Mix(Locate(Get<uint64_t>(Address + 0x30), MipCount * 24), MipCount * 24);
                const uint32_t Size = Get<uint32_t>(Address + 0x18);
                Mix(Locate(Get<uint64_t>(Address + 0x38), Size), Size);
                Mix((const uint8_t*)Extension.data(), Extension.size());
                return H ? H : 1;
            };
            L.Decode = [this](int64_t Pos, const std::string& Name) {
                return GameBlackOps7::LoadXImage(XImage_t(ImageUsageType::Unknown, 0, Live().Addr((uint64_t)Pos), Name));
            };
            L.FailWhy = [this](int64_t Pos) {
                return std::string(Get<uint64_t>(Live().Addr((uint64_t)Pos) + 0x38) != 0 ? "in zone" : "streamed");
            };
            L.OnePerName = true;
            ExportImageList(C, T, L);
        }

        // sndasset (0x40, 0x1d24110): named by its hash (and its language, IwLanguageNames: Black Ops
        // 7's language table until this exe's is read), decoded by GameMW4::ReadXSound
        void ExportSounds(const DiskGame::Context& C, DiskGame::Totals& T)
        {
            static IwLanguageNames ByLanguage({ std::begin(GameBlackOps7::Languages), std::end(GameBlackOps7::Languages) });
            std::vector<IwctxReaders::SoundEntry> Found;
            std::vector<uint64_t> Hashes;
            for (const auto& I : Walk().Instances)
                if (OwnerIs(I, "sndasset"))
                {
                    Hashes.push_back(At<uint64_t>(Image(), I.Pos));
                    Found.push_back({ GameBlackOps7::HashName("xsound", Hashes.back()), Live().Addr((uint64_t)I.Pos),
                                      At<uint16_t>(Image(), I.Pos + GameMW4::SoundRate),
                                      At<uint32_t>(Image(), I.Pos + GameMW4::SoundFrames) });
                }
            if (const size_t N = ByLanguage.Rename(Hashes, Found)) Counter("sounds_named_by_language") += N;
            IwctxReaders::ExportSounds(C, T, Name(), Found);
        }
    };

    class ModernWarfare4Game final : public DiskGame::IDiskGame
    {
    public:
        const char* Id() const override { return "mw4"; }
        const char* Title() const override { return "Call of Duty: Modern Warfare 4"; }
        const char* Engine() const override { return "MW4"; }
        std::unique_ptr<DiskGame::IDiskZone> Unopened() const override { return std::make_unique<MW4Zone>(std::string()); }
        bool IndexInParallel() const override { return true; }
        // the string cipher's strings (DiskGame::EnsureStringCipher): the zones' script strings, as
        // Black Ops 7's (its XAssetList layout)
        const DiskGame::CipherSource* CipherStrings() const override
        {
            static const DiskGame::CipherSource Multiplayer = IwctxReaders::ScriptStringCipher("mw4");
            return &Multiplayer;
        }

        std::unique_ptr<DiskGame::IDiskZone> Open(const std::string& Path, const DiskGame::ExportOptions& Options,
                                                  std::string& Error) const override
        {
            // the packages the readers stream from (--install: the cod26 folder, or the HQ install
            // for cod26_shared's too)
            auto Packages = Options.Install.empty() ? nullptr : TreyarchPackages::Open<XSUBCache>(Options.Install);
            if (!Options.Install.empty() && Packages == nullptr)
            {
                Error = "could not open the packages of " + Options.Install;
                return nullptr;
            }
            auto Zone = std::make_unique<MW4Zone>(Path);
            Zone->ServesPackages(std::move(Packages));
            Zone->SetInstall(this, Options, Options.Install);
            if (!Zone->Open(Error)) return nullptr;
            return Zone;
        }
    };

    const ModernWarfare4Game Game;
    const DiskGame::Registration Registered(Game);
}
