#include "stdafx.h"

#include "DiskGameCli.h"
#include "DiskGameSizes.h"
#include "ExportNaming.h"

#include "CliCommands.h"
#include "CliOptions.h"
#include "CliSupport.h"
#include "GameCommand.h"
#include "SettingsManager.h"
#include "ZonekitZone.h"
#include "ZoneTableWalker.h"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <limits>
#include <map>
#include <tuple>
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
        if (!Cli.CodLuaJitExe.empty()) O.Settings["cod_luajit_decompiler_exe"] = Cli.CodLuaJitExe;
        if (!Cli.CodLuaExe.empty()) O.Settings["cod_lua_decompiler_exe"] = Cli.CodLuaExe;
        if (!Cli.FfmpegExe.empty()) O.Settings["ffmpeg_exe"] = Cli.FfmpegExe;
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
    C.Limit = Options.Limit;
    // --assets kind:name,...: only those (as the Load Game panel's picks and previews ask)
    if (!Options.Assets.empty())
    {
        auto Only = std::make_shared<Selection>();
        for (const auto& Item : SplitNameFilter(Options.Assets))
        {
            const size_t Colon = Item.find(':');
            std::set<Kind> K;
            std::string Why;
            if (Colon == std::string::npos || !ParseKinds(Item.substr(0, Colon), K, Why) || K.size() != 1)
                return Fail("--assets wants kind:name, not " + Item);
            Only->insert({ *K.begin(), Item.substr(Colon + 1) });
        }
        C.Only = std::move(Only);
    }
    std::string Error;
    if (!ReadOptions(Game, Options, O, Error) || !ParseKinds(Options.Type, C.Kinds, Error)) return Fail(Error);
    O.ZoneAlone = std::all_of(C.Kinds.begin(), C.Kinds.end(), [](Kind K) { return K == Kind::MaterialLinks || K == Kind::SoundLinks || K == Kind::AssetRefs || K == Kind::Techsets || K == Kind::ScriptBundles || K == Kind::Weapons || K == Kind::WeaponStats || K == Kind::MapInfo || K == Kind::Scriptables || K == Kind::MapEnts || K == Kind::AiTypes; }) && !C.Kinds.empty();
    std::vector<std::string> Zones;
    const auto Start = std::chrono::steady_clock::now();
    if (!Game.Zones(Input, O, Zones, Error))
    {
        // a zone named as the install lists it (ListZones, the install index), from --install
        std::string Why;
        Zones.clear();
        if (O.Install.empty() || !Game.ResolveZones(O.Install, { Input }, O, Zones, Why) || Zones.empty()) return Fail(Error);
    }
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
        std::string Renamed = "index_renamed=" + std::to_string(Ix->Renamed.size()) + "\n";
        for (const auto& R : Ix->Renamed) Renamed += "index_renamed " + R + "\n";
        CliReport(Options, std::string("result=ok\ngame=") + Game->Id() + "\nzones=" + std::to_string(Ix->Zones().size()) +
                               "\nopened=" + std::to_string(Opened) + "\nassets=" + std::to_string(Ix->Entries) +
                               "\nbuilt=" + (Ix->Built ? "1" : "0") + "\nseconds=" + std::to_string(Ix->Seconds) + "\nfile=" + Ix->File + "\n" + Renamed);
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
        S.Weights = Sizes::WeighPicks(*Ix, Picked, S.Kinds, S.Formats);
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

    // disk-weigh <zone> --game <id> --out <dir> [--type kinds]: each asset's weight as the install
    // index keeps it (IDiskZone::Weighs) and what Sizes::Predict makes of it in the formats now,
    // against what an export of the zone writes for it, file by file: per kind the assets matched,
    // those predicted exactly, the bytes predicted and written, and the furthest apart
    int RunDiskWeigh(const CliOptions& Options, const std::string& Input)
    {
        const auto Fail = [&](const std::string& Why) {
            CliReport(Options, "result=error\nreason=" + Why + "\n");
            return 1;
        };
        const IDiskGame* Game = Options.DiskGameId.empty() ? Detect(Input) : Find(Options.DiskGameId);
        if (Game == nullptr) return Fail("no disk reader for " + Input);
        if (Options.Out.empty()) return Fail("disk-weigh needs --out");
        ApplyExportOverrides(Options);
        if (Options.ImageFormat.empty() && Game->DefaultImageFormat()) SettingsManager::SetRunSetting("exportimg", Game->DefaultImageFormat());
        ExportOptions O;
        Context C;
        std::string Error;
        if (!ReadOptions(*Game, Options, O, Error)) return Fail(Error);
        if (!ParseKinds(Options.Type.empty() ? std::string("image,sound") : Options.Type, C.Kinds, Error)) return Fail(Error);
        std::vector<std::string> Zones;
        if (!Game->Zones(Input, O, Zones, Error))
        {
            std::string Why;
            Zones.clear();
            if (O.Install.empty() || !Game->ResolveZones(O.Install, { Input }, O, Zones, Why) || Zones.empty()) return Fail(Error);
        }
        // the weights, as the index build reads them (an archive of several zones: each)
        std::vector<std::pair<AssetId, Weight>> Weighed;
        for (const auto& Path : Zones)
        {
            ExportOptions Indexing = O;
            Indexing.Indexing = true;
            std::string Why;
            const auto Zone = Game->Open(Path, Indexing, Why);
            if (!Zone) return Fail("the zone did not open: " + Why);
            WeighAll(*Zone, Weighed);
        }
        // what an export writes, by kind and file (its path under the kind's folder, no extension)
        C.OutRoot = (fs::path(Options.Out) / Game->Id()).string();
        C.PullDependencies = false;   // the zone's own assets, as the index weighs them
        std::error_code Ec;
        fs::remove_all(C.OutRoot, Ec);
        const Totals T = Export(*Game, O, Zones, C);
        const std::string Formats = Sizes::FormatsNow();
        std::string Lines;
        for (const Kind K : C.Kinds)
        {
            const fs::path Dir = fs::path(C.OutRoot) / KindFolder(K);
            std::map<std::string, uint64_t> Files;
            for (auto It = fs::recursive_directory_iterator(Dir, Ec); !Ec && It != fs::recursive_directory_iterator(); It.increment(Ec))
                if (It->is_regular_file(Ec))
                {
                    std::string Rel = It->path().lexically_relative(Dir).replace_extension().generic_string();
                    Files[ExportNaming::Lower(Rel)] += It->file_size(Ec);
                }
            size_t Assets = 0, Matched = 0, Exact = 0, Unknown = 0;
            uint64_t Predicted = 0, Written = 0;
            std::vector<std::tuple<double, std::string, uint64_t, uint64_t>> Apart;   // |log ratio|, name, predicted, written
            std::set<std::string> Used;
            for (const auto& [A, W] : Weighed)
            {
                if (A.K != K) continue;
                Assets++;
                // a sound's name is its path under the kind's folder (its extension, if it has one, dropped)
                std::string Key = ExportNaming::Lower(K == Kind::Sound ? A.Name : ExportNaming::SafeFileName(A.Name));
                std::replace(Key.begin(), Key.end(), '\\', '/');
                if (K == Kind::Sound)
                    for (const char* E : { ".wav", ".flac", ".mp3", ".ogg" })
                        if (Key.size() > std::strlen(E) && Key.compare(Key.size() - std::strlen(E), std::string::npos, E) == 0) Key.resize(Key.size() - std::strlen(E));
                const auto F = Files.find(Key);
                if (F == Files.end())
                {
                    if (Assets - Matched <= 3)
                        Lines += std::string("unmatched_") + KindName(K) + "=" + A.Name + (Files.empty() ? "" : "\t(a file: " + Files.begin()->first + ")") + "\n";
                    continue;
                }
                Matched++;
                Used.insert(Key);
                bool IsExact = false;
                const uint64_t P = Sizes::Predict(K, W, Formats, IsExact);
                if (P == 0)
                {
                    Unknown++;
                    continue;
                }
                Predicted += P;
                Written += F->second;
                if (P == F->second) Exact++;
                Apart.emplace_back(std::abs(std::log((double)P / std::max<uint64_t>(1, F->second))),
                                   A.Name + " (" + std::to_string(W.A) + "x" + std::to_string(W.B) + ", " + std::to_string(W.Bytes) + " B)", P, F->second);
            }
            std::sort(Apart.rbegin(), Apart.rend());
            uint64_t Unweighed = 0;
            for (const auto& [Key, N] : Files)
                if (!Used.count(Key)) Unweighed += N;
            Lines += std::string("kind_") + KindName(K) + "=weighed " + std::to_string(Assets) + ", written " + std::to_string(Files.size()) +
                     " files, matched " + std::to_string(Matched) + ", exact " + std::to_string(Exact) + ", weight unknown " + std::to_string(Unknown) +
                     ", predicted " + Sizes::Text(Predicted) + " for " + Sizes::Text(Written) + " written (" +
                     std::to_string(Written ? (double)Predicted / (double)Written : 0.0) + "), files not weighed " + Sizes::Text(Unweighed) + "\n";
            for (size_t I = 0; I < Apart.size() && I < 8; I++)
                Lines += std::string("apart_") + KindName(K) + "=" + std::get<1>(Apart[I]) + "\tpredicted " + std::to_string(std::get<2>(Apart[I])) +
                         "\twritten " + std::to_string(std::get<3>(Apart[I])) + "\n";
        }
        if (!Options.KeepOutput) fs::remove_all(C.OutRoot, Ec);
        CliReport(Options, std::string("result=ok\ngame=") + Game->Id() + "\nformats=" + Formats + "\n" + Lines + Report(T));
        return 0;
    }

    void AddDiskWeighOptions(CLI::App& Sub, CliOptions& Options)
    {
        Sub.add_option("--game", Options.DiskGameId, "The game; detected from the zone when omitted");
        Sub.add_flag("--keep", Options.KeepOutput, "Keep what the export wrote");
    }

    const GameCommand DiskWeighCommand{
        "disk-weigh",
        "Each asset's weight (what the install index keeps for the size estimate) against what an export of the zone writes for it",
        "The zone",
        RunDiskWeigh,
        AddDiskWeighOptions,
    };
    const GameCommandRegistration DiskWeighRegistered(DiskWeighCommand);
}
}

