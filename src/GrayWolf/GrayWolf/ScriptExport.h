#pragma once

#include "ByteView.h"

// The scripts kind (DiskGame::Kind::Script), the same for every game. A reader hands each script
// of a zone to a Sink as the zone stores it: source text, or compiled bytecode in the form its
// decompiler reads. Its game says which decompiler its VM takes (Vm). Source is written as is
// under scripts/<name>; compiled scripts as bytecode, or with the decompile setting on decompiled
// in one run of the tool per zone (acts or gsc-tool, external processes, OatBridge.h), the
// bytecode kept for a script the tool does not decompile.

#include "DiskGame.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Scripts
{
    enum class Tool { None, Acts, GscTool };

    // A script VM: the decompiler that reads it and how its compiled files are named.
    struct Vm
    {
        Tool Decompiler = Tool::None;
        const char* Game = "";       // gsc-tool -g
        const char* System = "pc";   // gsc-tool -s
        // a compiled script's extension after its .gsc/.csc: "bin" for gsc-tool's .gscbin (IW),
        // "c" for a Treyarch GSCOBJ (.gscc)
        const char* Suffix = "c";
        // a VM whose string literals the game encrypts (acts): the file name of the game's dumped
        // exe acts decrypts them with, looked for in acts' deps folder when the game_exe setting
        // (--game-exe) does not name one
        const char* StringsDump = nullptr;
    };
    // A Treyarch GSCOBJ read by acts (BO3 and later)
    inline constexpr Vm Acts{ Tool::Acts, "", "pc", "c" };
    // An IW scriptfile as gsc-tool's .gscbin
    constexpr Vm GscToolBin(const char* Game, const char* System = "pc") { return { Tool::GscTool, Game, System, "bin" }; }

    struct Script
    {
        // its file: "maps/mp/_utility.gsc", "scripts/zm/zm_bgb.csc"; .gsc assumed when it has no
        // extension (an IW scriptfile: "maps/mp/_utility", or a number gsc-tool names)
        std::string Name;
        std::vector<uint8_t> Bytes;
        bool Compiled = false;
    };

    // A script file by its extension: .gsc .csc .gsh .csh
    bool IsSourceName(const std::string& Name);

    // gsc-tool's .gscbin of an IW scriptfile: "GSC\0", compressedLen, len, bytecodeLen (u32 each),
    // the compressed stack, the bytecode
    std::vector<uint8_t> GscBin(uint32_t CompressedLen, uint32_t Len, ByteView Stack,
                                ByteView Bytecode);

    // One zone's scripts, written into the run's scripts/ folder. Counts Seen and Written of the
    // kind; scripts_decompiled, scripts_decompile_failed (bytecode kept), scripts_bytecode,
    // scripts_no_decompiler (the tool not found) and scripts_strings_encrypted (decompiled with
    // their strings left encrypted: the VM's dump not found, or acts finds no decryption in it) and
    // scripts_decrypt_runs_retried in the run's counters.
    class Sink
    {
    public:
        Sink(const DiskGame::Context& C, DiskGame::Totals& T, std::string Zone, const Vm& V);
        Sink(const Sink&) = delete;
        Sink& operator=(const Sink&) = delete;
        ~Sink();
        void Add(Script S);
        // the decompiler's run over the compiled scripts added since the last one
        void Finish();

    private:
        struct Pending
        {
            std::string Name, Input;   // the script, its file under the tool's input folder
            std::vector<uint8_t> Bytes;
        };
        void WriteBytecode(const std::string& Name, ByteView Bytes);
        void Decompile();

        const DiskGame::Context& C;
        DiskGame::Totals& T;
        std::string Zone, Dir;
        Vm V;
        bool Decompiling = false;
        bool NoTool = false;   // decompiling asked for, and the game's tool is not found
        std::string DecryptExe;   // the VM's StringsDump found
        std::vector<Pending> Queue;
    };
}
