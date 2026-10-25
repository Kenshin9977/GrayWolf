#include "stdafx.h"

#include <stdio.h>
#include <cmath>
#include "AppLog.h"
#include <CLI/CLI.hpp>

// Must be included before additional includes
#include "WraithX.h"
#include "Updater.h"


#include "resource.h"

#include "SettingsManager.h"
#include "CoDAssets.h"
#include "GameCommand.h"
#include "DiskGameCli.h"
#include "ImGuiShell.h"

#include <vector>
#include "FileSystems.h"

#include <string>

#include "CliOptions.h"
#include "CliCommands.h"
#include "CliSupport.h"

#include <dbghelp.h>
#include <filesystem>
#pragma comment(lib, "dbghelp.lib")

// Allow modern GUI controls
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// Cleanup old patch files before launch...
static void CleanupFilesystem()
{
    std::error_code Ec;
    auto CurrentPath = FileSystems::GetApplicationPath();

    std::filesystem::remove((std::filesystem::path(CurrentPath) / "exhalelib.dll").string(), Ec);
    std::filesystem::remove((std::filesystem::path(CurrentPath) / "latest_log.txt").string(), Ec);
    std::filesystem::remove((std::filesystem::path(CurrentPath) / "settings.dat").string(), Ec);
    std::filesystem::remove((std::filesystem::path(CurrentPath) / "Wraith.exe").string(), Ec);

    // Old cache
    std::filesystem::remove_all((std::filesystem::path(CurrentPath) / "data").string(), Ec);
    // Old update cache
    std::filesystem::remove_all((std::filesystem::path(CurrentPath) / "Temp").string(), Ec);
}

// Set once a CLI subcommand runs: no one is there to click a dialog, and a modal box on a crashed
// worker thread froze whole exports instead of ending them.
static volatile bool g_CliMode = false;

LONG WINAPI MyUnhandledExceptionFilter(EXCEPTION_POINTERS* ExceptionInfo)
{
    if (!g_CliMode)
    MessageBoxA(NULL, "GrayWolf has encountered a fatal error and must close.\n\nA dump file will be written to where GrayWolf's exe is located, please provide this and as much information as you can when reporting this crash.", "GrayWolf", MB_OK | MB_ICONERROR);
    
    HANDLE hFile = CreateFile(
        L"crash_dump.dmp",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = GetCurrentThreadId();
    mei.ClientPointers = TRUE;
    mei.ExceptionPointers = ExceptionInfo;
    MiniDumpWriteDump(
        GetCurrentProcess(),
        GetCurrentProcessId(),
        hFile,
        MiniDumpWithFullMemory,
        &mei,
        NULL,
        NULL);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hFile);
    }

    // headless: the dump is written, end with a failing code rather than waiting on anything
    if (g_CliMode) TerminateProcess(GetCurrentProcess(), 0xC0DE);

    return EXCEPTION_EXECUTE_HANDLER;
}