// disk-export <input> [--game id]: every game on the disk template, plus their aliases
void AddDiskExportCommands(CliCommandBuilders& B)
{
    B["disk-export"] = [](CLI::App& App, CliOptions& Options) {
        std::string Ids, KindNames;
        for (const DiskGame::IDiskGame* G : DiskGame::Games()) Ids += std::string(Ids.empty() ? "" : ", ") + G->Id();
        for (const DiskGame::KindInfo& K : DiskGame::Kinds) KindNames += std::string(KindNames.empty() ? "" : ", ") + K.Folder;
        auto* Sub = App.add_subcommand("disk-export",
            "Read zones from disk and export " + KindNames + " to <out>/<game>/<kind>/ (--type; games: " + Ids + ")");
        Sub->add_option("zones", Options.DiskExportIn["disk-export"], "A zone, or an install / folder of zones")->required();
        Sub->add_option("--game", Options.DiskGameId, "The game (" + Ids + "); detected from the zones when omitted");
        Sub->add_option("--assets", Options.Assets, "Only these assets, kind:name comma-separated (model:wpn_x,image:i_y)");
        for (const DiskGame::IDiskGame* G : DiskGame::Games())
            for (const auto& Alias : G->Aliases())
            {
                auto* A = App.add_subcommand(Alias, std::string(G->Title()) + " from disk (same as disk-export --game " + G->Id() + ")");
                A->add_option("zones", Options.DiskExportIn[Alias], "A zone, or an install / folder of zones")->required();
            }
    };
}

