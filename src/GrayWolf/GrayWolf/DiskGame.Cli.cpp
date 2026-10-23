#include "stdafx.h"

#include "DiskGameCli.h"
#include "DiskGameSizes.h"
#include "ExportNaming.h"

#include "CliOptions.h"
#include "CliSupport.h"
#include "GameCommand.h"
#include "SettingsManager.h"

#include <CLI/CLI.hpp>

#include <chrono>
#include <filesystem>

namespace fs = std::filesystem;

namespace DiskGame
{
namespace
{
    // The flags that set a game's Setting, by its key.
    struct SettingFlag
    {
        const char* Key;
        const char* Flag;
        std::string CliOptions::*Value;
    };
    const SettingFlag SettingFlags[] = {
        { "ui_res", "--ui-res", &CliOptions::UiRes },
        { "map_png", "--map-png", &CliOptions::MapPng },
        { "map_textures", "--map-textures", &CliOptions::MapTextures },
        { "map_cast", "--map-cast", &CliOptions::MapCast },
        { "map_radiant", "--map-radiant", &CliOptions::MapRadiant },
        { "material_gdt", "--material-gdt", &CliOptions::MaterialGdt },
        { "sound_aliases", "--sound-aliases", &CliOptions::SoundAliases },
        { "map_collision", "--map-collision", &CliOptions::MapCollision },
        { "map_content", "--map-content", &CliOptions::MapContent },
        { "language", "--language", &CliOptions::Language },
        { "decompile", "--decompile", &CliOptions::Decompile },
    };

    bool ReadOptions(const IDiskGame& Game, const CliOptions& Cli, ExportOptions& O, std::string& Error)
    {
        O.Install = Cli.Install;
        O.Cache = Cli.Cache;
        O.Profile = Cli.ZoneWalkProfile;
        O.AudioFormat = Cli.AudioFormat;
        O.ActsExe = Cli.ActsExe;
        O.GscToolExe = Cli.GscToolExe;
        O.GameExe = Cli.GameExe;
        O.OatModelFormat = Cli.OatModelFmt;
        O.OatImageFormat = Cli.OatImageFmt;
        O.OatGdt = Cli.OatGdt;
        O.Threads = Cli.Threads;
        for (const Setting* S : Game.AllSettings())
            for (const SettingFlag& F : SettingFlags)
            {
                const std::string& Text = Cli.*F.Value;
                if (std::string(F.Key) != S->Key || Text.empty()) continue;
                if (!S->Parse(Text, O.Settings[S->Key]))
                {
                    std::string Values;
                    for (const auto& [V, Label] : S->Values)
                        Values += std::string(Values.empty() ? "" : ", ") + V + " (" + Label + ")";
                    Error = std::string("unknown ") + F.Flag + " " + Text + ": " + Values;
                    return false;
                }
            }
        return true;
    }
}

int Run(const IDiskGame& Game, const CliOptions& Options, const std::string& Input)
{
    const auto Fail = [&](const std::string& Why) {
        CliReport(Options, "result=error\nreason=" + Why + "\n");
        return 1;
    };
    if (Options.Out.empty()) return Fail("disk export needs --out");
    ApplyExportOverrides(Options);
    if (Options.ImageFormat.empty() && Game.DefaultImageFormat())
        SettingsManager::SetRunSetting("exportimg", Game.DefaultImageFormat());

    ExportOptions O;
    Context C;
    C.OutRoot = (fs::path(Options.Out) / Game.Id()).string();
    C.NameFilter = Options.NameFilter;
    std::string Error;
    if (!ReadOptions(Game, Options, O, Error) || !ParseKinds(Options.Type, C.Kinds, Error)) return Fail(Error);
    O.ZoneAlone = std::all_of(C.Kinds.begin(), C.Kinds.end(), [](Kind K) { return K == Kind::MaterialLinks || K == Kind::SoundLinks || K == Kind::AssetRefs || K == Kind::Techsets || K == Kind::ScriptBundles || K == Kind::Weapons || K == Kind::WeaponStats || K == Kind::MapInfo || K == Kind::Scriptables || K == Kind::MapEnts || K == Kind::AiTypes; }) && !C.Kinds.empty();
    std::vector<std::string> Zones;
    const auto Start = std::chrono::steady_clock::now();
    if (!Game.Zones(Input, O, Zones, Error)) return Fail(Error);
    C.ResolveSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
    const Totals T = Export(Game, O, Zones, C);
    const bool Ignored = !C.NameFilter.empty() && !Game.FiltersByName();
    CliReport(Options, std::string("result=ok\ngame=") + Game.Id() + "\n" + (Ignored ? "name_filter=ignored\n" : "") + Report(T));
    return 0;
}

namespace
{
    // level-names <install> --game <id>: what the Load Game panel shows as each zone's level name
    // (DiskGame::LevelNames, built or read from its kept file), one "zone=<name>\t<level>" line
    // per zone, named or not
    int RunLevelNames(const CliOptions& Options, const std::string& Install)
    {
        const IDiskGame* Game = Options.DiskGameId.empty() ? Detect(Install) : Find(Options.DiskGameId);
        if (Game == nullptr)
        {
            CliReport(Options, "result=error\nreason=no disk reader for " + Install + "\n");
            return 1;
        }
        ExportOptions O;
        std::string Error;
        if (!ReadOptions(*Game, Options, O, Error))
        {
            CliReport(Options, "result=error\nreason=" + Error + "\n");
            return 1;
        }
        if (O.Install.empty()) O.Install = Install;
        std::vector<std::string> Zones;
        if (!Game->ListZones(Install, O, Zones, Error))
        {
            CliReport(Options, "result=error\nreason=" + Error + "\n");
            return 1;
        }
        const auto Start = std::chrono::steady_clock::now();
        const auto Names = LevelNames::For(*Game, O, Install, nullptr, Error);
        const double Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
        std::string Lines;
        for (const auto& Z : Zones) Lines += "zone=" + Z + "\t" + (Names ? Names->Of(Z) : std::string()) + "\n";
        CliReport(Options, std::string("result=ok\ngame=") + Game->Id() + "\nzones=" + std::to_string(Zones.size()) +
                               "\nnamed=" + std::to_string(Names ? Names->Named() : 0) + "\nbuilt=" + (Names && Names->Built ? "1" : "0") +
                               "\nseconds=" + std::to_string(Seconds) + (Error.empty() ? "" : "\nerror=" + Error) + "\n" + Lines);
        return 0;
    }