// Loads a package file and optionally exports it, without ever creating a window.
// Everything below runs the exact same asset pipeline the UI drives, the only
// difference being that no progress callback is installed.
static int RunCli(const CliOptions& Options)
{
    if (Options.SelfTest) return RunSelfTest(Options);
    if (Options.ListInstalls) return RunListInstalls(Options);


    if (!Options.Identify.empty()) return RunIdentify(Options);

    if (!Options.Bo3Images.empty()) return RunBo3Images(Options);

    if (!Options.Bo3DumpXpakEntries.empty()) return RunBo3DumpXpakEntries(Options);

    if (Options.Salsa20SelfTest) return RunSalsa20SelfTest(Options);

    if (!Options.Bo3DecryptMovie.empty()) return RunBo3DecryptMovie(Options);

    if (!Options.DecompressFF.empty()) return RunDecompressFF(Options);

    if (!Options.VerifyOpus.empty()) return RunVerifyOpus(Options);

    if (!Options.IndexAssets.empty() || !Options.ModelsDir.empty()) return RunIndexAssets(Options);

    if (!Options.QueryAssets.empty()) return RunQueryAssets(Options);

    if (!Options.AudioProbe.empty()) return RunAudioProbe(Options);

    if (!Options.CascList.empty()) return RunCascList(Options);
    if (!Options.XpakIndex.empty()) return RunXpakIndex(Options);
    if (!Options.XsubIndex.empty()) return RunXsubIndex(Options);
    if (!Options.XsubExtract.empty()) return RunXsubExtract(Options);

    if (!Options.CascExtractDir.empty()) return RunCascExtractDir(Options);

    if (!Options.ZoneDecompressIn.empty()) return RunZoneDecompress(Options);

    if (!Options.Bo4ExportLocalizeDiskIn.empty()) return RunBo4ExportLocalizeDisk(Options);

    if (!Options.Bo4ExportRawStringsDiskIn.empty()) return RunBo4ExportRawStringsDisk(Options);

    if (!Options.ZoneWalkList.empty()) return RunZoneWalk(Options);

    for (const GameCommand* Command : GameCommands())
    {
        auto It = Options.DiskExportIn.find(Command->Name);
        if (It != Options.DiskExportIn.end() && !It->second.empty()) return Command->Run(Options, It->second);
    }
    // disk-export <input> [--game id], or a game's own alias: the one driver (DiskGame.h)
    for (const auto& [Command, Input] : Options.DiskExportIn)
    {
        if (Input.empty()) continue;
        const DiskGame::IDiskGame* Game = nullptr;
        if (Command != "disk-export") Game = DiskGame::Find(Command);
        else if (!Options.DiskGameId.empty()) Game = DiskGame::Find(Options.DiskGameId);
        else Game = DiskGame::Detect(Input);
        if (Game == nullptr)
        {
            CliReport(Options, "result=error\nreason=no disk reader for " + (Options.DiskGameId.empty() ? Input : Options.DiskGameId) + "\n");
            return 1;
        }
        return DiskGame::Run(*Game, Options, Input);
    }

    if (!Options.DecodeIwd.empty()) return RunDecodeIwd(Options);

    if (!Options.GltfSelfTest.empty()) return RunGltfSelfTest(Options);

    if (!Options.ExportMapGlb.empty()) return RunExportMapGlb(Options);

    if (!Options.OpenMap.empty()) return RunOpenMap(Options);

    if (Options.CerberusSelfTest) return RunCerberusSelfTest(Options);

    if (Options.SeAnimSelfTest) return RunSeAnimSelfTest(Options);
    if (Options.CastSelfTest) return RunCastSelfTest(Options);

    if (!Options.MakeTestSeAnim.empty()) return RunMakeTestSeAnim(Options);

    if (!Options.RenderAnimModel.empty()) return RunRenderAnimModel(Options);

    if (!Options.Coverage.empty()) return RunCoverage(Options);
    if (!Options.OatExtract.empty()) return RunOatExtract(Options);

    if (!Options.XAnimBinDump.empty()) return RunXAnimBinDump(Options);

    if (!Options.XAnimBinSelfTest.empty()) return RunXAnimBinSelfTest(Options);

    if (!Options.XAnimBinToSeAnimIn.empty()) return RunXAnimBinToSeAnimIn(Options);

    if (!Options.XAnimBinExportDirIn.empty()) return RunXAnimBinExportDirIn(Options);

    if (!Options.OatFx.empty()) return RunOatFx(Options);

    if (!Options.OatFxExport.empty()) return RunOatFxExport(Options);

    if (!Options.OatXModelMtl.empty()) return RunOatXModelMtl(Options);

    if (!Options.OatWeapon.empty()) return RunOatWeapon(Options);

    if (!Options.OatRawfiles.empty()) return RunOatRawfiles(Options);

    if (!Options.GenDisc.empty()) return RunGenDisc(Options);

    if (!Options.ActsExtract.empty()) return RunActsExtract(Options);

    if (!Options.DumpModel.empty()) return RunDumpModel(Options);

    if (!Options.DumpGlb.empty()) return RunDumpGlb(Options);

    if (!Options.RenderGlbPng.empty()) return RunRenderGlbPng(Options);

    if (!Options.Ui.empty()) return RunUiMode(Options);

    if (!Options.UiGridShot.empty()) return RunUiGridShot(Options);

    if (!Options.AnimRigs.empty()) return RunAnimRigs(Options);

    if (!Options.PreviewWindow.empty()) return RunPreviewWindow(Options);

    if (!Options.RenderModel.empty()) return RunRenderModelCli(Options);

    if (!Options.RenderFx.empty()) return RunRenderFxCli(Options);

    if (!Options.SabProbe.empty()) return RunSabProbe(Options);

    if (Options.UpdateNameDb) return RunUpdateNameDb(Options);

    if (Options.Update) return RunUpdate(Options);

    if (!Options.ApplyUpdate.empty()) return RunApplyUpdate(Options);

    if (!Options.ImportHashCsv.empty()) return RunImportHashCsv(Options);


    if (!Options.Bo4DecryptHex.empty() || !Options.Bo4DecryptDiskHex.empty()) return RunBo4Decrypt(Options);

    if (Options.File.empty())
    {
        CliReport(Options, "result=error\nreason=missing subcommand\n");
        return 2;
    }

    if (!WraithX::InitializeAPI())
    {
        CliReport(Options, "result=error\nreason=api init failed\n");
        return 1;
    }

    return RunLoadFileMode(Options);
}

