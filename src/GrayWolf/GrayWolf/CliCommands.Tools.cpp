// NOMINMAX before <windows.h>: std::max/std::min below would be mangled by its macros.
#define NOMINMAX

#include <filesystem>
#include <algorithm>
#include <string>
#include <format>

#include "CliOptions.h"
#include "CliCommands.h"

#include <CLI/CLI.hpp>
#include "CliSupport.h"

#include "WraithX.h"
#include "CoDAssets.h"
#include "FileSystems.h"

#include "OatBridge.h"
#include "GameInstalls.h"
#include "NameDbUpdater.h"
#include "Updater.h"
#include "OatInProcess.h"
#include "ZoneSource.h"
#include "CliCommandOptions.h"

// Reports, for one fastfile on disk, every asset type it holds and which mechanism can extract it:
// OAT's own writers, one of ours, or nothing.
CoverageCommandOptions CoverageCommandOptions::From(const CliOptions& Options)
{
    return { Options.Coverage, Options.Report };
}

int RunCoverage(const CliOptions& Options)
{
    const auto Command = CoverageCommandOptions::From(Options);

    // Everything goes through the shared reader first, so a container nobody models yet reports
    // what it is rather than an error - that distinction is the whole point of tracking coverage.
    std::string OpenError;
    const auto Source = OpenZoneSource(Command.FastFilePath, OpenError);
    if (!Source)
    {
        CliReport(Options, std::format("result=error\nreason={}\n", OpenError.c_str()));
        return 6;
    }

    if (Source->Kind() == ZoneSourceKind::DiskContainerOnly)
    {
        CliReport(Options, std::format(
            "zone={}\ngame={}\nstage=container\nzone_bytes={}\n"
            "covered=0/0\nnote=container decompresses, its asset list is not modelled yet\nresult=ok\n",
            Source->ZoneName().c_str(), Source->Game().c_str(), (unsigned long long)Source->ZoneBytes()));
        return 0;
    }

    std::string Report;
    if (OatZoneCoverage(Command.FastFilePath, Report))
    {
        CliReport(Options, Report + "result=ok\n");
        return 0;
    }

    // A reader of our own knows the list for titles OAT never loads, so report from the source
    // instead of passing OAT's failure off as the zone's own.
    CliReport(Options, ZoneSourceCoverageReport(*Source) + "result=ok\n");
    return 0;
}

int RunOatExtract(const CliOptions& Options)
{
    WraithX::InitializeAPI();
    if (Options.OatList)
    {
        std::string Report;
        const bool Ok = OatListZoneInProcess(Options.OatExtract, Report);
        CliReport(Options, std::format("result={}\n{}", Ok ? "ok" : "error", Report.c_str()));
        return Ok ? 0 : 5;
    }
    OatDumpOptions Opts;
    if (!Options.OatModelFmt.empty()) Opts.ModelFormat = Options.OatModelFmt;
    if (!Options.OatImageFmt.empty()) Opts.ImageFormat = Options.OatImageFmt;
    Opts.Gdt = Options.OatGdt;
    Opts.WithSounds = Options.WithSounds;
    const std::string OutDir = Options.Out.empty() ? "oat_extracted" : Options.Out;
    std::string Report;
    const bool Ok = OatDumpZoneInProcess(Options.OatExtract, OutDir, Opts, Report);
    CliReport(Options, std::format("result={}\n{}", Ok ? "ok" : "error", Report.c_str()));
    return Ok ? 0 : 5;
}


// Per-element FX/material/blend detail read straight out of a T5 (BO1) fastfile, in-process, no
// game required - reuses OAT's already-loaded, already-relocated FxEffectDef/Material pool.
int RunOatFx(const CliOptions& Options)
{
    WraithX::InitializeAPI();
    std::string Report;
    const bool Ok = OatDescribeFxInProcess(Options.OatFx, Report);
    CliReport(Options, std::format("result={}\n{}", Ok ? "ok" : "error", Report.c_str()));
    return Ok ? 0 : 5;
}


int RunOatFxExport(const CliOptions& Options)
{
    WraithX::InitializeAPI();
    const std::string OutDir = Options.Out.empty() ? "fx_exported" : Options.Out;
    std::string Report;
    const bool Ok = OatExportFxInProcess(Options.OatFxExport, Options.NameFilter, OutDir, Report);
    CliReport(Options, std::format("result={}\n{}", Ok ? "ok" : "error", Report.c_str()));
    return Ok ? 0 : 5;
}


