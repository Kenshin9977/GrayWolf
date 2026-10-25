#include "stdafx.h"

#include "OatBridge.h"
#include "FileSystems.h"
#include "Strings.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <vector>
#include <cctype>

namespace fs = std::filesystem;

namespace
{
    // Quotes an argument for a Windows command line (wraps in quotes, escapes embedded quotes).
    std::wstring Quote(const std::wstring& Arg)
    {
        std::wstring Out = L"\"";
        for (wchar_t C : Arg)
        {
            if (C == L'"') Out += L"\\\"";
            else Out += C;
        }
        Out += L"\"";
        return Out;
    }

    // A command line: the exe quoted, then words as they are (Add) or quoted (AddQ)
    struct CommandLine
    {
        std::wstring Text;
        explicit CommandLine(const std::string& Exe) : Text(Quote(Strings::ToUnicodeString(Exe))) {}
        void Add(const std::wstring& S) { Text += L" " + S; }
        void AddQ(const std::string& S) { Text += L" " + Quote(Strings::ToUnicodeString(S)); }
    };

    // This process' environment with Set's variables set (replaced when there), as CreateProcessW's
    // Unicode block: NAME=value\0 ... \0
    std::wstring EnvironmentWith(const std::vector<std::pair<std::wstring, std::wstring>>& Set)
    {
        std::wstring Block;
        if (wchar_t* Env = GetEnvironmentStringsW())
        {
            for (const wchar_t* P = Env; *P; P += wcslen(P) + 1)
            {
                const std::wstring Entry = P;
                const size_t Eq = Entry.find(L'=', 1);
                const std::wstring Name = Entry.substr(0, Eq);
                bool Replaced = false;
                for (const auto& [N, V] : Set) Replaced = Replaced || _wcsicmp(N.c_str(), Name.c_str()) == 0;
                if (!Replaced) Block += Entry + L'\0';
            }
            FreeEnvironmentStringsW(Env);
        }
        for (const auto& [N, V] : Set) Block += N + L"=" + V + L'\0';
        return Block + L'\0';
    }