// zone-resume-check <zone> --game <id> [--limit N]: each sampled zone-level asset walked alone from
// its resume point (ZoneTables::PartialWalk, the assets it reads walked on demand) against the full
// walk of the zone: the same reads (position, size, stream memory), instances, links, back
// references and keys, or what differs
namespace DiskGame
{
namespace
{
    using Read3 = std::tuple<int64_t, int64_t, int64_t>;   // position, size, stream memory

    // the entries of a list of (position-like key, value) whose key is in [Lo, Hi), sorted
    template <class A, class B>
    std::vector<std::pair<A, B>> InRange(const std::vector<std::pair<A, B>>& Sorted, int64_t Lo, int64_t Hi)
    {
        auto F = std::lower_bound(Sorted.begin(), Sorted.end(), std::make_pair((A)Lo, std::numeric_limits<B>::min()));
        std::vector<std::pair<A, B>> Out;
        for (; F != Sorted.end() && (int64_t)F->first < Hi; ++F) Out.push_back(*F);
        return Out;
    }

    template <class T>
    std::string FirstDiff(const std::vector<T>& Full, const std::vector<T>& Part, const std::function<std::string(const T&)>& Text)
    {
        for (size_t K = 0; K < std::max(Full.size(), Part.size()); K++)
        {
            const bool InF = K < Full.size(), InP = K < Part.size();
            if (InF && InP && Full[K] == Part[K]) continue;
            return "#" + std::to_string(K) + " full " + (InF ? Text(Full[K]) : std::string("-")) + " partial " + (InP ? Text(Part[K]) : std::string("-")) +
                   " (" + std::to_string(Full.size()) + " vs " + std::to_string(Part.size()) + ")";
        }
        return {};
    }