// The subcommands in the order the help lists them; each one's builder is in the file that runs
// it (CliCommands.h). "<game commands>": the games' own (GameCommand.h); "zone-walk" adds
// bo4-walk too, "disk-export" the games' aliases.
static const char* const kHelpOrder[] = {
    "self-test", "list-installs", "verify-opus", "decompress-ff", "identify",
    "bo3-images", "bo3-dump-xpak-entries", "salsa20-selftest", "bo3-decrypt-movie",
    "index-assets", "query-assets", "audio-probe",
    "casc-list", "xpak-index", "xsub-index", "xsub-extract", "casc-extract", "zone-decompress",
    "bo4-export-localize-disk", "bo4-export-rawstrings-disk", "zone-walk", "<game commands>", "disk-export",
    "decode-iwd", "gltf-selftest", "export-map-glb", "open-map", "cerberus-selftest", "seanim-selftest", "cast-selftest",
    "make-test-seanim", "render-anim", "coverage", "oat-extract",
    "xanimbin-dump", "xanimbin-selftest", "xanimbin-to-seanim", "xanimbin-export-dir",
    "oat-fx", "oat-fx-export", "oat-xmodel-materials", "oat-weapon", "oat-rawfiles", "gen-disc", "acts-extract",
    "dump-model", "dump-glb", "render-glb-png", "ui", "ui-grid-shot", "anim-rigs", "preview-window", "render-model", "render-fx",
    "sab-probe", "update-name-db", "update", "apply-update", "import-hash-csv", "bo4-decrypt", "file",
};

// CLI11 front end: a subcommand alone means "run headless". Shared options
// (--install/--out/--cache/--threads/--limit/--type/--report/--name-filter/format overrides/
// --acts-*/--oat-*) live on the parent App with fallthrough(). Fills CliOptions for RunCli.
static int RunHeadlessCli(int argc, char** argv, bool& CliInvoked)
{
    CliOptions Options;
    CliInvoked = false;


    // Let CLI11-printed help/error text reach an invoking console (a no-op without one).
    if (AttachConsole(ATTACH_PARENT_PROCESS))
    {
        FILE* OutF = nullptr;
        FILE* ErrF = nullptr;
        freopen_s(&OutF, "CONOUT$", "w", stdout);
        freopen_s(&ErrF, "CONOUT$", "w", stderr);
    }

    // The bare "verifiedhashes" token (no leading dashes) is scanned for directly in main()
    // before this function ever runs, and is not part of CLI11's grammar - drop it here so it
    // does not turn into an "unrecognized argument" parse error.
    std::vector<std::string> ArgStorage;
    for (int i = 1; i < argc; i++)
    {
        if (std::string(argv[i]) != "verifiedhashes") ArgStorage.push_back(argv[i]);
    }

    std::vector<char*> ArgvPtrs;
    ArgvPtrs.push_back(const_cast<char*>("GrayWolf.exe"));
    for (auto& A : ArgStorage) ArgvPtrs.push_back(const_cast<char*>(A.c_str()));

    CLI::App App{"GrayWolf - Call of Duty asset extractor (headless mode)", "GrayWolf"};
    App.fallthrough();
    App.require_subcommand(0, 1);

    // Shared options, usable by any subcommand
    AddSharedOptions(App, Options);

    // The subcommands, in the help's order. A subcommand with no options of its own sets its
    // CliOptions bool from its callback once parsed.
    CliCommandBuilders Builders;
    for (const auto Add : { AddMiscCommands, AddToolsCommands, AddAudioCommands, AddAssetExportCommands, AddCascCommands,
                            AddZoneWalkCommands, AddModelPreviewCommands, AddBlenderCommands, AddFxPreviewCommands,
                            AddBo3Commands, AddBo4StringCommands, AddGameCommands, AddDiskExportCommands })
        Add(Builders);
    for (const char* Name : kHelpOrder) Builders.at(Name)(App, Options);

    try
    {
        App.parse((int)ArgvPtrs.size(), ArgvPtrs.data());
    }
    catch (const CLI::ParseError& e)
    {
        CliInvoked = true;
        return App.exit(e);
    }


    if (App.get_subcommands().empty())
    {
        // No subcommand was invoked (a plain launch) - fall through to the classic GUI.
        return 0;
    }

    CliInvoked = true;
    g_CliMode = true;
    return RunCli(Options);
}

