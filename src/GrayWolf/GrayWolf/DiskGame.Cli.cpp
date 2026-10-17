#include "stdafx.h"

#include "DiskGameCli.h"

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
    O.ZoneAlone = std::all_of(C.Kinds.begin(), C.Kinds.end(), [](Kind K) { return K == Kind::MaterialLinks || K == Kind::SoundLinks || K == Kind::AssetRefs || K == Kind::Techsets || K == Kind::ScriptBundles || K == Kind::Weapons; }) && !C.Kinds.empty();
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
}
}
