#include "stdafx.h"

#include "ScriptExport.h"

#include "ExportNaming.h"
#include "ExportWriters.h"
#include "OatBridge.h"

#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

namespace Scripts
{
namespace
{
    // Name as a path under the scripts folder: forward slashes, no root, no "..", no character
    // Windows refuses in a file name.
    std::string Relative(std::string Name)
    {
        for (auto& Ch : Name) if (Ch == '\\') Ch = '/';
        std::string Out, Piece;
        std::stringstream Ss(Name);
        while (std::getline(Ss, Piece, '/'))
        {
            if (Piece.empty() || Piece == "." || Piece == "..") continue;
            Out += (Out.empty() ? "" : "/") + ExportNaming::SafeFileName(Piece);
        }
        return Out;
    }

    // ".gsc" or ".csc" (lower case): the name's own, .gsc when it has another or none
    std::string ScriptExtension(const std::string& Name)
    {
        const std::string Ext = ExportNaming::Lower(fs::path(Name).extension().string());
        return Ext == ".csc" ? ".csc" : ".gsc";
    }

    // the name without its .gsc/.csc
    std::string Stem(const std::string& Name)
    {
        const std::string Ext = ExportNaming::Lower(fs::path(Name).extension().string());
        return Ext == ".gsc" || Ext == ".csc" ? Name.substr(0, Name.size() - 4) : Name;
    }
}

bool IsSourceName(const std::string& Name)
{
    const std::string Ext = ExportNaming::Lower(fs::path(Name).extension().string());
    return Ext == ".gsc" || Ext == ".csc" || Ext == ".gsh" || Ext == ".csh";
}

std::vector<uint8_t> GscBin(uint32_t CompressedLen, uint32_t Len, const std::vector<uint8_t>& Stack,
                            const std::vector<uint8_t>& Bytecode)
{
    std::vector<uint8_t> Out = { 'G', 'S', 'C', 0 };
    for (uint32_t V : { CompressedLen, Len, (uint32_t)Bytecode.size() })
        for (int I = 0; I < 4; I++) Out.push_back((uint8_t)(V >> (8 * I)));
    Out.insert(Out.end(), Stack.begin(), Stack.end());
    Out.insert(Out.end(), Bytecode.begin(), Bytecode.end());
    return Out;
}

Sink::Sink(const DiskGame::Context& Ctx, DiskGame::Totals& Tot, std::string ZoneName, const Vm& Machine)
    : C(Ctx), T(Tot), Zone(std::move(ZoneName)),
      Dir((fs::path(Ctx.OutRoot) / DiskGame::KindFolder(DiskGame::Kind::Script)).string()),   // made by the first file
      V(Machine)
{
    Decompiling = DiskGame::ScriptDecompile.Of(C.Settings) == "on" && V.Decompiler != Tool::None;
    if (Decompiling)
    {
        const std::string Exe = V.Decompiler == Tool::Acts ? ActsFindExe(C.Setting("acts_exe"))
                                                           : GscToolFindExe(C.Setting("gsc_tool_exe"));
        if (Exe.empty())
        {
            Decompiling = false;
            NoTool = true;
        }
        else if (V.StringsDump != nullptr)
        {
            std::error_code Ec;
            const std::string Named = C.Setting("game_exe");
            const fs::path InDeps = fs::path(Exe).parent_path() / "deps" / V.StringsDump;
            const fs::path Dump = Named.empty() ? InDeps : fs::path(Named);
            if (fs::is_regular_file(Dump, Ec)) DecryptExe = Dump.string();
        }
    }
}

Sink::~Sink()
{
    Finish();
}

void Sink::Add(Script S)
{
    auto& Kt = T.Kinds[DiskGame::Kind::Script];
    Kt.Seen++;
    const std::string Rel = Relative(S.Name);
    if (Rel.empty())
    {
        T.Fail(Zone, DiskGame::Kind::Script, S.Name, "no file name");
        return;
    }
    if (!S.Compiled)
    {
        if (ExportWriters::WriteBytes(fs::path(Dir) / fs::path(Rel).make_preferred(), S.Bytes.data(), S.Bytes.size())) Kt.Written++;
        else T.Fail(Zone, DiskGame::Kind::Script, S.Name, "write failed");
        return;
    }
    if (!Decompiling)
    {
        if (NoTool) T.Counters["scripts_no_decompiler"]++;
        WriteBytecode(Rel, S.Bytes);
        return;
    }
    // the tool's input named by its place in the queue: a script's own name may hold anything
    const std::string Input = "s" + std::to_string(Queue.size()) + ScriptExtension(Rel) + V.Suffix;
    Queue.push_back({ Rel, Input, std::move(S.Bytes) });
}

void Sink::WriteBytecode(const std::string& Rel, const std::vector<uint8_t>& Bytes)
{
    auto& Kt = T.Kinds[DiskGame::Kind::Script];
    const std::string File = Stem(Rel) + ScriptExtension(Rel) + V.Suffix;
    if (!ExportWriters::WriteBytes(fs::path(Dir) / fs::path(File).make_preferred(), Bytes.data(), Bytes.size()))
    {
        T.Fail(Zone, DiskGame::Kind::Script, Rel, "write failed");
        return;
    }
    Kt.Written++;
    T.Counters["scripts_bytecode"]++;
}

void Sink::Finish()
{
    if (Queue.empty()) return;
    Decompile();
    Queue.clear();
}

void Sink::Decompile()
{
    const fs::path Work = ToolScratchPath("gh_scripts");
    std::error_code Ec;
    fs::remove_all(Work, Ec);

    // One run over a folder of inputs; where the tool writes the source of an input.
    // acts runs the dump's decryption in its own process, where it finds its thread-local storage
    // at the dump's tls_index (0: acts' own block, far smaller): some runs crash on the first
    // string, the others decrypt every one. Such a run is tried again; a dump acts finds no
    // decryption in is dropped, the strings then left encrypted.
    constexpr int DecryptTries = 6;
    const auto Run = [&](const fs::path& Base, const std::vector<const Pending*>& Items, std::string& Log) {
        const fs::path In = Base / "in";
        for (const Pending* P : Items) ExportWriters::WriteBytes(In / P->Input, P->Bytes.data(), P->Bytes.size());
        if (V.Decompiler != Tool::Acts)
            return GscToolDecompileDir(C.Setting("gsc_tool_exe"), V.Game, V.System, In.string(), Base.string(), Log);
        for (int Try = 1;; Try++)
        {
            const bool Ran = ActsDecompileDir(C.Setting("acts_exe"), In.string(), (Base / "out").string(), Log, DecryptExe);
            if (DecryptExe.empty()) return Ran;
            if (ActsNoDecryption(Log)) DecryptExe.clear();   // acts exits -1 there, as for a script it fails
            else if (Ran || Try == DecryptTries) return Ran;
            else T.Counters["scripts_decrypt_runs_retried"]++;
            std::error_code RetryEc;
            fs::remove_all(Base / "out", RetryEc);
        }
    };
    // acts: out/<stem>.gsc; gsc-tool: decompiled/<game>/<stem>.gsc|.csc, or the input's own name
    // (T6). Empty for a script without code (a header-only GSCOBJ). acts opens its output before
    // decompiling ("Decompiling into '<file>'..."): in a run that crashed, the file of the last
    // such line is the script it crashed on, not one it decompiled.
    const auto Output = [&](const fs::path& Base, const Pending& P, const std::string& Log, bool Ran, std::vector<uint8_t>& Source) {
        const std::string StemOf = fs::path(P.Input).stem().string();
        if (!Ran && V.Decompiler == Tool::Acts)
        {
            const size_t Last = Log.rfind("Decompiling into '");
            const size_t End = Last == std::string::npos ? Last : Log.find('\'', Last + 18);
            if (End != std::string::npos && fs::path(Log.substr(Last + 18, End - Last - 18)).stem().string() == StemOf) return false;
        }
        std::vector<fs::path> Candidates;
        if (V.Decompiler == Tool::Acts) Candidates = { Base / "out" / (StemOf + ".gsc") };
        else
        {
            const fs::path D = Base / "decompiled" / V.Game;
            Candidates = { D / (StemOf + ScriptExtension(P.Name)), D / P.Input };
        }
        for (const fs::path& F : Candidates)
            if (ExportWriters::ReadBytes(F, Source)) return true;
        return false;
    };

    std::vector<const Pending*> All;
    for (const Pending& P : Queue) All.push_back(&P);
    std::string Log;
    const bool Ran = Run(Work, All, Log);
    const bool Encrypted = V.StringsDump != nullptr && DecryptExe.empty();

    auto& Kt = T.Kinds[DiskGame::Kind::Script];
    size_t Retry = 0;
    for (size_t I = 0; I < Queue.size(); I++)
    {
        const Pending& P = Queue[I];
        std::vector<uint8_t> Source;
        bool Got = Output(Work, P, Log, Ran, Source);
        // a run that stopped short (a crash on one script): the ones it did not reach, alone
        if (!Got && !Ran)
        {
            const fs::path Alone = Work / ("retry" + std::to_string(Retry++));
            std::string AloneLog;
            const bool AloneRan = Run(Alone, { &P }, AloneLog);
            Got = Output(Alone, P, AloneLog, AloneRan, Source);
            fs::remove_all(Alone, Ec);
        }
        if (!Got)
        {
            T.Counters["scripts_decompile_failed"]++;
            WriteBytecode(P.Name, P.Bytes);
            continue;
        }
        const std::string File = Stem(P.Name) + ScriptExtension(P.Name);
        if (ExportWriters::WriteBytes(fs::path(Dir) / fs::path(File).make_preferred(), Source.data(), Source.size()))
        {
            Kt.Written++;
            T.Counters["scripts_decompiled"]++;
            if (Encrypted) T.Counters["scripts_strings_encrypted"]++;
        }
        else
            T.Fail(Zone, DiskGame::Kind::Script, P.Name, "write failed");
    }
    if (!Ran) T.Counters["scripts_decompiler_runs_cut_short"]++;
    fs::remove_all(Work, Ec);
}
}