    std::string Hex64(int64_t V)
    {
        char B[32];
        snprintf(B, sizeof(B), "0x%llx", (unsigned long long)V);
        return B;
    }

    int RunResumeCheck(const CliOptions& Options, const std::string& Input)
    {
        const auto Fail = [&](const std::string& Why) {
            CliReport(Options, "result=error\nreason=" + Why + "\n");
            return 1;
        };
        const IDiskGame* Game = Options.DiskGameId.empty() ? Detect(Input) : Find(Options.DiskGameId);
        if (Game == nullptr) return Fail("no disk reader for " + Input);
        ExportOptions O;
        std::string Error;
        if (!ReadOptions(*Game, Options, O, Error)) return Fail(Error);
        std::vector<std::string> Zones;
        if (!Game->Zones(Input, O, Zones, Error))
        {
            std::string Why;
            Zones.clear();
            if (O.Install.empty() || !Game->ResolveZones(O.Install, { Input }, O, Zones, Why) || Zones.empty()) return Fail(Error);
        }
        if (Zones.empty()) return Fail("no zone in " + Input);
        const auto T0 = std::chrono::steady_clock::now();
        auto Opened = Game->Open(Zones.front(), O, Error);
        if (!Opened) return Fail("did not open: " + Error);
        auto* Z = dynamic_cast<ZonekitZone*>(Opened.get());
        if (Z == nullptr) return Fail("not a zonekit zone");
        const double OpenSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - T0).count();
        const ZoneTables::ZoneResult& R = Z->FullWalk();
        const ByteView Buf = Z->WalkedBytes();
        const auto* Profile = Z->WalkProfile();
        const ZoneTables::StreamRules Rules = Z->WalkRules();
        if (R.Resume.empty()) return Fail("the full walk recorded no resume point (no layout)");

        // what a resume index gives a partial walk: the zone's assets by hash, the rewind keys
        std::map<std::pair<std::string, uint64_t>, int64_t> ByHash;
        for (const auto& I : R.Instances)
            if (I.Pos >= 0 && I.Pos + 8 <= (int64_t)Buf.size())
            {
                const uint64_t H = ExportNaming::At<uint64_t>(Buf, I.Pos) & 0x7FFFFFFFFFFFFFFFull;
                ByHash.try_emplace({ R.OwnerNames[I.Owner], H }, I.Pos);
                ByHash.try_emplace({ std::string(), H }, I.Pos);
            }

