#include "stdafx.h"

// The titles OpenAssetTools reads (CoD4, WaW, MW2, MW3, BO1, BO2) on the disk template: one
// OatZoneSession per zone, one OAT dump per kind. OAT writes the mod tools' raw tree (images/,
// xmodel/, model_export/, xanim/...) at <out>/<game>/, which keeps its glTF texture paths
// (../images/) and its zone_source valid; images/ is the template's own image folder.
// --name-filter does not apply: OAT dumps a kind whole (but the weapons and weaponstats kinds,
// OatWeapons.h).

#include "DiskGame.h"
#include "ExportWriters.h"
#include "OatInProcess.h"
#include "OatWeapons.h"

#include <filesystem>

namespace
{
    class OatZone final : public DiskGame::IDiskZone
    {
    public:
        OatZone(std::unique_ptr<OatZoneSession> S, const DiskGame::ExportOptions& Options, bool Maps, const char* Title)
            : Session(std::move(S)), ZoneName(Session ? Session->ZoneName() : std::string()), Weapons(Session.get())
        {
            if (!Options.OatModelFormat.empty()) Dump.ModelFormat = Options.OatModelFormat;
            if (!Options.OatImageFormat.empty()) Dump.ImageFormat = Options.OatImageFormat;
            Dump.Gdt = Options.OatGdt;
            using namespace DiskGame;
            const std::pair<Kind, const char*> Categories[] = { { Kind::Model, "model" }, { Kind::Anim, "anim" },
                { Kind::Image, "image" }, { Kind::Sound, "sound" }, { Kind::Rawfile, "rawfile" } };
            for (const auto& [K, Category] : Categories)
                Exports(K, [this, K = K, Category = Category](const Context& C, Totals& T) { DumpKind(C, T, K, Category); });
            Exports(Kind::Localize, [this](const Context& C, Totals& T) { ExportLocalize(C, T); });
            Exports(Kind::Data, [this](const Context& C, Totals& T) {
                Session->WriteZoneSource(C.OutRoot, Dump.Gdt);
                DumpKind(C, T, Kind::Data, "data");
            });
            if (Maps) Exports(Kind::Map, [this](const Context& C, Totals& T) { ExportMaps(C, T); });
            // the weapon files and the stats table read from them (OatWeapons.h)
            Exports(Kind::Weapons, [this](const Context& C, Totals& T) { OatWeapons::ExportWeapons(Weapons, ZoneName, C, T); });
            Exports(Kind::WeaponStats, [this, Title](const Context& C, Totals& T) { OatWeapons::ExportWeaponStats(Weapons, ZoneName, C, T, Title); });
        }

        const std::string& Name() const override { return ZoneName; }
        bool WalkedToEnd() const override { return true; }   // OAT loads a zone whole or not at all

    private:
        // OAT's .str (<language>/localizedstrings/<zone>.str), and the strings as every game writes
        // them: localize/<zone>.csv, a row per entry, in UTF-8 (the older games' code page by the
        // language the zone's path names)
        void ExportLocalize(const DiskGame::Context& C, DiskGame::Totals& T)
        {
            DumpKind(C, T, DiskGame::Kind::Localize, "localize");
            std::vector<std::pair<std::string, std::string>> Rows;
            Session->Strings(Rows);
            if (Rows.empty()) return;
            const std::string Csv = (std::filesystem::path(C.KindDir(DiskGame::Kind::Localize)) / (ZoneName + ".csv")).string();
            auto& Kt = T.Kinds[DiskGame::Kind::Localize];
            if (ExportWriters::WriteLocalize(Csv, Rows, ExportWriters::LanguageCodePage(Session->FastFile()))) Kt.Written++;
            else T.Fail(ZoneName, DiskGame::Kind::Localize, ZoneName, "write failed");
        }

        // the level as one Blender .glb (OatExportMapGlb: world mesh, props, textures, lights)
        void ExportMaps(const DiskGame::Context& C, DiskGame::Totals& T)
        {
            auto& Kt = T.Kinds[DiskGame::Kind::Map];
            if (Session->Count("gfxworld") == 0) return;
            Kt.Seen++;
            const std::string Glb = (std::filesystem::path(C.KindDir(DiskGame::Kind::Map)) / (ZoneName + ".glb")).string();
            std::string Report;
            if (OatExportMapGlb(Session->FastFile(), Glb, Report)) Kt.Written++;
            else T.Fail(ZoneName, DiskGame::Kind::Map, ZoneName, Report);
        }