int RunOatXModelMtl(const CliOptions& Options)
{
    WraithX::InitializeAPI();
    std::string Report;
    const bool Ok = OatListXModelMaterialsInProcess(Options.OatXModelMtl, Report);
    CliReport(Options, std::format("result={}\n{}", Ok ? "ok" : "error", Report.c_str()));
    return Ok ? 0 : 5;
}


int RunOatWeapon(const CliOptions& Options)
{
    WraithX::InitializeAPI();
    std::string Report;
    const bool Ok = OatDescribeWeaponInProcess(Options.OatWeapon, Options.NameFilter, Report);
    CliReport(Options, std::format("result={}\n{}", Ok ? "ok" : "error", Report.c_str()));
    return Ok ? 0 : 5;
}


int RunOatRawfiles(const CliOptions& Options)
{
    WraithX::InitializeAPI();
    const std::string OutDir = Options.Out.empty() ? "rawfiles_extracted" : Options.Out;
    std::string Report;
    const bool Ok = OatDumpRawfilesInProcess(Options.OatRawfiles, Options.NameFilter, OutDir, Report);
    CliReport(Options, std::format("result={}\n{}", Ok ? "ok" : "error", Report.c_str()));
    return Ok ? 0 : 5;
}


// Extracts a modern-CoD fastfile (BO4/CW/Vanguard/MW19/MWII/MWIII/BO6, incl. GSC decompilation)
// by driving ate47's "acts" - reuses its handlers/struct specs across a process boundary.
int RunActsExtract(const CliOptions& Options)
{
    ActsExtractOptions Opts;
    Opts.Handler = Options.ActsHandler;
    const std::string OutDir = Options.Out.empty() ? "acts_extracted" : Options.Out;
    std::string Log;
    const bool Ok = ActsExtractFastFile(Options.ActsExe, Options.ActsExtract, OutDir, Opts, Log);
    CliReport(Options, std::format("result={}\nfastfile={}\nout={}\n--- acts ---\n{}",
        Ok ? "ok" : "error", Options.ActsExtract.c_str(), OutDir.c_str(), Log.c_str()));
    return Ok ? 0 : 5;
}


int RunUpdateNameDb(const CliOptions& Options)
{
    // Where every LoadIndex("package_index\\<name>.wni") call already looks - see
    // NameDbUpdater.cpp's own mapping table for exactly which files this can fill in and why not all.
    const auto OutDir = !Options.NameDbDir.empty() ? Options.NameDbDir
        : (std::filesystem::path(FileSystems::GetApplicationPath()) / "package_index").string();

    std::string Report = std::format("output_dir={}\n", OutDir.c_str());

    const auto Results = NameDbUpdater::Update(OutDir);

    size_t OkCount = 0;
    for (const auto& R : Results)
    {
        if (R.Downloaded && R.Converted)
        {
            OkCount++;
            Report += std::format("file={:<24} result=ok entries={}\n", R.WniName.c_str(), R.Entries);
        }
        else
        {
            Report += std::format("file={:<24} result=error reason={}\n", R.WniName.c_str(), R.Error.c_str());
        }
    }

    Report += std::format("result={}\nfiles={} ok={}\n",
        (OkCount == Results.size()) ? "ok" : (OkCount > 0 ? "partial" : "error"),
        Results.size(), OkCount);

    CliReport(Options, Report);
    return (OkCount == Results.size()) ? 0 : (OkCount > 0 ? 1 : 2);
}