        // the full walk's reads, links, back references and keys by position
        std::vector<int64_t> ReadOrder(R.ReadPos.size());
        for (size_t K = 0; K < ReadOrder.size(); K++) ReadOrder[K] = (int64_t)K;
        std::sort(ReadOrder.begin(), ReadOrder.end(), [&](int64_t A, int64_t B) { return R.ReadPos[(size_t)A] < R.ReadPos[(size_t)B]; });
        auto Links = R.Links;
        std::sort(Links.begin(), Links.end());
        std::vector<std::pair<int64_t, uint64_t>> Refs = R.BackRefs;
        std::sort(Refs.begin(), Refs.end());
        std::vector<std::pair<int64_t, uint64_t>> Keys;   // by where the data is
        for (const auto& [Key, Pos] : R.Keys) Keys.emplace_back(Pos, Key);
        std::sort(Keys.begin(), Keys.end());

        const size_t Points = R.Resume.size();
        const size_t Want = Options.Limit ? std::min<size_t>(Options.Limit, Points) : std::min<size_t>(500, Points);
        const size_t Step = std::max<size_t>(1, Points / std::max<size_t>(Want, 1));
        size_t Checked = 0, Ok = 0, Failed = 0, Fetched = 0;
        std::map<std::string, size_t> Bad;
        std::string Examples;
        size_t ExampleCount = 0;
        const auto T1 = std::chrono::steady_clock::now();
        ZoneTables::WalkOnLargeStack([&] {
            ZoneTables::PartialWalk PW(Buf, Profile->Tables, Profile->Profile.TypeNames, Rules, R.Resume);
            if (!PW.Ok())
            {
                Error = "partial walk: " + PW.Why();
                return;
            }
            std::vector<std::pair<uint64_t, int64_t>> Given(R.Keys.begin(), R.Keys.end());
            PW.AddKeys(Given);
            PW.AddAssetHashes(ByHash);
            for (size_t K = 0; K < Points; K += Step)
            {
                Checked++;
                PW.Forget();
                const ZoneTables::ZoneResult* P = PW.Walk(K);
                const auto Note = [&](const std::string& Category, const std::string& What) {
                    Bad[Category]++;
                    if (ExampleCount++ < 30)
                        Examples += "point " + std::to_string(K) + " (" + Hex64(R.Resume[K].Pos) + ") " + Category + ": " + What + "\n";
                };
                if (P == nullptr)
                {
                    Failed++;
                    Note("walk_failed", PW.Why());
                    continue;
                }
                Fetched += PW.Walked().size() - 1;
                const int64_t Lo = R.Resume[K].Pos, Hi = K + 1 < Points ? R.Resume[K + 1].Pos : (int64_t)Buf.size();
                bool Same = true;
                // reads
                std::vector<Read3> FullReads, PartReads;
                for (auto It = std::lower_bound(ReadOrder.begin(), ReadOrder.end(), Lo, [&](int64_t I, int64_t V) { return R.ReadPos[(size_t)I] < V; });
                     It != ReadOrder.end() && R.ReadPos[(size_t)*It] < Hi; ++It)
                    FullReads.emplace_back(R.ReadPos[(size_t)*It], R.Reads[(size_t)*It], (size_t)*It < R.ReadMem.size() ? R.ReadMem[(size_t)*It] : -2);
                for (size_t I = 0; I < P->ReadPos.size(); I++)
                    PartReads.emplace_back(P->ReadPos[I], P->Reads[I], I < P->ReadMem.size() ? P->ReadMem[I] : -2);
                std::sort(FullReads.begin(), FullReads.end());
                std::sort(PartReads.begin(), PartReads.end());
                if (FullReads != PartReads)
                {
                    Same = false;
                    Note("reads", FirstDiff<Read3>(FullReads, PartReads, [](const Read3& X) {
                        return Hex64(std::get<0>(X)) + "/" + std::to_string(std::get<1>(X)) + "/" + Hex64(std::get<2>(X));
                    }));
                }
                // links, back references, keys
                auto PL = P->Links;
                std::sort(PL.begin(), PL.end());
                const auto FL = InRange(Links, Lo, Hi);
                if (FL != PL)
                {
                    Same = false;
                    Note("links", FirstDiff<std::pair<int64_t, int64_t>>(FL, PL, [](const auto& X) { return Hex64(X.first) + "->" + Hex64(X.second); }));
                }
                auto PR = P->BackRefs;
                std::sort(PR.begin(), PR.end());
                const auto FR = InRange(Refs, Lo, Hi);
                if (FR != PR)
                {
                    Same = false;
                    Note("backrefs", FirstDiff<std::pair<int64_t, uint64_t>>(FR, PR, [](const auto& X) { return Hex64(X.first) + "=" + Hex64((int64_t)X.second); }));
                }
                std::vector<std::pair<int64_t, uint64_t>> PK;
                for (const auto& [Key, Pos] : P->Keys) PK.emplace_back(Pos, Key);
                std::sort(PK.begin(), PK.end());
                const auto FK = InRange(Keys, Lo, Hi);
                if (FK != PK)
                {
                    Same = false;
                    Note("keys", FirstDiff<std::pair<int64_t, uint64_t>>(FK, PK, [](const auto& X) { return Hex64(X.first) + ":" + Hex64((int64_t)X.second); }));
                }
                // instances: their positions and types
                std::vector<std::pair<int64_t, std::string>> FI, PI;
                for (const auto& I : R.Instances)
                    if (I.Pos >= Lo && I.Pos < Hi) FI.emplace_back(I.Pos, R.OwnerNames[I.Owner]);
                for (const auto& I : P->Instances) PI.emplace_back(I.Pos, P->OwnerNames[I.Owner]);
                std::sort(FI.begin(), FI.end());
                std::sort(PI.begin(), PI.end());
                if (FI != PI)
                {
                    Same = false;
                    Note("instances", FirstDiff<std::pair<int64_t, std::string>>(FI, PI, [](const auto& X) { return Hex64(X.first) + " " + X.second; }));
                }
                Ok += Same;
            }
        });
        if (!Error.empty()) return Fail(Error);
        const double CheckSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - T1).count();