        void DumpKind(const DiskGame::Context& C, DiskGame::Totals& T, DiskGame::Kind K, const char* Category)
        {
            OatDumpCounts N;
            const bool Ok = Session->Dump(C.OutRoot, { Category }, Dump, N);
            auto& Kt = T.Kinds[K];
            Kt.Seen += N.Assets;
            Kt.References += N.References;
            // written outside OAT's output: T6 streamed images, T5 IWD sounds, T6 sound banks
            Kt.Written += N.Files + (K == DiskGame::Kind::Image ? N.StreamedImages : 0);
            if (K == DiskGame::Kind::Sound)
                Kt.Written += Session->GameShortName() == "T5" ? N.Sounds : N.NewSoundFiles;
            if (!Ok) T.Fail(ZoneName, K, "", "OAT dump failed");
        }

        std::unique_ptr<OatZoneSession> Session;
        std::string ZoneName;
        OatWeapons::Source Weapons;
        OatDumpOptions Dump;
    };

    class OatGame final : public DiskGame::IDiskGame
    {
    public:
        OatGame(const char* Id, const char* Title, const char* Engine, bool Maps)
            : GameId(Id), GameTitle(Title), GameEngine(Engine), Maps(Maps) {}

        const char* Id() const override { return GameId; }
        const char* Title() const override { return GameTitle; }
        const char* Engine() const override { return GameEngine; }
        bool FiltersByName() const override { return false; }
        std::unique_ptr<DiskGame::IDiskZone> Unopened() const override
        {
            return std::make_unique<OatZone>(nullptr, DiskGame::ExportOptions(), Maps, GameTitle);
        }

        std::unique_ptr<DiskGame::IDiskZone> Open(const std::string& Path, const DiskGame::ExportOptions& Options, std::string& Error) const override
        {
            auto S = OatZoneSession::Open(Path, Error);
            if (!S) return nullptr;
            return std::make_unique<OatZone>(std::move(S), Options, Maps, GameTitle);
        }

    private:
        const char* GameId;
        const char* GameTitle;
        const char* GameEngine;
        bool Maps;   // OatExportMapGlb reads T5 and T6 GfxWorlds only
    };

    // CoD4 itself is read without OAT (Games/IW3, the zonekit iw3 tables): this one stays reachable
    // as --game cod4-oat, out of the install detection (its engine is not a GameModule's)
    const OatGame Cod4("cod4-oat", "Call of Duty 4: Modern Warfare (OpenAssetTools)", "IW3-OAT", false);
    // World at War likewise (Games/T4, the zonekit t4 tables): --game waw-oat
    const OatGame Waw("waw-oat", "Call of Duty: World at War (OpenAssetTools)", "T4-OAT", false);
    // MW2 and MW3 are read without OAT too (Games/IW4, Games/IW5, the zonekit iw4/iw5 tables, signed
    // multiplayer zones included); OAT's stays as a second reader (its data tree), out of detection
    const OatGame Mw2("mw2-oat", "Call of Duty: Modern Warfare 2 (OpenAssetTools)", "IW4-OAT", false);
    const OatGame Mw3("mw3-oat", "Call of Duty: Modern Warfare 3 (OpenAssetTools)", "IW5-OAT", false);
    // Black Ops is read without OAT (Games/T5, the zonekit t5 tables): this one stays as --game
    // bo1-oat, out of the install detection
    const OatGame Bo1("bo1-oat", "Call of Duty: Black Ops (OpenAssetTools)", "T5-OAT", true);
    // Black Ops II likewise (Games/T6, the zonekit t6 tables): --game bo2-oat
    const OatGame Bo2("bo2-oat", "Call of Duty: Black Ops II (OpenAssetTools)", "T6-OAT", true);
    const DiskGame::Registration Registered[] = { DiskGame::Registration(Cod4), DiskGame::Registration(Waw),
        DiskGame::Registration(Mw2), DiskGame::Registration(Mw3), DiskGame::Registration(Bo1), DiskGame::Registration(Bo2) };
}