    void AddLevelNamesOptions(CLI::App& Sub, CliOptions& Options)
    {
        Sub.add_option("--game", Options.DiskGameId, "The game; detected from the install when omitted");
    }

    const GameCommand LevelNamesCommand{
        "level-names",
        "The display names of an install's levels, read from the game's own strings (what the Load Game panel shows)",
        "The install folder",
        RunLevelNames,
        AddLevelNamesOptions,
    };
    const GameCommandRegistration LevelNamesRegistered(LevelNamesCommand);

    // disk-index <install> --game <id>: the install index (DiskGame::ZoneIndex) read from its kept
    // file, or built (--threads workers, resumed from its partial file when one was stopped);
    // what an export pulling dependencies and the Load Game panel use
    int RunDiskIndex(const CliOptions& Options, const std::string& Install)
    {
        const IDiskGame* Game = Options.DiskGameId.empty() ? Detect(Install) : Find(Options.DiskGameId);
        if (Game == nullptr)
        {
            CliReport(Options, "result=error\nreason=no disk reader for " + Install + "\n");
            return 1;
        }
        ExportOptions O;
        std::string Error;
        if (!ReadOptions(*Game, Options, O, Error))
        {
            CliReport(Options, "result=error\nreason=" + Error + "\n");
            return 1;
        }
        if (O.Install.empty()) O.Install = Install;
        const auto Ix = ZoneIndex::Build(*Game, O, Install, nullptr, nullptr, Error);
        if (!Ix)
        {
            CliReport(Options, "result=error\nreason=" + Error + "\n");
            return 1;
        }
        size_t Opened = 0;
        for (size_t Z = 0; Z < Ix->Zones().size(); Z++) Opened += Ix->Footprint(Z) != 0;
        CliReport(Options, std::string("result=ok\ngame=") + Game->Id() + "\nzones=" + std::to_string(Ix->Zones().size()) +
                               "\nopened=" + std::to_string(Opened) + "\nassets=" + std::to_string(Ix->Entries) +
                               "\nbuilt=" + (Ix->Built ? "1" : "0") + "\nseconds=" + std::to_string(Ix->Seconds) + "\nfile=" + Ix->File + "\n");
        return 0;
    }

    const GameCommand DiskIndexCommand{
        "disk-index",
        "The install index of a game (where each asset is defined), read or built",
        "The install folder",
        RunDiskIndex,
        AddLevelNamesOptions,
    };
    const GameCommandRegistration DiskIndexRegistered(DiskIndexCommand);