        // links: a tenth of the points, each with the closure its back references reach walked on
        // demand (MissingOwners), resolved with the resume points' hints against the full walk's
        const auto T2 = std::chrono::steady_clock::now();
        ResolveHints Hints;
        Hints.Points = &R.Resume;
        const std::vector<std::pair<uint64_t, int64_t>> HintKeys(R.Keys.begin(), R.Keys.end());
        Hints.Keys = &HintKeys;
        Hints.ByHash = &ByHash;
        const ResolvedLinks Full = ResolveLinks(R, Rules, Z->ResolvesDerefs(), Buf);
        auto FullLinks = Full.Links;
        std::sort(FullLinks.begin(), FullLinks.end());
        size_t LinkChecked = 0, LinkSame = 0, ClosurePoints = 0, ClosureMax = 0, Capped = 0;
        int64_t ClosureBytes = 0;
        ZoneTables::WalkOnLargeStack([&] {
            ZoneTables::PartialWalk PW(Buf, Profile->Tables, Profile->Profile.TypeNames, Rules, R.Resume);
            if (!PW.Ok()) return;
            std::vector<std::pair<uint64_t, int64_t>> Given(R.Keys.begin(), R.Keys.end());
            PW.AddKeys(Given);
            PW.AddAssetHashes(ByHash);
            for (size_t K = 0; K < Points; K += Step * 10)
            {
                PW.Forget();
                if (PW.Walk(K) == nullptr) continue;
                LinkChecked++;
                bool Over = false;
                for (;;)
                {
                    const std::set<size_t> More = MissingOwners(PW.Merged(), Rules, PW, Hints);
                    if (More.empty()) break;
                    for (const size_t P : More) PW.Walk(P);
                    if (PW.Walked().size() > 5000) { Over = true; break; }
                }
                Capped += Over;
                const ZoneTables::ZoneResult M = PW.Merged();
                const ResolvedLinks Part = ResolveLinks(M, Rules, Z->ResolvesDerefs(), Buf, &Hints);
                auto PL = Part.Links;
                std::sort(PL.begin(), PL.end());
                PL.erase(std::unique(PL.begin(), PL.end()), PL.end());
                std::vector<std::pair<int64_t, int64_t>> FL;
                for (const auto& [P, W] : PW.Walked())
                {
                    const int64_t Lo = R.Resume[P].Pos, Hi = P + 1 < Points ? R.Resume[P + 1].Pos : (int64_t)Buf.size();
                    const auto In = InRange(FullLinks, Lo, Hi);
                    FL.insert(FL.end(), In.begin(), In.end());
                    ClosureBytes += Hi - Lo;
                }
                std::sort(FL.begin(), FL.end());
                FL.erase(std::unique(FL.begin(), FL.end()), FL.end());
                ClosurePoints += PW.Walked().size();
                ClosureMax = std::max(ClosureMax, PW.Walked().size());
                if (FL == PL) LinkSame++;
                else
                {
                    Bad["resolved_links"]++;
                    if (ExampleCount++ < 30)
                        Examples += "point " + std::to_string(K) + " resolved_links (closure " + std::to_string(PW.Walked().size()) + "): " +
                                    FirstDiff<std::pair<int64_t, int64_t>>(FL, PL, [](const auto& X) { return Hex64(X.first) + "->" + Hex64(X.second); }) + "\n";
                }
            }
        });
        const double LinkSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - T2).count();
        const std::string LinkLines = "links_checked=" + std::to_string(LinkChecked) + "\nlinks_same=" + std::to_string(LinkSame) +
                                      "\nclosure_points_avg=" + std::to_string(LinkChecked ? ClosurePoints / LinkChecked : 0) +
                                      "\nclosure_points_max=" + std::to_string(ClosureMax) + "\nclosure_bytes_avg=" +
                                      std::to_string(LinkChecked ? ClosureBytes / (int64_t)LinkChecked : 0) + "\nclosure_capped=" + std::to_string(Capped) +
                                      "\nlink_seconds=" + std::to_string(LinkSeconds) + "\n";
        const bool AllSame = Ok == Checked && LinkSame == LinkChecked;
        std::string Lines;
        for (const auto& [Category, N] : Bad) Lines += "mismatch_" + Category + "=" + std::to_string(N) + "\n";
        CliReport(Options, std::string(AllSame ? "result=ok" : "result=mismatch") + "\ngame=" + Game->Id() + "\nzone=" + Zones.front() +
                               "\nresume_points=" + std::to_string(Points) + "\nchecked=" + std::to_string(Checked) + "\nsame=" + std::to_string(Ok) +
                               "\nwalk_failed=" + std::to_string(Failed) + "\nwalked_on_demand=" + std::to_string(Fetched) +
                               "\nopen_seconds=" + std::to_string(OpenSeconds) + "\ncheck_seconds=" + std::to_string(CheckSeconds) + "\n" + LinkLines + Lines +
                               (Examples.empty() ? "" : "\n" + Examples));
        return AllSame ? 0 : 2;
    }

    void AddResumeCheckOptions(CLI::App& Sub, CliOptions& Options)
    {
        Sub.add_option("--game", Options.DiskGameId, "The game; detected from the zone when omitted");
    }

    const GameCommand ResumeCheckCommand{
        "zone-resume-check",
        "Each sampled asset of a zone walked alone from its resume point against the full walk (--limit: how many, 500 by default)",
        "A zone",
        RunResumeCheck,
        AddResumeCheckOptions,
    };
    const GameCommandRegistration ResumeCheckRegistered(ResumeCheckCommand);
}
}