// update: the latest GitHub release's manifest; newer hash packages installed, a newer app
// reported (or, with --app, downloaded and applied once this run exits). Exit codes: 0 done or
// nothing newer, 3 unavailable (no release, private repository without a token), 4 network or
// HTTP error, 5 verification failed (nothing installed), 6 refused (package_index not managed by
// updates), 7 local file error (previous files kept).
int RunUpdate(const CliOptions& Options)
{
    Updater::Config C = Updater::FromSettings();
    if (!Options.UpdateRepo.empty()) C.Repo = Options.UpdateRepo;
    if (!Options.UpdatePackageDir.empty()) C.PackageDir = Options.UpdatePackageDir;
    C.Force = Options.UpdateForce;

    auto Code = [](Updater::Status S) {
        switch (S)
        {
        case Updater::Status::UpToDate: case Updater::Status::Available: case Updater::Status::Updated: return 0;
        case Updater::Status::Unavailable: return 3;
        case Updater::Status::NetworkError: return 4;
        case Updater::Status::VerifyFailed: return 5;
        case Updater::Status::Refused: return 6;
        default: return 7;
        }
    };

    const Updater::CheckResult R = Updater::Check(C);
    std::string Report = std::format("repo={}\ncheck={}\nmessage={}\n", C.Repo, Updater::StatusName(R.Result), R.Message);
    if (R.Result != Updater::Status::UpToDate && R.Result != Updater::Status::Available)
    {
        CliReport(Options, Report + "result=error\n");
        return Code(R.Result);
    }
    Report += std::format("latest={}\napp_local={}\napp_latest={}\napp_newer={}\npackages_dir={}\npackages_local={}\npackages_latest={}\npackages_newer={}\n",
        R.Latest.Tag, R.LocalApp, R.Latest.App ? R.Latest.App->Version : "-", R.AppNewer ? "yes" : "no",
        C.PackageDir, R.LocalPackages.empty() ? "none" : R.LocalPackages,
        R.Latest.Packages ? R.Latest.Packages->Version : "-", R.PackagesNewer ? "yes" : "no");

    int Exit = 0;
    if (!Options.UpdateCheck && R.PackagesNewer && R.Latest.Packages)
    {
        std::string Msg;
        const Updater::Status S = Updater::UpdatePackages(C, *R.Latest.Packages, Msg);
        Report += std::format("packages={}\npackages_message={}\n", Updater::StatusName(S), Msg);
        Exit = Code(S);
    }
    if (!Options.UpdateCheck && Options.UpdateApp && R.AppNewer && R.Latest.App && Exit == 0)
    {
        std::string Dir, Msg;
        const Updater::Status S = Updater::StageApp(C, *R.Latest.App, Dir, Msg);
        Report += std::format("app={}\napp_message={}\n", Updater::StatusName(S), Msg);
        Exit = Code(S);
        if (S == Updater::Status::Updated)
        {
            const bool Started = Updater::LaunchApply(C, Dir, false, Msg);
            Report += std::format("apply={}\napply_message={}\n", Started ? "started" : "error", Msg);
            if (!Started) Exit = 7;
        }
    }
    Report += std::format("result={}\n", Exit == 0 ? "ok" : "error");
    CliReport(Options, Report);
    return Exit;
}

// apply-update (internal): run from a copy of the exe by `update --app` or the GUI.
int RunApplyUpdate(const CliOptions& Options)
{
    std::string Report;
    const int Exit = Updater::ApplyStaged(Options.ApplyUpdate, Options.ApplyTarget, Options.ApplyWaitPid, Options.ApplyRelaunch, Report);
    // the helper has no console: its report goes beside the app, for the session that started it
    std::ofstream((std::filesystem::path(Options.ApplyTarget) / ".update" / "apply-report.txt").string(), std::ios::trunc) << Report;
    CliReport(Options, Report);
    return Exit;
}

int RunImportHashCsv(const CliOptions& Options)
{
    if (Options.ImportHashName.empty())
    {
        CliReport(Options, "result=error\nreason=--import-hash-name required (the output .wni stem)\n");
        return 2;
    }

    const auto OutDir = !Options.NameDbDir.empty() ? Options.NameDbDir
        : (std::filesystem::path(FileSystems::GetApplicationPath()) / "package_index").string();

    const auto R = NameDbUpdater::ConvertLocalCsv(Options.ImportHashCsv, Options.ImportHashName, OutDir);

    std::string Report = std::format("csv={} output_dir={}\n", Options.ImportHashCsv.c_str(), OutDir.c_str());
    if (R.Converted)
        Report += std::format("file={} result=ok entries={}\n", R.WniName.c_str(), R.Entries);
    else
        Report += std::format("file={} result=error reason={}\n", R.WniName.c_str(), R.Error.c_str());
    Report += std::format("result={}\n", R.Converted ? "ok" : "error");

    CliReport(Options, Report);
    return R.Converted ? 0 : 2;
}

// list-installs: what GameInstalls::Detect finds, one line per install, in the panel's order
int RunListInstalls(const CliOptions& Options)
{
    const auto Start = CliClock::now();
    const auto Installs = GameInstalls::Detect();
    std::string Report = std::format("result=ok\ninstalls={}\ndetect_ms={}\n", Installs.size(),
        (long long)CliElapsedMs(Start, CliClock::now()));
    for (const auto& I : Installs)
        Report += std::format("install={} year={} source={} disk={} path={}\n", I.Label.c_str(), I.Year,
            I.Source.c_str(), I.DiskId.c_str(), I.Path.c_str());
    CliReport(Options, Report);
    return 0;
}

// ---- the subcommands (CliCommands.h)