    // disk-estimate <install> --game <id> --picked a,b [--type kinds]: what the Load Game panel's
    // size estimate says for those zones and kinds (DiskGame::Sizes): samples measured from the
    // picked zones, then counts times their sizes; each kind's bytes and basis
    int RunDiskEstimate(const CliOptions& Options, const std::string& Install)
    {
        const auto Fail = [&](const std::string& Why) {
            CliReport(Options, "result=error\nreason=" + Why + "\n");
            return 1;
        };
        const IDiskGame* Game = Options.DiskGameId.empty() ? Detect(Install) : Find(Options.DiskGameId);
        if (Game == nullptr) return Fail("no disk reader for " + Install);
        ApplyExportOverrides(Options);
        ExportOptions O;
        std::set<Kind> Kinds;
        std::string Error;
        if (!ReadOptions(*Game, Options, O, Error) || !ParseKinds(Options.Type, Kinds, Error)) return Fail(Error);
        if (O.Install.empty()) O.Install = Install;
        // the kept index whatever its tables (what the panel counts with), else built
        auto Ix = ZoneIndex::Kept(*Game, O, Install);
        if (!Ix) Ix = ZoneIndex::Build(*Game, O, Install, nullptr, nullptr, Error);
        if (!Ix) return Fail(Error);
        std::vector<size_t> Picked;
        for (const auto& Name : SplitNameFilter(Options.EstimateZones))
            for (size_t Z = 0; Z < Ix->Zones().size(); Z++)
                if (ExportNaming::Lower(Ix->Zones()[Z]) == Name || ExportNaming::Lower(fs::path(Ix->Zones()[Z]).stem().string()) == Name)
                    Picked.push_back(Z);
        if (Picked.empty()) return Fail("--picked names none of the install's zones");
        // the picked zones that are levels, as the panel knows them (their level names)
        std::set<size_t> Levels;
        std::string NoNames;
        const auto Named = Game->LevelNames() ? LevelNames::For(*Game, O, Install, nullptr, NoNames) : nullptr;
        if (Named)
            for (size_t Z : Picked)
                if (!Named->Of(Ix->Zones()[Z]).empty()) Levels.insert(Z);
        const auto Start = std::chrono::steady_clock::now();
        Sizes::Calibration C;
        // the sounds' headers read by an earlier count are kept (not read again)
        Sizes::Calibration Kept;
        const bool HasKept = Sizes::Load(KeptFile(*Game, O, Install, ".sizes"), Kept);
        if (!Options.EstimateKept) C = Sizes::Measure(*Game, O, Install, *Ix, Picked, Levels, Kinds, nullptr, nullptr, HasKept ? &Kept : nullptr);
        else if (!HasKept) return Fail("no samples kept for this install");
        else C = std::move(Kept);
        const double Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
        if (!Options.EstimateKept) Sizes::Save(KeptFile(*Game, O, Install, ".sizes"), C);

        Sizes::Picks S;
        // the kinds the game exports (the panel offers no other)
        const std::set<Kind> Exported = Game->Kinds();
        for (Kind K : Kinds)
            if (Exported.count(K)) S.Kinds.insert(K);
        S.Settings = O.Settings;
        S.Formats = Sizes::FormatsNow();
        S.Zones = Picked.size();
        for (size_t Z : Picked)
        {
            for (const auto& A : Ix->Defined(Z)) S.Counts[A.K]++;
            S.Footprint += Ix->Footprint(Z);
            S.Picked.emplace_back(Ix->Zones()[Z], Ix->Footprint(Z));
            if (Levels.count(Z)) S.Levels++, S.LevelFootprint += Ix->Footprint(Z);
        }
        S.LevelsKnown = Named != nullptr;
        const Sizes::Estimate E = Sizes::Compute(C, S);
        std::string Lines;
        for (const auto& [K, R] : E.Kinds)
            Lines += std::string("kind_") + KindName(K) + "=" + std::to_string(R.Bytes) + (R.Approx ? " approx" : "") + (R.Unknown ? " unknown" : "") +
                     " " + R.Basis + "\n";
        for (const auto& [K, M] : C.Kinds)
            Lines += std::string("sample_") + KindName(K) + "=" + std::to_string(M.Assets) + " assets from " + M.Zone + " in " +
                     std::to_string(M.Seconds) + " s" + (M.Cut ? " (cut)" : "") + ", wrote " + Sizes::Text(M.Listed ? Sizes::Written(M) : M.ZoneBytes) +
                     (M.Textures ? ", textures " + Sizes::Text(M.Textures) + " (listed " + Sizes::Text(M.ListedTextures) + ")" : "") + "\n";
        CliReport(Options, std::string("result=ok\ngame=") + Game->Id() + "\nzones=" + std::to_string(Picked.size()) +
                               "\nbytes=" + std::to_string(E.Bytes) + "\ntext=" + (E.Approx ? "~" : "") + Sizes::Text(E.Bytes) +
                               "\nmeasure_seconds=" + std::to_string(Seconds) + "\n" + Lines);
        return 0;
    }

