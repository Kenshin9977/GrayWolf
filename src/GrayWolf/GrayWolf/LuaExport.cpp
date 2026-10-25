#include "stdafx.h"

#include "LuaExport.h"

#include "ExportWriters.h"
#include "OatBridge.h"

#include <functional>
#include <map>

namespace fs = std::filesystem;

namespace LuaScripts
{
namespace
{
    enum class Tool { LuaJit, CodLua };
    // one run of a tool with its options
    struct Attempt
    {
        Tool By;
        std::vector<std::string> Args;
        bool operator<(const Attempt& O) const { return By != O.By ? By < O.By : Args < O.Args; }
    };

    // The runs a file is given, in order, until one decompiles it. cod-luajit-decompiler reads
    // Black Ops 6's 0x84 hashes with --uleb-hash and Black Ops 7's without: a file without hashes
    // reads the same either way.
    std::vector<Attempt> AttemptsFor(const std::vector<uint8_t>& B)
    {
        const Attempt CodLua{ Tool::CodLua, {} };
        if (B.size() > 3 && B[0] == 0x1b && B[1] == 'L' && B[2] == 'J')
        {
            if (B[3] == 0x84) return { { Tool::LuaJit, {} }, { Tool::LuaJit, { "--uleb-hash" } }, CodLua };
            if (B[3] == 0x82 || B[3] == 0x83) return { { Tool::LuaJit, {} }, CodLua };
        }
        return { CodLua };
    }
}

bool IsBytecode(const std::vector<uint8_t>& B)
{
    return B.size() > 3 && B[0] == 0x1b && ((B[1] == 'L' && B[2] == 'J') || (B[1] == 'L' && B[2] == 'u' && B[3] == 'a'));
}

Sink::Sink(const DiskGame::Context& Ctx, DiskGame::Totals& Tot, std::string ZoneName)
    : C(Ctx), T(Tot), Zone(std::move(ZoneName))
{
    On = DiskGame::ScriptDecompile.Of(C.Settings) == "on";
    if (!On) return;
    LuaJitExe = CodLuaJitFindExe(C.Setting("cod_luajit_decompiler_exe"));
    CodLuaExe = CodLuaFindExe(C.Setting("cod_lua_decompiler_exe"));
}

Sink::~Sink()
{
    Finish();
}

void Sink::Add(fs::path File, std::vector<uint8_t> Bytes)
{
    Queue.push_back({ std::move(File), std::move(Bytes), {} });
}

void Sink::WriteBytecode(const Pending& P)
{
    if (ExportWriters::WriteBytes(P.File, P.Bytes.data(), P.Bytes.size())) T.Kinds[DiskGame::Kind::Rawfile].Written++;
    else T.Fail(Zone, DiskGame::Kind::Rawfile, P.File.filename().string(), "write failed");
}

void Sink::Finish()
{
    if (Queue.empty()) return;
    const fs::path Work = ToolScratchPath("gh_lua");
    std::error_code Ec;
    fs::remove_all(Work, Ec);
    size_t Runs = 0;

    // One run of an attempt over Items (indices into Queue), the ones it decompiles given their
    // Source. A run that stopped short (a crash on one file, a timeout): the files it neither
    // decompiled nor named as failed go again, in two halves, until the one it stops on is alone.
    const std::function<void(const Attempt&, const std::vector<size_t>&)> Run = [&](const Attempt& A, const std::vector<size_t>& Items) {
        const fs::path Base = Work / ("run" + std::to_string(Runs++));
        const fs::path In = Base / "in", Out = Base / "out";
        for (size_t I : Items)
            ExportWriters::WriteBytes(In / ("f" + std::to_string(I) + ".luac"), Queue[I].Bytes.data(), Queue[I].Bytes.size());
        std::string Log;
        const bool Ran = A.By == Tool::LuaJit ? CodLuaJitDecompileDir(LuaJitExe, A.Args, In.string(), Out.string(), Log)
                                              : CodLuaDecompileDir(CodLuaExe, In.string(), Out.string(), Log);
        std::vector<size_t> Left;
        for (size_t I : Items)
        {
            const std::string Stem = "f" + std::to_string(I);
            const fs::path Source = Out / (Stem + (A.By == Tool::LuaJit ? ".lua" : ".dec.lua"));
            if (ExportWriters::ReadBytes(Source, Queue[I].Source) && !Queue[I].Source.empty()) continue;
            Queue[I].Source.clear();
            const std::string Named = Stem + ".luac";
            if (!Ran && Log.find("\\" + Named) == std::string::npos && Log.find("/" + Named) == std::string::npos) Left.push_back(I);
        }
        fs::remove_all(Base, Ec);
        if (Ran || Left.empty()) return;
        T.Counters["lua_decompiler_runs_cut_short"]++;
        if (Left.size() == 1 && Items.size() == 1) return;
        const auto Half = Left.begin() + (ptrdiff_t)(Left.size() + 1) / 2;
        Run(A, std::vector<size_t>(Left.begin(), Half));
        if (Half != Left.end()) Run(A, std::vector<size_t>(Half, Left.end()));
    };

    // the attempts in rounds: a file's first attempt, then its second for the ones still left...
    std::vector<std::vector<Attempt>> Chains;
    bool AnyTool = false;
    for (const Pending& P : Queue)
    {
        std::vector<Attempt> Chain;
        for (const Attempt& A : AttemptsFor(P.Bytes))
            if (!(A.By == Tool::LuaJit ? LuaJitExe : CodLuaExe).empty()) Chain.push_back(A);
        AnyTool = AnyTool || !Chain.empty();
        Chains.push_back(std::move(Chain));
    }
    for (size_t Round = 0; AnyTool; Round++)
    {
        std::map<Attempt, std::vector<size_t>> Groups;
        for (size_t I = 0; I < Queue.size(); I++)
            if (Queue[I].Source.empty() && Round < Chains[I].size()) Groups[Chains[I][Round]].push_back(I);
        if (Groups.empty()) break;
        for (const auto& [A, Items] : Groups) Run(A, Items);
    }
    fs::remove_all(Work, Ec);

    auto& Kt = T.Kinds[DiskGame::Kind::Rawfile];
    for (size_t I = 0; I < Queue.size(); I++)
    {
        const Pending& P = Queue[I];
        if (P.Source.empty())
        {
            T.Counters[Chains[I].empty() ? "lua_no_decompiler" : "lua_decompile_failed"]++;
            WriteBytecode(P);
            continue;
        }
        fs::path File = P.File;
        File.replace_extension(".lua");
        if (ExportWriters::WriteBytes(File, P.Source.data(), P.Source.size()))
        {
            Kt.Written++;
            T.Counters["lua_decompiled"]++;
        }
        else
            T.Fail(Zone, DiskGame::Kind::Rawfile, File.filename().string(), "write failed");
    }
    Queue.clear();
}
}
