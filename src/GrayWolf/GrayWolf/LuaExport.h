#pragma once

// A zone's compiled Lua files (ZonekitZone's rawfile kind), decompiled with the decompile setting
// on (DiskGame::ScriptDecompile, --decompile): one run of each decompiler per zone over the files
// still left, by the bytecode's header (OatBridge.h), external processes like acts:
// - Call of Duty LuaJIT, ESC "LJ" 0x82 (Cold War), 0x83 (MWII, MWIII), 0x84 (Black Ops 6, 7):
//   cod-luajit-decompiler (0x84 with its --uleb-hash first, the hash form Black Ops 6 has), then
//   CoDLuaDecompiler for a file it does not decompile;
// - LuaJIT 2, ESC "LJ" 2 (Modern Warfare 2019), and Havok Script, ESC "Lua" (Black Ops 4,
//   Ghosts to WWII): CoDLuaDecompiler.
// A file is written as <name>.lua with its source, or as its bytecode <name>.luac when no tool
// decompiles it (counted, never a failure of the run).

#include "DiskGame.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace LuaScripts
{
    // Compiled Lua bytecode by its header (ESC "Lua" or ESC "LJ")
    bool IsBytecode(const std::vector<uint8_t>& Bytes);

    // One zone's compiled Lua files. Counters: lua_decompiled, lua_decompile_failed (bytecode
    // kept), lua_no_decompiler (decompiling asked for, no tool for the file found),
    // lua_decompiler_runs_cut_short.
    class Sink
    {
    public:
        Sink(const DiskGame::Context& C, DiskGame::Totals& T, std::string Zone);
        Sink(const Sink&) = delete;
        Sink& operator=(const Sink&) = delete;
        ~Sink();
        // the decompile setting on
        bool Decompiling() const { return On; }
        // File: the bytecode's file, <name>.luac (the source goes to <name>.lua); Seen counted by the caller
        void Add(std::filesystem::path File, std::vector<uint8_t> Bytes);
        // the decompilers' runs over the files added since the last one, their files written
        void Finish();

    private:
        struct Pending
        {
            std::filesystem::path File;
            std::vector<uint8_t> Bytes;
            std::vector<uint8_t> Source;   // set once a tool decompiles it
        };
        void WriteBytecode(const Pending& P);

        const DiskGame::Context& C;
        DiskGame::Totals& T;
        std::string Zone;
        bool On = false;
        std::string LuaJitExe, CodLuaExe;   // the tools found, empty when not
        std::vector<Pending> Queue;
    };
}