    void AddDiskEstimateOptions(CLI::App& Sub, CliOptions& Options)
    {
        Sub.add_option("--game", Options.DiskGameId, "The game; detected from the install when omitted");
        Sub.add_option("--picked", Options.EstimateZones, "The zones picked, comma-separated, as the Load Game panel lists them")->required();
        Sub.add_flag("--kept", Options.EstimateKept, "The samples the last run kept, not measured again (their formats converted to these)");
    }

    const GameCommand DiskEstimateCommand{
        "disk-estimate",
        "The size an export of some zones would write, as the Load Game panel estimates it (a sample of each kind measured)",
        "The install folder",
        RunDiskEstimate,
        AddDiskEstimateOptions,
    };
    const GameCommandRegistration DiskEstimateRegistered(DiskEstimateCommand);

    // disk-where <install> --game <id> --names a,b [--type kinds]: the zones the install index
    // (ZoneIndex, the kept file, else built) says define each asset, the smallest zone first; with
    // --name-filter instead, every indexed asset whose name holds one of its substrings
    int RunDiskWhere(const CliOptions& Options, const std::string& Install)
    {
        const auto Fail = [&](const std::string& Why) {
            CliReport(Options, "result=error\nreason=" + Why + "\n");
            return 1;
        };
        const IDiskGame* Game = Options.DiskGameId.empty() ? Detect(Install) : Find(Options.DiskGameId);
        if (Game == nullptr) return Fail("no disk reader for " + Install);
        ExportOptions O;
        std::set<Kind> Kinds;
        std::string Error;
        if (!ReadOptions(*Game, Options, O, Error)) return Fail(Error);
        if (!Options.Type.empty() && !ParseKinds(Options.Type, Kinds, Error)) return Fail(Error);
        if (O.Install.empty()) O.Install = Install;
        const std::vector<std::string> Names = SplitNameFilter(Options.WhereNames), Parts = SplitNameFilter(Options.NameFilter);
        if (Names.empty() && Parts.empty()) return Fail("no --names and no --name-filter");
        // the kept index whatever its tables (as disk-estimate), else built
        auto Ix = ZoneIndex::Kept(*Game, O, Install);
        if (!Ix) Ix = ZoneIndex::Build(*Game, O, Install, nullptr, nullptr, Error);
        if (!Ix) return Fail(Error);
        // each asset asked for: its zones (an asset defined in several zones: each)
        std::map<AssetId, std::vector<size_t>> Found;
        for (size_t Z = 0; Z < Ix->Zones().size(); Z++)
            for (const auto& A : Ix->Defined(Z))
            {
                if (!Kinds.empty() && !Kinds.count(A.K)) continue;
                const std::string N = ExportNaming::Lower(A.Name);
                bool Wanted = std::find(Names.begin(), Names.end(), N) != Names.end();
                for (size_t P = 0; !Wanted && P < Parts.size(); P++) Wanted = N.find(Parts[P]) != std::string::npos;
                if (Wanted) Found[A].push_back(Z);
            }
        std::string Lines;
        std::set<std::string> Seen;
        size_t Rows = 0;
        for (auto& [A, Zones] : Found)
        {
            std::stable_sort(Zones.begin(), Zones.end(), [&](size_t L, size_t R) { return Ix->Footprint(L) < Ix->Footprint(R); });
            Seen.insert(ExportNaming::Lower(A.Name));
            for (const size_t Z : Zones)
                Lines += std::string("asset=") + KindName(A.K) + "\t" + A.Name + "\t" + Ix->Zones()[Z] + "\t" + std::to_string(Ix->Footprint(Z)) + "\n", Rows++;
        }
        for (const auto& N : Names)
            if (!Seen.count(N)) Lines += "missing=" + N + "\n";
        CliReport(Options, std::string("result=ok\ngame=") + Game->Id() + "\nassets=" + std::to_string(Found.size()) + "\nrows=" +
                               std::to_string(Rows) + "\nindex=" + Ix->File + "\nbuilt=" + (Ix->Built ? "1" : "0") +
                               "\n# asset=<kind>\\t<name>\\t<zone>\\t<zone footprint, bytes>, the smallest zone first\n" + Lines);
        return 0;
    }

    void AddDiskWhereOptions(CLI::App& Sub, CliOptions& Options)
    {
        Sub.add_option("--game", Options.DiskGameId, "The game; detected from the install when omitted");
        Sub.add_option("--names", Options.WhereNames, "Asset names, comma-separated, matched whole without case (or --name-filter: substrings)");
    }

    const GameCommand DiskWhereCommand{
        "disk-where",
        "The zones defining each asset, from the install index (kept, else built as disk-index): "
        "asset=<kind> <name> <zone> <footprint> lines, the smallest zone first; --type limits the kinds",
        "The install folder",
        RunDiskWhere,
        AddDiskWhereOptions,
    };
    const GameCommandRegistration DiskWhereRegistered(DiskWhereCommand);
}
}
