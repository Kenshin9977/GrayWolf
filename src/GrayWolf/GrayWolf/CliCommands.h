#pragma once

#include "CliOptions.h"

#include <functional>
#include <map>
#include <string>

namespace CLI
{
    class App;
}

// The command line's grammar. Each subcommand is built by the file that runs it: a builder adds it
// to App, its options bound to Options; Main.cpp adds them in the help's order (kHelpOrder).
using CliCommandBuilder = std::function<void(CLI::App& App, CliOptions& Options)>;
using CliCommandBuilders = std::map<std::string, CliCommandBuilder>;   // by subcommand name

// --install, --out, --type, the format overrides...: on the parent app, any subcommand's
void AddSharedOptions(CLI::App& App, CliOptions& Options);   // CliCommands.Shared.cpp

void AddMiscCommands(CliCommandBuilders& B);           // CliCommands.Misc.cpp
void AddToolsCommands(CliCommandBuilders& B);          // CliCommands.Tools.cpp
void AddAudioCommands(CliCommandBuilders& B);          // CliCommands.Audio.cpp
void AddAssetExportCommands(CliCommandBuilders& B);    // CliCommands.AssetExport.cpp
void AddCascCommands(CliCommandBuilders& B);           // CliCommands.Casc.cpp
void AddZoneWalkCommands(CliCommandBuilders& B);       // CliCommands.ZoneWalk.cpp
void AddModelPreviewCommands(CliCommandBuilders& B);   // CliCommands.ModelPreview.cpp
void AddBlenderCommands(CliCommandBuilders& B);        // CliCommands.Blender.cpp
void AddFxPreviewCommands(CliCommandBuilders& B);      // FxPreviewCli.cpp
void AddBo3Commands(CliCommandBuilders& B);            // Games/T7/CliCommands.BO3.cpp
void AddBo4StringCommands(CliCommandBuilders& B);      // Games/T8/CliCommands.Bo4Strings.cpp
void AddGameCommands(CliCommandBuilders& B);           // GameCommand.cpp: the games' own (GameCommand.h)
void AddDiskExportCommands(CliCommandBuilders& B);     // DiskGame.Cli.cpp: disk-export and its aliases

int RunSelfTest(const CliOptions& Options);
int RunListInstalls(const CliOptions& Options);
int RunIdentify(const CliOptions& Options);
int RunDecompressFF(const CliOptions& Options);

int RunIndexAssets(const CliOptions& Options);
int RunQueryAssets(const CliOptions& Options);
int RunLoadFileMode(const CliOptions& Options);

int RunOatExtract(const CliOptions& Options);
int RunCoverage(const CliOptions& Options);
int RunOatFx(const CliOptions& Options);
int RunOatFxExport(const CliOptions& Options);
int RunOatXModelMtl(const CliOptions& Options);
int RunOatWeapon(const CliOptions& Options);
int RunOatRawfiles(const CliOptions& Options);
int RunActsExtract(const CliOptions& Options);
int RunUpdateNameDb(const CliOptions& Options);
int RunUpdate(const CliOptions& Options);
int RunApplyUpdate(const CliOptions& Options);
int RunImportHashCsv(const CliOptions& Options);

int RunBo3Images(const CliOptions& Options);
int RunBo3DumpXpakEntries(const CliOptions& Options);
int RunSalsa20SelfTest(const CliOptions& Options);
int RunBo3DecryptMovie(const CliOptions& Options);

int RunCascList(const CliOptions& Options);
int RunXpakIndex(const CliOptions& Options);
int RunXsubIndex(const CliOptions& Options);
int RunXsubExtract(const CliOptions& Options);
int RunCascExtractDir(const CliOptions& Options);
int RunZoneDecompress(const CliOptions& Options);
int RunBo4ExportLocalizeDisk(const CliOptions& Options);
int RunBo4ExportRawStringsDisk(const CliOptions& Options);
int RunBo4Decrypt(const CliOptions& Options);

int RunZoneWalk(const CliOptions& Options);

int RunBo4ZoneDiag(const CliOptions& Options, const std::string& Input);

int RunAudioProbe(const CliOptions& Options);
int RunDecodeIwd(const CliOptions& Options);
int RunVerifyOpus(const CliOptions& Options);
int RunSabProbe(const CliOptions& Options);

int RunGltfSelfTest(const CliOptions& Options);
int RunExportMapGlb(const CliOptions& Options);
int RunOpenMap(const CliOptions& Options);
int RunCerberusSelfTest(const CliOptions& Options);
int RunSeAnimSelfTest(const CliOptions& Options);
int RunCastSelfTest(const CliOptions& Options);
int RunMakeTestSeAnim(const CliOptions& Options);
int RunRenderAnimModel(const CliOptions& Options);
int RunXAnimBinDump(const CliOptions& Options);
int RunXAnimBinSelfTest(const CliOptions& Options);
int RunXAnimBinToSeAnimIn(const CliOptions& Options);
int RunXAnimBinExportDirIn(const CliOptions& Options);
int RunGenDisc(const CliOptions& Options);
int RunDumpModel(const CliOptions& Options);
int RunDumpGlb(const CliOptions& Options);
int RunRenderGlbPng(const CliOptions& Options);
int RunUiMode(const CliOptions& Options);
int RunUiGridShot(const CliOptions& Options);
int RunUiRecord(const CliOptions& Options);
int RunPreviewWindow(const CliOptions& Options);
int RunRenderModelCli(const CliOptions& Options);
int RunRenderFxCli(const CliOptions& Options);   // FxPreviewCli.cpp
int RunAnimRigs(const CliOptions& Options);      // AssetGrid/AnimRigsCli.cpp