    // Runs a child process in WorkDir with stdout/stderr captured to LogPath, mirrored into Log.
    // Its exit code; a process still running after TimeoutMs is killed (RunTimedOut). Env: variables
    // set in the child's environment.
    constexpr DWORD RunFailed = 0xFFFFFFF0, RunTimedOut = 0xFFFFFFF1;
    DWORD RunProcess(const std::wstring& Cmd, const std::string& WorkDir, const std::string& LogPath, std::string& Log,
                     DWORD TimeoutMs = INFINITE, const std::vector<std::pair<std::wstring, std::wstring>>& Env = {})
    {
        SECURITY_ATTRIBUTES Sa{};
        Sa.nLength = sizeof(Sa);
        Sa.bInheritHandle = TRUE;
        HANDLE LogHandle = CreateFileW(Strings::ToUnicodeString(LogPath).c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                       &Sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (LogHandle == INVALID_HANDLE_VALUE) { Log = "cannot open log file"; return RunFailed; }

        STARTUPINFOW Si{};
        Si.cb = sizeof(Si);
        Si.dwFlags = STARTF_USESTDHANDLES;
        Si.hStdOutput = LogHandle;
        Si.hStdError = LogHandle;
        Si.hStdInput = nullptr;

        PROCESS_INFORMATION Pi{};
        std::vector<wchar_t> Mutable(Cmd.begin(), Cmd.end());
        Mutable.push_back(0);
        const std::wstring Dir = Strings::ToUnicodeString(WorkDir);
        std::wstring Block = Env.empty() ? std::wstring() : EnvironmentWith(Env);

        const BOOL Ok = CreateProcessW(nullptr, Mutable.data(), nullptr, nullptr, TRUE,
                                       CREATE_NO_WINDOW | (Env.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT),
                                       Env.empty() ? nullptr : Block.data(), Dir.c_str(), &Si, &Pi);
        CloseHandle(LogHandle);
        if (!Ok) { Log = "CreateProcess failed"; return RunFailed; }

        DWORD ExitCode = 1;
        if (WaitForSingleObject(Pi.hProcess, TimeoutMs) == WAIT_TIMEOUT)
        {
            TerminateProcess(Pi.hProcess, RunTimedOut);
            WaitForSingleObject(Pi.hProcess, INFINITE);
            ExitCode = RunTimedOut;
        }
        else
            GetExitCodeProcess(Pi.hProcess, &ExitCode);
        CloseHandle(Pi.hProcess);
        CloseHandle(Pi.hThread);

        std::ifstream In(LogPath, std::ios::binary);
        if (In)
            Log.assign((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
        return ExitCode;
    }

    // RunProcess in the exe's folder (so a tool resolves its own relative resources): true on exit 0.
    bool RunCaptured(const std::string& Exe, const std::wstring& Cmd, const std::string& LogPath, std::string& Log)
    {
        return RunProcess(Cmd, fs::path(Exe).parent_path().string(), LogPath, Log) == 0;
    }
}

namespace
{
    // Hint when that file exists, else the first <Name> on PATH; empty if neither.
    std::string FindTool(const std::string& Hint, const wchar_t* Name)
    {
        std::error_code Ec;
        if (!Hint.empty() && fs::exists(Hint, Ec))
            return Hint;
        wchar_t Found[MAX_PATH]{};
        const DWORD N = SearchPathW(nullptr, Name, nullptr, MAX_PATH, Found, nullptr);
        return N > 0 && N < MAX_PATH ? fs::path(Found).string() : std::string();
    }
}

std::filesystem::path ToolScratchPath(const char* Prefix)
{
    static std::atomic<unsigned> Next{ 0 };
    wchar_t Temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, Temp);
    return fs::path(Temp) / (std::string(Prefix) + "_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(Next++));
}

std::string ActsFindExe(const std::string& Hint)
{
    return FindTool(Hint, L"acts.exe");
}

bool ActsExtractFastFile(const std::string& ActsExe, const std::string& FastFilePath,
                         const std::string& OutputDir, const ActsExtractOptions& Opts, std::string& Log)
{
    std::error_code Ec;
    const std::string Exe = ActsFindExe(ActsExe);
    if (Exe.empty()) { Log = "acts not found"; return false; }

    fs::create_directories(OutputDir, Ec);
    const std::string LogPath = (fs::path(OutputDir) / "acts_fastfile.log").string();

    // acts [-t] fastfile -o <out> [options] <ff>
    CommandLine Cmd(Exe);

    Cmd.Add(L"-t");                 // hide the title banner
    Cmd.Add(L"fastfile");
    Cmd.Add(L"-d");                 // dump decompressed - without this acts writes nothing at all
    Cmd.Add(L"-o"); Cmd.AddQ(OutputDir);
    if (!Opts.Handler.empty())  { Cmd.Add(L"-r"); Cmd.Add(Strings::ToUnicodeString(Opts.Handler)); }
    if (!Opts.Assets.empty())   { Cmd.Add(L"-a"); Cmd.Add(Strings::ToUnicodeString(Opts.Assets)); }
    if (!Opts.GamePath.empty()) { Cmd.Add(L"-G"); Cmd.AddQ(Opts.GamePath); }
    if (Opts.DisableScriptDecomp) Cmd.Add(L"--disableScriptsDecomp");
    if (Opts.Graphic)             Cmd.Add(L"--graphic");

    // A "*_patch.ff" (BO3's own naming for a zone's patch fastfile) has no matching ".fd" in a
    // normal install - acts needs -p (use patch files) plus -i (ignore missing fd) to read one.
    const std::string StemLower = [&]
    {
        std::string S = fs::path(FastFilePath).stem().string();
        for (char& C : S) C = (char)tolower((unsigned char)C);
        return S;
    }();
    if (StemLower.size() >= 6 && StemLower.compare(StemLower.size() - 6, 6, "_patch") == 0)
    {
        Cmd.Add(L"-p");
        Cmd.Add(L"-i");
    }

    Cmd.AddQ(FastFilePath);

    return RunCaptured(Exe, Cmd.Text, LogPath, Log);
}

bool ActsDecompileGsc(const std::string& ActsExe, const std::string& ScriptPath,
                      const std::string& OutDir, std::string& OutSource, std::string& Log)
{
    std::error_code Ec;
    const std::string Exe = ActsFindExe(ActsExe);
    if (Exe.empty()) { Log = "acts not found"; return false; }

    fs::remove_all(OutDir, Ec);  // a stale file from a PREVIOUS decompile must not look like this one's
    fs::create_directories(OutDir, Ec);
    const std::string LogPath = (fs::path(OutDir) / "acts_gscd.log").string();

    // acts gscd -g -t pc -o <out> <script> - acts names its own output after the script's
    // internal compiled name (e.g. "scripts/cp/_accolades.gsc"), not ScriptPath's own filename,
    // so the result is found by scanning OutDir afterwards rather than predicted.
    CommandLine Cmd(Exe);

    Cmd.Add(L"-t");           // hide the title banner
    Cmd.Add(L"gscd");
    Cmd.Add(L"-g");            // produce GSC (not just ASM)
    Cmd.Add(L"-t"); Cmd.Add(L"pc");
    Cmd.Add(L"-o"); Cmd.AddQ(OutDir);
    Cmd.AddQ(ScriptPath);

    if (!RunCaptured(Exe, Cmd.Text, LogPath, Log))
        return false;

    // Whichever file acts wrote, wherever under OutDir it landed - if more than one somehow
    // exists (should not, given OutDir was just cleared), the most recently written wins.
    std::string BestPath;
    fs::file_time_type BestTime{};
    bool Any = false;
    for (const auto& Entry : fs::recursive_directory_iterator(OutDir, fs::directory_options::skip_permission_denied, Ec))
    {
        if (Ec) break;
        if (!Entry.is_regular_file(Ec)) continue;
        if (Entry.path().filename() == "acts_gscd.log") continue;

        const auto T = Entry.last_write_time(Ec);
        if (!Any || T > BestTime) { BestPath = Entry.path().string(); BestTime = T; Any = true; }
    }

    if (!Any)
    {
        if (Log.empty()) Log = "acts exited cleanly but wrote no output file";
        return false;
    }

    std::ifstream In(BestPath, std::ios::binary);
    if (!In) { Log = "could not read " + BestPath; return false; }
    OutSource.assign((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
    return true;
}

std::string GscToolFindExe(const std::string& Hint)
{
    return FindTool(Hint, L"gsc-tool.exe");
}

namespace
{
    // An exit code of a run that went through its inputs (a script failed: gsc-tool 1, acts -1),
    // not one that crashed, timed out or did not start.
    bool RanThrough(DWORD ExitCode) { return ExitCode == 0 || ExitCode == 1 || ExitCode == 0xFFFFFFFF; }
    constexpr DWORD DecompileTimeoutMs = 30 * 60 * 1000;
}

bool ActsDecompileDir(const std::string& ActsExe, const std::string& InDir, const std::string& OutDir, std::string& Log,
                      const std::string& DecryptExe)
{
    std::error_code Ec;
    const std::string Exe = ActsFindExe(ActsExe);
    if (Exe.empty()) { Log = "acts not found"; return false; }
    fs::create_directories(OutDir, Ec);
    const std::string LogPath = (fs::path(OutDir).parent_path() / "acts_gscd.log").string();

    // acts -t [--decrypt-mod <exe>] gscd -g -t pc --path-output -o <out> <in>: <in>/<rel>.gscc
    // decompiled to <out>/<rel>.gsc
    CommandLine Cmd(Exe);
    Cmd.Add(L"-t");
    if (!DecryptExe.empty()) { Cmd.Add(L"--decrypt-mod"); Cmd.AddQ(DecryptExe); }
    Cmd.Add(L"gscd");
    Cmd.Add(L"-g");
    Cmd.Add(L"-t"); Cmd.Add(L"pc");
    Cmd.Add(L"--path-output");
    Cmd.Add(L"-o"); Cmd.AddQ(OutDir);
    Cmd.AddQ(InDir);
    return RanThrough(RunProcess(Cmd.Text, fs::path(Exe).parent_path().string(), LogPath, Log, DecompileTimeoutMs));
}

bool ActsNoDecryption(const std::string& Log)
{
    // acts' decryptutils::LoadDecrypt: the dump does not load, or no decrypt.json signature matches once
    return Log.find("Can't load decryption") != std::string::npos;
}

bool GscToolDecompileDir(const std::string& GscToolExe, const std::string& Game, const std::string& System,
                         const std::string& InDir, const std::string& WorkDir, std::string& Log)
{
    const std::string Exe = GscToolFindExe(GscToolExe);
    if (Exe.empty()) { Log = "gsc-tool not found"; return false; }
    const std::string LogPath = (fs::path(WorkDir) / "gsc-tool.log").string();

    // gsc-tool -m decomp -g <game> -s <system> <in>: <in>/<rel>.gscbin decompiled to
    // <cwd>/decompiled/<game>/<rel>.gsc
    CommandLine Cmd(Exe);
    Cmd.Add(L"-m"); Cmd.Add(L"decomp");
    Cmd.Add(L"-g"); Cmd.Add(Strings::ToUnicodeString(Game));
    Cmd.Add(L"-s"); Cmd.Add(Strings::ToUnicodeString(System));
    Cmd.AddQ(InDir);
    return RanThrough(RunProcess(Cmd.Text, WorkDir, LogPath, Log, DecompileTimeoutMs));
}

std::string CodLuaJitFindExe(const std::string& Hint)
{
    const std::string Found = FindTool(Hint, L"cod-luajit-decompiler.exe");
    return Found.empty() ? FindTool(std::string(), L"cod-luajit-decompiler-windows-x86_64.exe") : Found;
}

std::string CodLuaFindExe(const std::string& Hint)
{
    return FindTool(Hint, L"CoDLuaDecompiler.CLI.exe");
}

namespace
{
    size_t LuaInputs(const std::string& InDir)
    {
        std::error_code Ec;
        size_t N = 0;
        for (fs::directory_iterator It(InDir, Ec), End; !Ec && It != End; It.increment(Ec))
            if (It->path().extension() == ".luac") N++;
        return N;
    }

    size_t Count(const std::string& Log, const char* What)
    {
        size_t N = 0;
        for (size_t At = Log.find(What); At != std::string::npos; At = Log.find(What, At + 1)) N++;
        return N;
    }
}

bool CodLuaJitDecompileDir(const std::string& Exe, const std::vector<std::string>& Args, const std::string& InDir,
                           const std::string& OutDir, std::string& Log)
{
    const std::string Found = CodLuaJitFindExe(Exe);
    if (Found.empty()) { Log = "cod-luajit-decompiler not found"; return false; }
    std::error_code Ec;
    fs::create_directories(OutDir, Ec);
    const std::string LogPath = (fs::path(OutDir).parent_path() / "cod-luajit-decompiler.log").string();

    // cod-luajit-decompiler -f -e .luac [Args] -o <out> <in>: <in>/<file>.luac decompiled to
    // <out>/<file>.lua; a file it does not decompile has none ("Failed to decompile N files.", exit 1)
    CommandLine Cmd(Found);
    Cmd.Add(L"-f");
    Cmd.Add(L"-e"); Cmd.Add(L".luac");
    for (const std::string& A : Args) Cmd.AddQ(A);
    Cmd.Add(L"-o"); Cmd.AddQ(OutDir);
    Cmd.AddQ(InDir);
    const DWORD Exit = RunProcess(Cmd.Text, fs::path(Found).parent_path().string(), LogPath, Log, DecompileTimeoutMs);
    return (Exit == 0 || Exit == 1) && Log.find("Done! Decompiled") != std::string::npos;
}

bool CodLuaDecompileDir(const std::string& Exe, const std::string& InDir, const std::string& OutDir, std::string& Log)
{
    const std::string Found = CodLuaFindExe(Exe);
    if (Found.empty()) { Log = "CoDLuaDecompiler not found"; return false; }
    std::error_code Ec;
    fs::create_directories(OutDir, Ec);
    const std::string LogPath = (fs::path(OutDir).parent_path() / "codluadecompiler.log").string();

    // CoDLuaDecompiler.CLI <in> -o <out>: <in>/<file>.luac decompiled to <out>/<file>.dec.lua, one
    // line per file ("Decompiled file: <path>" or "Error while decompiling file: <path>"). It asks
    // GitHub for a newer release at every start and waits for Enter at its end: the request goes
    // to a proxy that is not there (refused at once, no network; the run then ends on that
    // exception, its exit code not 0), stdin is none. A framework-dependent build of it runs on a
    // newer .NET than its 6 (DOTNET_ROLL_FORWARD). It writes numbers in the user's culture (0,5 under a
    // French Windows): the invariant culture keeps them Lua (0.5).
    CommandLine Cmd(Found);
    Cmd.AddQ(InDir);
    Cmd.Add(L"-o"); Cmd.AddQ(OutDir);
    RunProcess(Cmd.Text, fs::path(Found).parent_path().string(), LogPath, Log, DecompileTimeoutMs,
               { { L"HTTPS_PROXY", L"http://127.0.0.1:9" }, { L"HTTP_PROXY", L"http://127.0.0.1:9" }, { L"ALL_PROXY", L"" },
                 { L"NO_PROXY", L"" }, { L"DOTNET_ROLL_FORWARD", L"Major" },
                 { L"DOTNET_SYSTEM_GLOBALIZATION_INVARIANT", L"1" } });
    return Count(Log, "Decompiled file: ") + Count(Log, "Error while decompiling file: ") >= LuaInputs(InDir);
}

std::string FfmpegFindExe(const std::string& Hint)
{
    std::error_code Ec;
    if (!Hint.empty() && fs::exists(Hint, Ec)) return Hint;
    const fs::path Beside = fs::path(FileSystems::GetApplicationPath()) / "ffmpeg.exe";
    if (fs::exists(Beside, Ec)) return Beside.string();
    if (const std::string OnPath = FindTool(std::string(), L"ffmpeg.exe"); !OnPath.empty()) return OnPath;
    const auto Env = [](const wchar_t* Name, const fs::path& Else) {
        wchar_t Value[MAX_PATH]{};
        const DWORD N = GetEnvironmentVariableW(Name, Value, MAX_PATH);
        return N > 0 && N < MAX_PATH ? fs::path(Value) : Else;
    };
    const fs::path Local = Env(L"LOCALAPPDATA", fs::path()), Programs = Env(L"ProgramFiles", "C:/Program Files");
    const fs::path Data = Env(L"ProgramData", "C:/ProgramData"), Home = Env(L"USERPROFILE", fs::path());
    std::vector<fs::path> Places;
    // WinGet (per user, then machine-wide): its Links folder, then a Gyan.FFmpeg package's
    // <version>/bin, the most recent first
    for (const fs::path& WinGet : { Local.empty() ? fs::path() : Local / "Microsoft/WinGet", Programs / "WinGet" })
    {
        if (WinGet.empty()) continue;
        Places.push_back(WinGet / "Links/ffmpeg.exe");
        std::vector<std::pair<fs::file_time_type, fs::path>> Found;
        for (const auto& Package : fs::directory_iterator(WinGet / "Packages", Ec))
            if (Package.is_directory(Ec) && Package.path().filename().string().rfind("Gyan.FFmpeg", 0) == 0)
                for (const auto& Build : fs::directory_iterator(Package.path(), Ec))
                    if (const fs::path Exe = Build.path() / "bin/ffmpeg.exe"; fs::exists(Exe, Ec)) Found.emplace_back(fs::last_write_time(Exe, Ec), Exe);
        std::sort(Found.begin(), Found.end(), [](const auto& A, const auto& B) { return A.first > B.first; });
        for (const auto& [Time, Exe] : Found) Places.push_back(Exe);
    }
    // Chocolatey; Scoop (per user, then global: the app's current version, then its shim)
    Places.push_back(Env(L"ChocolateyInstall", Data / "chocolatey") / "bin/ffmpeg.exe");
    for (const fs::path& Scoop : { Env(L"SCOOP", Home.empty() ? fs::path() : Home / "scoop"), Env(L"SCOOP_GLOBAL", Data / "scoop") })
        if (!Scoop.empty()) Places.push_back(Scoop / "apps/ffmpeg/current/bin/ffmpeg.exe"), Places.push_back(Scoop / "shims/ffmpeg.exe");
    for (const fs::path& P : Places)
        if (!P.empty() && fs::exists(P, Ec)) return P.lexically_normal().string();
    return std::string();
}

bool FfmpegDecodeToWav(const std::string& Exe, const std::string& InPath, const std::string& OutPath, std::string& Log)
{
    if (Exe.empty()) { Log = "ffmpeg not found"; return false; }
    // ffmpeg -nostdin -loglevel error -y -i <in> -map 0:a:0 -c:a pcm_s16le -bitexact -map_metadata -1 -f wav <out>:
    // -bitexact and no metadata keep the WAV to its fmt and data chunks (no LIST "encoder" chunk)
    CommandLine Cmd(Exe);
    Cmd.Add(L"-hide_banner -nostdin -loglevel error -y -i"); Cmd.AddQ(InPath);
    Cmd.Add(L"-map 0:a:0 -c:a pcm_s16le -bitexact -map_metadata -1 -f wav"); Cmd.AddQ(OutPath);
    const DWORD Exit = RunProcess(Cmd.Text, fs::path(OutPath).parent_path().string(), OutPath + ".log", Log, 5 * 60 * 1000);
    std::error_code Ec;
    fs::remove(OutPath + ".log", Ec);
    return Exit == 0 && fs::file_size(OutPath, Ec) > 44 && !Ec;
}