#ifdef _DEBUG
int main(int argc, char** argv)
#else
int APIENTRY WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nCmdShow)
#endif
{
    SetUnhandledExceptionFilter(MyUnhandledExceptionFilter);


    AppLog::Init();

    {
        SettingsManager::LoadSettings("graywolf", "greyhound",
        {
            { "exportimg", "PNG" },
            { "exportsnd", "WAV" },
            // The game folder picked in the shell, so it survives a restart.
            { "game_folder", "" },
            // blender.exe for Open in Blender (BlenderLauncher.h); empty: detected
            { "blender_path", "" },
            // the decompilers' exes (--decompile; OatBridge.h); empty: on PATH
            { "acts_exe", "" },
            { "gsc_tool_exe", "" },
            { "cod_luajit_decompiler_exe", "" },
            { "cod_lua_decompiler_exe", "" },
            // ffmpeg.exe, the WMA sounds' decoder (WmaSounds.h); empty: found (FfmpegFindExe)
            { "ffmpeg_exe", "" },
            // Updates (Updater.h): at GUI start, hash packages installed and a newer app announced
            { "auto_update", "true" },
            { "update_repo", "Kenshin9977/GrayWolf" },
            { "update_use_gh_token", "false" },
            { "keepsndpath", "true" },
            { "skipblankaudio", "false" },
            { "usesabindexbo4", "false" },
            { "exportmodelimg", "true" },
            { "exportalllods", "false" },
            { "exporthitbox", "false" },
            { "exportvtxcolor", "false" },
            { "exportimgnames", "false"},
            { "mdlmtlfolders", "true"},
            { "skipprevmodel", "true"},
            { "skipprevimg", "true"},
            { "skipprevsound", "true"},
            { "skipprevanim", "true"},
            { "export_ma", "false" },
            { "export_obj", "false" },
            { "export_xna", "false" },
            { "export_smd", "false" },
            { "export_xmexport", "false" },
            { "export_xmbin", "false" },
            // Models and anims in Cast (github.com/dtzxporter/cast) by default; SEModel/SEAnim stay
            // available. A graywolf.json that already has these keys keeps its choice.
            { "export_seanim", "false" },
            { "export_semodel", "false" },
            { "export_castanim", "true" },
            { "export_castmdl", "true" },
            { "export_gltf", "false" },
            { "export_glb", "false" },
            { "export_directxanim", "false" },
            { "directxanim_ver", "17" },
            { "global_images", "false" },
            { "patchnormals", "true" },
            { "patchcolor", "true" },
            { "showxanim", "true" },
            { "showxmodel", "true" },
            { "showximage", "false" },
            { "showxrawfiles", "false" },
            { "showxsounds", "false" },
            { "showxmtl", "false" },
            { "match_game_lod_index", "false" },
            { "remove_mdl_basename", "false" },
            { "unit", "Centimeter" }
        });

#ifndef _DEBUG
        int argc = __argc;
        char** argv = __argv;
#endif

        // Headless mode, never touches the UI - a subcommand was invoked (or --help/a parse
        // error was already handled) if CliInvoked comes back true.
        bool CliInvoked = false;
        const int CliResult = RunHeadlessCli(argc, argv, CliInvoked);
        if (CliInvoked)
        {
            return CliResult;
        }

        CleanupFilesystem();
        // GUI only (never a CLI run, so never an export or a regress); off with auto_update=false
        // or GRAYWOLF_NO_UPDATE. Runs on a worker thread.
        Updater::Background::Start(true);

        // A plain double-click (no CLI args) opens the Dear ImGui + D3D11 shell, same as `ui ""`
        // (empty asset list, filled from its own panels). The CLI still works without a D3D11 device.
        const bool Started = WraithX::InitializeAPI() && RunImGuiShell("");
        if (Started)
            WraithX::ShutdownAPI();
        else
            MessageBoxA(NULL, "GrayWolf could not start its window (it needs a Direct3D 11 device).\n"
                "The command line still works: GrayWolf.exe --help", "GrayWolf", MB_OK | MB_ICONEXCLAMATION);

        return Started ? 0 : -1;
    }

    return 0;
}