void AddToolsCommands(CliCommandBuilders& B)
{
    B["list-installs"] = [](CLI::App& App, CliOptions& Options) {
        App.add_subcommand("list-installs", "The Call of Duty installs found on this machine, as the Load Game panel lists them")
            ->callback([&Options] { Options.ListInstalls = true; });
    };
    B["coverage"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("coverage", "Per-type disk-extraction coverage for one fastfile (what comes out, and through which mechanism)");
        Sub->add_option("file", Options.Coverage, "Fast file to report on")->required()->check(CLI::ExistingFile);
    };
    B["oat-extract"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("oat-extract", "Extract a CoD4/WaW/BO1/BO2/MW2/MW3 fastfile via linked OAT libs (in-process)");
        Sub->add_option("file", Options.OatExtract, "Fast file to extract")->required()->check(CLI::ExistingFile);
        Sub->add_flag("--oat-list", Options.OatList, "List the fastfile's assets instead of extracting");
    };
    B["oat-fx"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("oat-fx", "Per-element FX/material/blend detail, T5+T6, no game needed");
        Sub->add_option("file", Options.OatFx, "Fast file to read")->required()->check(CLI::ExistingFile);
    };
    B["oat-fx-export"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("oat-fx-export", "Real per-effect JSON export to --out, T5+T6");
        Sub->add_option("file", Options.OatFxExport, "Fast file to read")->required()->check(CLI::ExistingFile);
    };
    B["oat-xmodel-materials"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("oat-xmodel-materials", "Per-xmodel-surface material/blend, T5+T6");
        Sub->add_option("file", Options.OatXModelMtl, "Fast file to read")->required()->check(CLI::ExistingFile);
    };
    B["oat-weapon"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("oat-weapon", "Compiled WeaponDef splash/damage-falloff fields, T5+T6");
        Sub->add_option("file", Options.OatWeapon, "Fast file to read")->required()->check(CLI::ExistingFile);
    };
    B["oat-rawfiles"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("oat-rawfiles", "Dump .gsc/.csc (T5) / plain rawfiles+.atr (T6) source to --out");
        Sub->add_option("file", Options.OatRawfiles, "Fast file to read")->required()->check(CLI::ExistingFile);
    };
    B["acts-extract"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("acts-extract", "Extract a modern-CoD fastfile via ate47's acts");
        Sub->add_option("file", Options.ActsExtract, "Fast file to extract")->required()->check(CLI::ExistingFile);
    };
    B["update-name-db"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("update-name-db", "Fetch echo000/cod-name-db's CSVs into package_index\\*.wni");
        Sub->add_option("--name-db-dir", Options.NameDbDir, "Override the package_index output directory");
        Sub->callback([&Options] { Options.UpdateNameDb = true; });
    };
    B["update"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("update", "Update the hash packages (package_index) from the latest GitHub release; report or apply a newer GrayWolf");
        Sub->add_flag("--check", Options.UpdateCheck, "Only report what is newer, download nothing");
        Sub->add_flag("--app", Options.UpdateApp, "Also download a newer GrayWolf and replace this one once it exits");
        Sub->add_flag("--force", Options.UpdateForce, "Install the packages even when not newer, hand-built or linked");
        Sub->add_option("--package-dir", Options.UpdatePackageDir, "Override the package_index directory");
        Sub->add_option("--repo", Options.UpdateRepo, "owner/name (default: the update_repo setting)");
        Sub->callback([&Options] { Options.Update = true; });
    };
    // Run by `update --app` / the GUI's Restart now, from a copy of the exe: not for users.
    B["apply-update"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("apply-update", "(internal) Replace GrayWolf's files with a verified update");
        Sub->group("");
        Sub->add_option("stage", Options.ApplyUpdate, "Verified update folder")->required();
        Sub->add_option("--target", Options.ApplyTarget, "GrayWolf's folder")->required();
        Sub->add_option("--wait-pid", Options.ApplyWaitPid, "Wait for this process to exit first");
        Sub->add_flag("--relaunch", Options.ApplyRelaunch, "Start the new GrayWolf.exe afterwards");
    };
    B["import-hash-csv"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("import-hash-csv", "Convert a local \"hash,name\" CSV to .wni");
        Sub->add_option("file", Options.ImportHashCsv, "Local hash,name CSV")->required()->check(CLI::ExistingFile);
        Sub->add_option("--import-hash-name", Options.ImportHashName, "Output .wni stem (no extension)");
        Sub->add_option("--name-db-dir", Options.NameDbDir, "Override the package_index output directory");
    };
}
