#pragma once

#include <filesystem>
#include <string>
#include <vector>

// A path of its own under the system's temp folder, Prefix_<process>_<n>: where a tool run's
// inputs and outputs go
std::filesystem::path ToolScratchPath(const char* Prefix);

// OatBridge drives ate47's "acts" CLI (a separate process) to extract MODERN Call of Duty
// fastfiles that our native memory path doesn't cover. Old CoD (IW3-T6) fastfiles are extracted
// IN-PROCESS instead, via OAT's DumpZone pipeline (OatDumpZoneInProcess, OatInProcess.h) - this
// file covers only the modern-CoD "acts" path.

// ate47 / Atian CoD Tools ("acts"): the modern-CoD counterpart (BO4/CW/Vanguard/MW19/MWII/
//     MWIII/BO6, incl. GSC decompilation). Separate-process pattern. GPL-3/MIT.

struct ActsExtractOptions
{
    std::string Handler;      // force a game handler (e.g. "bo4","cw","mwiii","bo6"); empty = auto
    std::string Assets;       // comma-separated asset types to dump (empty = all)
    std::string GamePath;     // -G game path (some handlers need the game for hashes/keys)
    bool DisableScriptDecomp = false; // --disableScriptsDecomp
    bool Graphic = false;     // --graphic (dump graphic assets)
};

// Locates the acts CLI: Hint if it exists (--acts-exe, the acts_exe setting), else acts.exe on
// PATH. Empty if none.
std::string ActsFindExe(const std::string& Hint);

// Runs `acts fastfile -d -o <OutputDir> <ff>` (plus options) to dump a modern-CoD fastfile.
// `-d` ("dump decompressed") is always added - without it acts only parses the header and
// writes nothing. `-p -i` ("use patch files", "ignore missing fd") are added automatically for
// a BO3-style `*_patch.ff`, which never has a matching `.fd` in a normal install. Output is
// captured to <OutputDir>/acts_fastfile.log and mirrored into Log. Returns true on exit 0.
bool ActsExtractFastFile(const std::string& ActsExe, const std::string& FastFilePath,
                         const std::string& OutputDir, const ActsExtractOptions& Opts, std::string& Log);

// Runs `acts gscd -g` on ONE already-on-disk .gsc/.csc bytecode file and returns the decompiled
// GSC source it writes. acts resolves hash-named symbols via a package_index .wni next to
// acts.exe (see NameDbUpdater::ConvertLocalCsv for the BO3 script-hash one). ScriptPath's
// extension picks the bytecode format; OutDir is a caller-owned scratch dir. Returns false on
// anything but a clean decompile (a garbled or partial script is a real failure mode here).
bool ActsDecompileGsc(const std::string& ActsExe, const std::string& ScriptPath,
                      const std::string& OutDir, std::string& OutSource, std::string& Log);

// gsc-tool (xensik/gsc-tool, GPL-3): the IW-engine and T6 script decompiler, an external process
// like acts. Located like ActsFindExe: Hint if it exists (--gsc-tool-exe, the gsc_tool_exe
// setting), else gsc-tool.exe on PATH.
std::string GscToolFindExe(const std::string& Hint);

// One run of a decompiler over every compiled script under InDir; a script it does not decompile
// has no output file (acts: but the one it crashed on). False when the run did not go through its
// inputs (not found, crashed, timed out after 30 minutes); Log is the tool's output.
// acts: <InDir>/<rel>.gscc|.cscc (a Treyarch GSCOBJ) -> <OutDir>/<rel>.gsc. DecryptExe, when not
// empty: the game's dumped exe, whose string decryption acts finds (its data/decrypt.json
// signatures, --decrypt-mod) and runs on the scripts' encrypted strings; a dump it finds no
// decryption in stops the run before any script (exit -1, true), ActsNoDecryption(Log) then true.
bool ActsDecompileDir(const std::string& ActsExe, const std::string& InDir, const std::string& OutDir, std::string& Log,
                      const std::string& DecryptExe = "");
bool ActsNoDecryption(const std::string& Log);
// gsc-tool -g Game -s System, run in WorkDir: <InDir>/<rel>.gscbin|.cscbin (IW) ->
// <WorkDir>/decompiled/<Game>/<rel>.gsc|.csc; <InDir>/<file>.gscc|.cscc (T6) ->
// <WorkDir>/decompiled/<Game>/<file>.gscc|.cscc holding the source.
bool GscToolDecompileDir(const std::string& GscToolExe, const std::string& Game, const std::string& System,
                         const std::string& InDir, const std::string& WorkDir, std::string& Log);

// The Lua decompilers, external processes like acts (LuaExport.h picks one by the bytecode's header).
// Located like ActsFindExe: Hint if it exists (the cod_luajit_decompiler_exe and cod_lua_decompiler_exe
// settings), else on PATH. Both resolve hashes through the .wni files of a PackageIndex folder beside
// their own exe (none: hashes left as numbers).
// cod-luajit-decompiler (Nico-Posada, MIT): Call of Duty LuaJIT, versions 0x82 (Cold War), 0x83
// (MWII, MWIII), 0x84 (Black Ops 6, 7); cod-luajit-decompiler.exe or its release's
// cod-luajit-decompiler-windows-x86_64.exe on PATH.
std::string CodLuaJitFindExe(const std::string& Hint);
// CoDLuaDecompiler (JariKCoding, Nico-Posada's fork, MIT, .NET): Havok Script (Black Ops II to 4,
// Ghosts to WWII) and LuaJIT 2 (Modern Warfare 2019), the Call of Duty LuaJIT versions too;
// CoDLuaDecompiler.CLI.exe on PATH.
std::string CodLuaFindExe(const std::string& Hint);
// One run over every <InDir>/<file>.luac: decompiled to <OutDir>/<file>.lua (cod-luajit-decompiler,
// Args its options) or <OutDir>/<file>.dec.lua (CoDLuaDecompiler); a file it does not decompile has
// none. False when the run did not go through its inputs (not found, crashed, timed out after 30
// minutes); Log is the tool's output.
bool CodLuaJitDecompileDir(const std::string& Exe, const std::vector<std::string>& Args, const std::string& InDir,
                           const std::string& OutDir, std::string& Log);
bool CodLuaDecompileDir(const std::string& Exe, const std::string& InDir, const std::string& OutDir, std::string& Log);

// ffmpeg (an external process like the decompilers; GrayWolf ships none): the WMA sounds' decoder
// (WmaSounds.h). Hint if it exists (--ffmpeg-exe, the ffmpeg_exe setting), else ffmpeg.exe beside
// GrayWolf's exe, on PATH, or where WinGet (its Links, then Gyan.FFmpeg* packages' */bin), Chocolatey
// or Scoop install it. Empty if none.
std::string FfmpegFindExe(const std::string& Hint);
// ffmpeg decoding InPath's first audio stream to a 16-bit PCM WAV at OutPath (no metadata chunk);
// true when it exited 0 and wrote it. Log is its output (errors only).
bool FfmpegDecodeToWav(const std::string& Exe, const std::string& InPath, const std::string& OutPath, std::string& Log);
