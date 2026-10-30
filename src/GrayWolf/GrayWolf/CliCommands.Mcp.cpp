// NOMINMAX before <windows.h>: std::max/std::min below would be mangled by its macros.
#define NOMINMAX
#include <Windows.h>

#include "CliCommands.h"
#include "CliOptions.h"
#include "McpServer.h"
#include "SettingsManager.h"
#include "Updater.h"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <fstream>

#include <map>
#include <memory>
#include <random>
#include <regex>
#include <sstream>
#include <thread>

// `GrayWolf.exe mcp`: GrayWolf's tools for an AI agent, over MCP (McpServer.h). Each tool runs this
// same exe as a child with --report, so a crash or a stuck export never takes the server down,
// and cancelling = terminating that child (only children this server started: a job object).

// Guards: an install only as list-installs reports it, a zone only one of its zones, writes only
// under the output root, one export at a time, never update / apply-update / open-map / a game.

namespace fs = std::filesystem;
using Mcp::Json;
using Mcp::ToolError;

namespace
{
    std::wstring Wide(const std::string& S)
    {
        if (S.empty()) return {};
        const int N = MultiByteToWideChar(CP_UTF8, 0, S.data(), (int)S.size(), nullptr, 0);
        std::wstring Out(N, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, S.data(), (int)S.size(), Out.data(), N);
        return Out;
    }

    std::string Utf8(const std::wstring& S)
    {
        if (S.empty()) return {};
        const int N = WideCharToMultiByte(CP_UTF8, 0, S.data(), (int)S.size(), nullptr, 0, nullptr, nullptr);
        std::string Out(N, '\0');
        WideCharToMultiByte(CP_UTF8, 0, S.data(), (int)S.size(), Out.data(), N, nullptr, nullptr);
        return Out;
    }

    std::string Utf8(const fs::path& P) { return Utf8(P.wstring()); }

    std::string Lower(std::string S)
    {
        std::transform(S.begin(), S.end(), S.begin(), [](unsigned char C) { return (char)std::tolower(C); });
        return S;
    }

    // Win32's own full-path rules (trailing dots and spaces dropped, .. folded): what a write lands on.
    fs::path FullPath(const fs::path& P)
    {
        std::vector<wchar_t> Buf(32768);
        const DWORD N = GetFullPathNameW(P.c_str(), (DWORD)Buf.size(), Buf.data(), nullptr);
        return N > 0 && N < Buf.size() ? fs::path(std::wstring(Buf.data(), N)) : P;
    }

    // os.path.normcase(normpath(abspath(p))) of the prototype
    std::wstring Norm(const std::string& P)
    {
        std::wstring S = FullPath(fs::path(Wide(P)).make_preferred()).wstring();
        while (S.size() > 3 && (S.back() == L'\\' || S.back() == L'/')) S.pop_back();
        CharLowerBuffW(S.data(), (DWORD)S.size());
        return S;
    }

    // Windows' command-line quoting (CommandLineToArgvW's rules).
    std::wstring Quote(const std::wstring& Arg)
    {
        if (!Arg.empty() && Arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return Arg;
        std::wstring Out = L"\"";
        for (size_t I = 0;; I++)
        {
            size_t Slashes = 0;
            while (I < Arg.size() && Arg[I] == L'\\') { I++; Slashes++; }
            if (I == Arg.size()) { Out.append(Slashes * 2, L'\\'); break; }
            if (Arg[I] == L'"') Out.append(Slashes * 2 + 1, L'\\');
            else Out.append(Slashes, L'\\');
            Out += Arg[I];
        }
        return Out + L"\"";
    }

    // ---- the server's state

    fs::path g_Exe;           // this exe: every tool runs it
    fs::path g_OutRoot;       // every export writes under it
    HANDLE g_Job = nullptr;   // the children; closing it (or this process dying) kills them

    const std::vector<std::string> kKinds = { "model", "anim", "image", "sound", "rawfile", "data", "localize", "map", "script", "material" };
    const std::vector<std::string> kModelFormats = { "cast", "semodel", "obj", "ma", "xna", "xmodel_export", "xmodel_bin", "smd", "gltf", "glb" };
    const std::vector<std::string> kAnimFormats = { "cast", "seanim", "xanim_export" };

    // A --report file: key=value lines, a key met twice holds them all; other lines under Lines.
    struct Report
    {
        std::map<std::string, std::vector<std::string>> Values;
        std::vector<std::string> Lines;

        std::string Get(const std::string& Key, const std::string& Default = "") const
        {
            auto It = Values.find(Key);
            return It == Values.end() || It->second.empty() ? Default : It->second.front();
        }
        std::vector<std::string> All(const std::string& Key) const
        {
            auto It = Values.find(Key);
            return It == Values.end() ? std::vector<std::string>{} : It->second;
        }
        // a value given once as a string, several times as a list (the prototype's shape)
        Json AsJson(const std::string& Key) const
        {
            const auto V = All(Key);
            return V.size() == 1 ? Json(V.front()) : Json(V);
        }
    };

    Report ParseReport(const std::string& Text)
    {
        Report R;
        std::istringstream In(Text);
        std::string Line;
        while (std::getline(In, Line))
        {
            if (!Line.empty() && Line.back() == '\r') Line.pop_back();
            if (Line.empty() || Line[0] == '#') continue;
            const size_t Eq = Line.find('=');
            if (Eq == std::string::npos || Line.substr(0, Eq).find(' ') != std::string::npos)
            {
                R.Lines.push_back(Line);
                continue;
            }
            R.Values[Line.substr(0, Eq)].push_back(Line.substr(Eq + 1));
        }
        return R;
    }

    long long ToInt(const std::string& S)
    {
        try { return S.empty() ? 0 : std::stoll(S); }
        catch (...) { return 0; }
    }

    // ---- running the exe

    // GrayWolf.exe Args --report <tmp>, the report parsed. Stdio on NUL (the protocol's pipes are
    // never inherited), in the job, killed on cancel or past Timeout. Throws ToolError on failure.
    Report RunChild(const std::vector<std::string>& Args, int TimeoutSec, Mcp::CancelToken& Cancel)
    {
        std::error_code Ec;
        if (!fs::is_regular_file(g_Exe, Ec)) throw ToolError("GrayWolf.exe not found at " + Utf8(g_Exe));
        if (Cancel.Fired()) throw Mcp::Cancelled{};
        wchar_t TempDir[MAX_PATH + 1] = {}, Temp[MAX_PATH + 1] = {};
        GetTempPathW(MAX_PATH, TempDir);
        if (!GetTempFileNameW(TempDir, L"gwm", 0, Temp)) throw ToolError("cannot create a temporary report file");
        const std::wstring ReportPath = Temp;

        std::wstring Cmd = Quote(g_Exe.wstring());
        for (const auto& A : Args) Cmd += L" " + Quote(Wide(A));
        Cmd += L" --report " + Quote(ReportPath);

        SECURITY_ATTRIBUTES Sa{ sizeof(Sa), nullptr, TRUE };
        HANDLE Nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &Sa, OPEN_EXISTING, 0, nullptr);
        SIZE_T AttrSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &AttrSize);
        std::vector<uint8_t> AttrBuf(AttrSize);
        auto* Attr = (LPPROC_THREAD_ATTRIBUTE_LIST)AttrBuf.data();
        InitializeProcThreadAttributeList(Attr, 1, 0, &AttrSize);
        UpdateProcThreadAttribute(Attr, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &Nul, sizeof(Nul), nullptr, nullptr);
        STARTUPINFOEXW Si{};
        Si.StartupInfo.cb = sizeof(Si);
        Si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        Si.StartupInfo.hStdInput = Si.StartupInfo.hStdOutput = Si.StartupInfo.hStdError = Nul;
        Si.lpAttributeList = Attr;
        PROCESS_INFORMATION Pi{};
        const BOOL Started = CreateProcessW(g_Exe.c_str(), Cmd.data(), nullptr, nullptr, TRUE,
                                            CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                                            nullptr, nullptr, &Si.StartupInfo, &Pi);
        const DWORD StartError = GetLastError();
        DeleteProcThreadAttributeList(Attr);
        CloseHandle(Nul);
        if (!Started)
        {
            DeleteFileW(ReportPath.c_str());
            throw ToolError(std::format("cannot start GrayWolf.exe ({})", StartError));
        }
        AssignProcessToJobObject(g_Job, Pi.hProcess);
        ResumeThread(Pi.hThread);
        CloseHandle(Pi.hThread);
        HANDLE Proc = Pi.hProcess;
        Cancel.OnFire([Proc] { TerminateProcess(Proc, 1); });

        const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(TimeoutSec);
        bool TimedOut = false;
        while (WaitForSingleObject(Proc, 250) == WAIT_TIMEOUT)
        {
            if (std::chrono::steady_clock::now() > Deadline)
            {
                TerminateProcess(Proc, 1);
                WaitForSingleObject(Proc, 10000);
                TimedOut = true;
                break;
            }
        }
        Cancel.Clear();
        DWORD Code = 0;
        GetExitCodeProcess(Proc, &Code);
        CloseHandle(Proc);

        std::string Text;
        {
            std::ifstream F(fs::path(ReportPath), std::ios::binary);
            Text.assign(std::istreambuf_iterator<char>(F), std::istreambuf_iterator<char>());
        }
        DeleteFileW(ReportPath.c_str());
        if (Cancel.Fired()) throw Mcp::Cancelled{};
        if (TimedOut)
            throw ToolError(std::format("GrayWolf did not finish within {} s: {}{}", TimeoutSec, Args[0], Args.size() > 1 ? " " + Args[1] : ""));
        const Report R = ParseReport(Text);
        if (R.Get("result") != "ok")
        {
            const std::string Why = !R.Get("reason").empty() ? R.Get("reason")
                                  : Text.empty() ? std::format("exit code {}, no report", (int)Code)
                                                 : "result=" + R.Get("result", "None");
            throw ToolError(std::format("GrayWolf {} failed: {}", Args[0], Why));
        }
        return R;
    }

    // ---- the installs, the only places zones are read from

    struct Install
    {
        std::string Title, Source, Game, Language, Text, Installed, Path;
        int Year = 0;
        Json ToJson() const
        {
            return { { "title", Title }, { "year", Year }, { "source", Source }, { "game", Game }, { "language", Language },
                     { "text", Text }, { "installed", Installed }, { "path", Path } };
        }
    };

    std::mutex g_CacheLock;
    std::optional<std::vector<Install>> g_Installs;
    std::map<std::wstring, std::vector<std::pair<std::string, std::string>>> g_Zones;   // (zone, level) per install

    std::vector<Install> Installs(Mcp::CancelToken& Cancel)
    {
        {
            std::lock_guard<std::mutex> G(g_CacheLock);
            if (g_Installs) return *g_Installs;
        }
        const Report R = RunChild({ "list-installs" }, 120, Cancel);
        static const std::regex Line(R"(^(.*?) year=(\d+) source=(\S+) disk=(\S+) language=(\S+) text=(\S+) installed=(\S+) path=(.*)$)");
        std::vector<Install> Found;
        for (const auto& L : R.All("install"))
        {
            std::smatch M;
            if (!std::regex_match(L, M, Line)) continue;
            Install I;
            I.Title = M[1]; I.Year = (int)ToInt(M[2]); I.Source = M[3]; I.Game = M[4];
            I.Language = M[5]; I.Text = M[6]; I.Installed = M[7]; I.Path = M[8];
            Found.push_back(std::move(I));
        }
        std::lock_guard<std::mutex> G(g_CacheLock);
        g_Installs = Found;
        return Found;
    }

    std::string OptString(const Json& Args, const char* Key)
    {
        return Args.contains(Key) && Args[Key].is_string() ? Args[Key].get<std::string>() : std::string();
    }

    std::vector<std::string> OptList(const Json& Args, const char* Key)
    {
        std::vector<std::string> Out;
        if (Args.contains(Key) && Args[Key].is_array())
            for (const auto& V : Args[Key]) Out.push_back(V.get<std::string>());
        return Out;
    }

    long long OptInt(const Json& Args, const char* Key, long long Default)
    {
        return Args.contains(Key) && Args[Key].is_number() ? (long long)Args[Key].get<double>() : Default;
    }

    bool OptBool(const Json& Args, const char* Key, bool Default)
    {
        return Args.contains(Key) && Args[Key].is_boolean() ? Args[Key].get<bool>() : Default;
    }

    // The install at path, as list-installs reports it; game, when given, must match it.
    Install FindInstall(const Json& Args, Mcp::CancelToken& Cancel)
    {
        const std::string Path = OptString(Args, "install"), Game = OptString(Args, "game");
        const std::wstring Want = Norm(Path);
        for (const auto& I : Installs(Cancel))
            if (Norm(I.Path) == Want && (Game.empty() || Game == I.Game)) return I;
        throw ToolError(Path + " is not an install graywolf_list_installs reports" + (Game.empty() ? "" : " for game " + Game) +
                        ": use one of its paths");
    }

    std::vector<std::pair<std::string, std::string>> ZonesOf(const Install& I, Mcp::CancelToken& Cancel)
    {
        const std::wstring Key = Norm(I.Path) + L"|" + Wide(I.Game);
        {
            std::lock_guard<std::mutex> G(g_CacheLock);
            auto It = g_Zones.find(Key);
            if (It != g_Zones.end()) return It->second;
        }
        const Report R = RunChild({ "level-names", I.Path, "--game", I.Game }, 600, Cancel);
        std::vector<std::pair<std::string, std::string>> Zones;
        for (const auto& L : R.All("zone"))
        {
            const size_t Tab = L.find('\t');
            Zones.emplace_back(L.substr(0, Tab), Tab == std::string::npos ? "" : L.substr(Tab + 1));
        }
        std::lock_guard<std::mutex> G(g_CacheLock);
        g_Zones[Key] = Zones;
        return Zones;
    }

    std::string BaseName(std::string Zone)
    {
        std::replace(Zone.begin(), Zone.end(), '\\', '/');
        const size_t Slash = Zone.rfind('/');
        return Lower(Slash == std::string::npos ? Zone : Zone.substr(Slash + 1));
    }

    // Each zone as GrayWolf lists it: given so, or by its file name without folder or extension
    // (the base game's copy before a DLC pack's, then the shortest path).
    std::vector<std::string> ResolveZones(const Install& I, const std::vector<std::string>& Wanted, Mcp::CancelToken& Cancel)
    {
        const auto Listed = ZonesOf(I, Cancel);
        std::vector<std::string> Out;
        for (const auto& Want : Wanted)
        {
            const std::string LowWant = Lower(Want);
            auto Exact = std::find_if(Listed.begin(), Listed.end(), [&](const auto& Z) { return Lower(Z.first) == LowWant; });
            if (Exact != Listed.end()) { Out.push_back(Exact->first); continue; }
            std::string Stem = BaseName(Want);
            if (Stem.ends_with(".ff")) Stem.resize(Stem.size() - 3);
            std::vector<std::string> Hits;
            for (const auto& [Zone, Level] : Listed)
            {
                const std::string B = BaseName(Zone);
                if (B == Stem || B == Stem + ".ff") Hits.push_back(Zone);
            }
            if (Hits.empty()) throw ToolError("zone " + Want + " is not one of the install's (graywolf_list_zones lists them)");
            auto Depth = [](const std::string& Z) { return std::count(Z.begin(), Z.end(), '/') + std::count(Z.begin(), Z.end(), '\\'); };
            std::stable_sort(Hits.begin(), Hits.end(), [&](const std::string& A, const std::string& B) {
                return std::make_pair(Depth(A), A.size()) < std::make_pair(Depth(B), B.size());
            });
            Out.push_back(Hits.front());
        }
        return Out;
    }

    // g_OutRoot\Name, refusing anything that leaves it.
    fs::path OutDir(const std::string& Name)
    {
        static const std::regex Ok(R"([A-Za-z0-9_.\-]+)");
        if (Name.empty() || !std::regex_match(Name, Ok) || Name == "." || Name == "..")
            throw ToolError("out_name: letters, digits, '_', '-', '.' only");
        const fs::path D = FullPath(g_OutRoot / Wide(Name));
        const std::wstring Root = g_OutRoot.wstring() + L"\\";
        if (_wcsnicmp(D.c_str(), Root.c_str(), Root.size()) != 0 || D.wstring().size() <= Root.size())
            throw ToolError("out_name leaves the output root");
        return D;
    }

    // Values joined by commas for the CLI, each checked against Allowed when given.
    std::string Csv(const std::vector<std::string>& Items, const std::vector<std::string>* Allowed, const std::string& What)
    {
        std::string Out;
        for (const auto& X : Items)
        {
            if (Allowed && std::find(Allowed->begin(), Allowed->end(), X) == Allowed->end())
            {
                std::string List;
                for (const auto& A : *Allowed) List += (List.empty() ? "" : ", ") + A;
                throw ToolError(What + ": " + X + " is not one of " + List);
            }
            if (X.find(',') != std::string::npos) throw ToolError(What + ": one value per item");
            Out += (Out.empty() ? "" : ",") + X;
        }
        return Out;
    }

    Json Page(const std::vector<Json>& Items, long long Offset, long long Limit)
    {
        Limit = std::clamp<long long>(Limit, 1, 500);
        Offset = std::max<long long>(0, Offset);
        Json Part = Json::array();
        for (long long I = Offset; I < (long long)Items.size() && I < Offset + Limit; I++) Part.push_back(Items[(size_t)I]);
        const long long Next = Offset + (long long)Part.size();
        return { { "total", Items.size() }, { "offset", Offset }, { "items", Part },
                 { "next_offset", Next < (long long)Items.size() ? Json(Next) : Json() } };
    }

    // ---- exports: one at a time (the preview's too), followed by a job id

    // A lock released by whichever thread holds the export, not only the one that took it.
    struct ExportSlot
    {
        std::mutex M;
        std::condition_variable Cv;
        bool Held = false;
        bool TryTake()
        {
            std::lock_guard<std::mutex> G(M);
            if (Held) return false;
            Held = true;
            return true;
        }
        void Take(Mcp::CancelToken& Cancel)
        {
            std::unique_lock<std::mutex> G(M);
            while (Held)
            {
                Cv.wait_for(G, std::chrono::milliseconds(200));
                if (Cancel.Fired()) throw Mcp::Cancelled{};
            }
            Held = true;
        }
        void Release()
        {
            {
                std::lock_guard<std::mutex> G(M);
                Held = false;
            }
            Cv.notify_all();
        }
    } g_Slot;

    struct Job
    {
        std::string Id;
        std::vector<std::string> Args;
        fs::path Out;
        std::vector<std::string> Zones;
        std::chrono::steady_clock::time_point Started = std::chrono::steady_clock::now();
        Mcp::CancelToken Cancel;

        std::mutex M;
        std::condition_variable Done;
        std::string State = "running";   // running, done, failed, cancelled
        size_t ZoneIndex = 0;
        std::vector<Report> Reports;
        std::string Error;

        bool WaitDone(std::chrono::milliseconds For)
        {
            std::unique_lock<std::mutex> G(M);
            return Done.wait_for(G, For, [this] { return State != "running"; });
        }
    };

    std::mutex g_JobsLock;
    std::map<std::string, std::shared_ptr<Job>> g_Jobs;
    std::atomic<int> g_JobThreads{ 0 };

    std::pair<long long, long long> FilesUnder(const fs::path& D, long long Cap = 200000)
    {
        long long N = 0, Size = 0;
        std::error_code Ec;
        for (auto It = fs::recursive_directory_iterator(D, fs::directory_options::skip_permission_denied, Ec);
             !Ec && It != fs::recursive_directory_iterator(); It.increment(Ec))
        {
            std::error_code E2;
            if (!It->is_regular_file(E2)) continue;
            N++;
            const auto S = It->file_size(E2);
            if (!E2) Size += (long long)S;
            if (N >= Cap) break;
        }
        return { N, Size };
    }

    Json Summary(Job& J, size_t FilesListed = 50)
    {
        std::string State, Error;
        size_t ZoneIndex;
        std::vector<Report> Reports;
        {
            std::lock_guard<std::mutex> G(J.M);
            State = J.State;
            Error = J.Error;
            ZoneIndex = J.ZoneIndex;
            Reports = J.Reports;
        }
        std::error_code Ec;
        const auto [Files, Bytes] = fs::exists(J.Out, Ec) ? FilesUnder(J.Out) : std::pair<long long, long long>{ 0, 0 };
        const double Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - J.Started).count();
        Json S = { { "job", J.Id }, { "state", State }, { "out", Utf8(J.Out) }, { "zones", J.Zones },
                   { "zone_now", J.Zones.empty() ? Json() : Json(J.Zones[std::min(ZoneIndex, J.Zones.size() - 1)]) },
                   { "seconds", std::round(Seconds * 10) / 10 }, { "files", Files }, { "bytes", Bytes } };
        if (!Error.empty()) S["error"] = Error;
        if (!Reports.empty())
        {
            Json Written = Json::object();
            std::vector<std::string> Failures, Unresolved, ZoneErrors;
            for (const auto& R : Reports)
            {
                for (const auto& L : R.Lines)
                {
                    if (L.starts_with("zone_error ")) ZoneErrors.push_back(L.substr(11));
                    else if (L.starts_with("unresolved ")) Unresolved.push_back(L.substr(11));
                    else if (L.starts_with("failure ")) Failures.push_back(L.substr(8));
                }
                for (const auto& [K, V] : R.Values)
                    if (K.ends_with("_written") && V.size() == 1 && !V[0].empty() &&
                        std::all_of(V[0].begin(), V[0].end(), [](unsigned char C) { return std::isdigit(C); }))
                    {
                        const std::string Kind = K.substr(0, K.size() - 8);
                        Written[Kind] = Written.value(Kind, 0LL) + ToInt(V[0]);
                    }
            }
            S["written"] = Written;
            if (!ZoneErrors.empty()) S["zones_not_read"] = ZoneErrors;
            if (Failures.size() > 50) Failures.resize(50);
            if (Unresolved.size() > 50) Unresolved.resize(50);
            S["failures"] = Failures;
            S["unresolved_dependencies"] = Unresolved;
        }
        if (State == "done" && fs::exists(J.Out, Ec))
        {
            Json Listed = Json::array();
            for (auto It = fs::recursive_directory_iterator(J.Out, fs::directory_options::skip_permission_denied, Ec);
                 !Ec && It != fs::recursive_directory_iterator() && Listed.size() < FilesListed; It.increment(Ec))
            {
                std::error_code E2;
                if (!It->is_regular_file(E2) || It->path().filename() == L"export.log") continue;
                Listed.push_back(Utf8(It->path().lexically_relative(J.Out)));
            }
            S["some_files"] = Listed;
        }
        return S;
    }

    // The job's thread: one disk-export per zone, holding the export slot throughout.
    void RunJob(std::shared_ptr<Job> J)
    {
        std::string State = "done", Error;
        try
        {
            for (size_t I = 0; I < J->Zones.size(); I++)
            {
                {
                    std::lock_guard<std::mutex> G(J->M);
                    J->ZoneIndex = I;
                }
                std::vector<std::string> Args = { J->Args[0], J->Zones[I] };
                Args.insert(Args.end(), J->Args.begin() + 1, J->Args.end());
                Report R = RunChild(Args, 6 * 3600, J->Cancel);
                std::lock_guard<std::mutex> G(J->M);
                J->Reports.push_back(std::move(R));
            }
        }
        catch (const Mcp::Cancelled&) { State = "cancelled"; }
        catch (const ToolError& E) { State = "failed"; Error = E.what(); }
        catch (const std::exception& E) { State = "failed"; Error = std::string("internal error: ") + E.what(); }
        g_Slot.Release();
        {
            std::lock_guard<std::mutex> G(J->M);
            J->State = State;
            J->Error = Error;
        }
        J->Done.notify_all();
        Mcp::Log(std::format("export job {} {}", J->Id, State));
        g_JobThreads--;
    }

    std::shared_ptr<Job> FindJob(const Json& Args)
    {
        const std::string Id = OptString(Args, "job");
        std::lock_guard<std::mutex> G(g_JobsLock);
        auto It = g_Jobs.find(Id);
        if (It == g_Jobs.end()) throw ToolError("no export job " + Id);
        return It->second;
    }

    std::string NewId()
    {
        static std::mt19937_64 Rng{ std::random_device{}() };
        static std::mutex M;
        std::lock_guard<std::mutex> G(M);
        return std::format("{:08x}", (uint32_t)Rng());
    }

    // ---- the tools

    Mcp::ToolResult ListInstalls(const Json&, Mcp::Call& C)
    {
        Json List = Json::array();
        for (const auto& I : Installs(C.Cancel)) List.push_back(I.ToJson());
        return Mcp::ToolResult::Object({ { "installs", List }, { "output_root", Utf8(g_OutRoot) } });
    }

    Mcp::ToolResult ListZones(const Json& Args, Mcp::Call& C)
    {
        const Install I = FindInstall(Args, C.Cancel);
        const std::string Contains = Lower(OptString(Args, "contains"));
        const bool LevelsOnly = OptBool(Args, "levels_only", false);
        std::vector<Json> Rows;
        for (const auto& [Zone, Level] : ZonesOf(I, C.Cancel))
        {
            if (LevelsOnly && Level.empty()) continue;
            if (!Contains.empty() && Lower(Zone).find(Contains) == std::string::npos && Lower(Level).find(Contains) == std::string::npos) continue;
            Rows.push_back({ { "zone", Zone }, { "level", Level.empty() ? Json() : Json(Level) } });
        }
        Json Out = Page(Rows, OptInt(Args, "offset", 0), OptInt(Args, "limit", 100));
        Out["game"] = I.Game;
        return Mcp::ToolResult::Object(Out);
    }

    Mcp::ToolResult FindAssets(const Json& Args, Mcp::Call& C)
    {
        const Install I = FindInstall(Args, C.Cancel);
        const auto Names = OptList(Args, "names"), Contains = OptList(Args, "contains"), Kinds = OptList(Args, "kinds");
        if (Names.empty() && Contains.empty()) throw ToolError("give names or contains");
        std::vector<std::string> Cmd = { "disk-where", I.Path, "--game", I.Game };
        if (!Names.empty()) Cmd.insert(Cmd.end(), { "--names", Csv(Names, nullptr, "names") });
        if (!Contains.empty()) Cmd.insert(Cmd.end(), { "--name-filter", Csv(Contains, nullptr, "contains") });
        if (!Kinds.empty()) Cmd.insert(Cmd.end(), { "--type", Csv(Kinds, &kKinds, "kinds") });
        const Report R = RunChild(Cmd, 3600, C.Cancel);
        std::vector<Json> Rows;
        for (const auto& L : R.All("asset"))
        {
            std::vector<std::string> F;
            std::stringstream SS(L);
            std::string Part;
            while (std::getline(SS, Part, '\t')) F.push_back(Part);
            F.resize(4);
            Rows.push_back({ { "kind", F[0] }, { "name", F[1] }, { "zone", F[2] }, { "zone_bytes", ToInt(F[3]) } });
        }
        Json Out = Page(Rows, OptInt(Args, "offset", 0), OptInt(Args, "limit", 100));
        Out["game"] = I.Game;
        Out["assets"] = ToInt(R.Get("assets", "0"));
        Out["missing"] = R.All("missing");
        return Mcp::ToolResult::Object(Out);
    }

    Mcp::ToolResult Estimate(const Json& Args, Mcp::Call& C)
    {
        const Install I = FindInstall(Args, C.Cancel);
        std::vector<std::string> Cmd = { "disk-estimate", I.Path, "--game", I.Game, "--picked", Csv(OptList(Args, "zones"), nullptr, "zones") };
        if (const auto Kinds = OptList(Args, "kinds"); !Kinds.empty()) Cmd.insert(Cmd.end(), { "--type", Csv(Kinds, &kKinds, "kinds") });
        const Report R = RunChild(Cmd, 1800, C.Cancel);
        Json PerKind = Json::object();
        for (const auto& [K, V] : R.Values)
            if (K.starts_with("kind_")) PerKind[K.substr(5)] = R.AsJson(K);
        return Mcp::ToolResult::Object({ { "game", I.Game }, { "bytes", ToInt(R.Get("bytes", "0")) },
                                         { "text", R.Values.contains("text") ? R.AsJson("text") : Json() }, { "kinds", PerKind } });
    }

    Mcp::ToolResult Export(const Json& Args, Mcp::Call& C)
    {
        const Install I = FindInstall(Args, C.Cancel);
        auto Zones = OptList(Args, "zones");
        if (Zones.empty()) throw ToolError("give at least one zone");
        const fs::path Out = OutDir(OptString(Args, "out_name"));
        Zones = ResolveZones(I, Zones, C.Cancel);
        std::vector<std::string> Cmd = { "disk-export", "--install", I.Path, "--game", I.Game, "--out", Utf8(Out) };
        const auto Assets = OptList(Args, "assets");
        std::vector<std::string> AssetKinds;
        if (!Assets.empty())
        {
            std::string Joined;
            for (const auto& A : Assets)
            {
                const size_t Colon = A.find(':');
                const std::string Kind = Colon == std::string::npos ? "" : A.substr(0, Colon);
                if (Colon == std::string::npos || std::find(kKinds.begin(), kKinds.end(), Kind) == kKinds.end() || Colon + 1 == A.size())
                    throw ToolError("assets: kind:name, not " + A);
                if (std::find(AssetKinds.begin(), AssetKinds.end(), Kind) == AssetKinds.end()) AssetKinds.push_back(Kind);
                Joined += (Joined.empty() ? "" : ",") + A;
            }
            Cmd.insert(Cmd.end(), { "--assets", Joined });
            std::sort(AssetKinds.begin(), AssetKinds.end());
        }
        std::string Types = Csv(OptList(Args, "kinds"), &kKinds, "kinds");
        if (Types.empty()) Types = Assets.empty() ? "model,anim,image,sound" : Csv(AssetKinds, nullptr, "kinds");
        Cmd.insert(Cmd.end(), { "--type", Types });
        static const std::vector<std::string> ImageFormats = { "png", "tga", "dds" }, AudioFormats = { "opus", "wav", "flac" };
        if (const auto F = OptList(Args, "name_filter"); !F.empty()) Cmd.insert(Cmd.end(), { "--name-filter", Csv(F, nullptr, "name_filter") });
        if (const long long L = OptInt(Args, "limit", 0); L != 0) Cmd.insert(Cmd.end(), { "--limit", std::to_string(std::clamp<long long>(L, 1, 100000)) });
        if (const auto F = OptList(Args, "model_format"); !F.empty()) Cmd.insert(Cmd.end(), { "--model-format", Csv(F, &kModelFormats, "model_format") });
        if (const auto F = OptList(Args, "anim_format"); !F.empty()) Cmd.insert(Cmd.end(), { "--anim-format", Csv(F, &kAnimFormats, "anim_format") });
        if (const auto F = OptString(Args, "image_format"); !F.empty()) Cmd.insert(Cmd.end(), { "--image-format", Csv({ F }, &ImageFormats, "image_format") });
        if (const auto F = OptString(Args, "audio_format"); !F.empty()) Cmd.insert(Cmd.end(), { "--audio-format", Csv({ F }, &AudioFormats, "audio_format") });
        if (OptBool(Args, "material_gdt", false)) Cmd.insert(Cmd.end(), { "--material-gdt", "on" });
        if (OptBool(Args, "sound_aliases", false)) Cmd.insert(Cmd.end(), { "--sound-aliases", "on" });
        if (OptBool(Args, "map_radiant", false)) Cmd.insert(Cmd.end(), { "--map-radiant", "on" });
        if (OptBool(Args, "decompile", false)) Cmd.push_back("--decompile");
        if (const auto L = OptString(Args, "languages"); !L.empty()) Cmd.insert(Cmd.end(), { "--languages", L });
        if (!OptBool(Args, "pull_dependencies", true)) throw ToolError("pull_dependencies=false is not offered by the CLI yet");

        if (!g_Slot.TryTake())
        {
            std::string Running;
            std::lock_guard<std::mutex> G(g_JobsLock);
            for (auto& [Id, Other] : g_Jobs)
            {
                std::lock_guard<std::mutex> G2(Other->M);
                if (Other->State == "running") Running += (Running.empty() ? "" : ", ") + Id;
            }
            throw ToolError("an export is running (" + Running + "): wait for it (graywolf_export_status)");
        }
        auto J = std::make_shared<Job>();
        J->Id = NewId();
        J->Args = Cmd;
        J->Out = Out;
        J->Zones = Zones;
        {
            std::lock_guard<std::mutex> G(g_JobsLock);
            g_Jobs[J->Id] = J;
        }
        g_JobThreads++;
        std::thread(RunJob, J).detach();
        Mcp::Log(std::format("export job {}: {} zone(s) to {}", J->Id, Zones.size(), Utf8(Out)));
        if (!OptBool(Args, "wait", true)) return Mcp::ToolResult::Object(Summary(*J));

        // Waiting: progress every 3 s, zones done plus a growing part of the current one (always
        // increasing). Cancelling this request cancels the export, as in the prototype.
        C.Cancel.OnFire([J] { J->Cancel.Fire(); });
        C.Progress(0, (double)J->Zones.size(), std::format("zone {}: started", J->Zones.front()));
        size_t Zone = (size_t)-1;
        int Ticks = 0;
        while (!J->WaitDone(std::chrono::seconds(3)))
        {
            size_t Now;
            {
                std::lock_guard<std::mutex> G(J->M);
                Now = J->ZoneIndex;
            }
            Ticks = Now == Zone ? Ticks + 1 : 0;
            Zone = Now;
            const long long Files = FilesUnder(Out).first;
            C.Progress((double)Now + 1.0 - 1.0 / (1.0 + Ticks / 20.0), (double)J->Zones.size(),
                       std::format("zone {}: {} files written", J->Zones[Now], Files));
        }
        C.Cancel.Clear();
        if (C.Cancel.Fired()) throw Mcp::Cancelled{};
        return Mcp::ToolResult::Object(Summary(*J));
    }

    Mcp::ToolResult ExportStatus(const Json& Args, Mcp::Call&)
    {
        return Mcp::ToolResult::Object(Summary(*FindJob(Args)));
    }

    Mcp::ToolResult ExportCancel(const Json& Args, Mcp::Call&)
    {
        const auto J = FindJob(Args);
        J->Cancel.Fire();
        J->WaitDone(std::chrono::seconds(60));
        return Mcp::ToolResult::Object(Summary(*J));
    }

    Mcp::ToolResult PreviewModel(const Json& Args, Mcp::Call& C)
    {
        const Install I = FindInstall(Args, C.Cancel);
        const std::string Model = OptString(Args, "model");
        const std::string Zone = ResolveZones(I, { OptString(Args, "zone") }, C.Cancel).front();
        const fs::path Scratch = OutDir("_preview") / Wide(NewId());
        struct Cleanup
        {
            fs::path P;
            ~Cleanup()
            {
                std::error_code Ec;
                fs::remove_all(P, Ec);
            }
        } Clean{ Scratch };

        g_Slot.Take(C.Cancel);
        try
        {
            RunChild({ "disk-export", Zone, "--install", I.Path, "--game", I.Game, "--out", Utf8(Scratch), "--assets", "model:" + Model,
                       "--type", "model", "--model-format", "semodel", "--image-format", "png" }, 900, C.Cancel);
        }
        catch (...)
        {
            g_Slot.Release();
            throw;
        }
        g_Slot.Release();

        std::vector<fs::path> Found;
        std::error_code Ec;
        for (auto It = fs::recursive_directory_iterator(Scratch, Ec); !Ec && It != fs::recursive_directory_iterator(); It.increment(Ec))
            if (Lower(Utf8(It->path().extension())) == ".semodel") Found.push_back(It->path());
        const std::string LowModel = Lower(Model);
        auto Rank = [&](const fs::path& P) { return std::make_pair(Lower(Utf8(P.stem())) != LowModel, P.native().size()); };
        std::stable_sort(Found.begin(), Found.end(), [&](const fs::path& A, const fs::path& B) { return Rank(A) < Rank(B); });
        if (Found.empty()) throw ToolError("model " + Model + " was not written from " + Zone + " (graywolf_find_assets finds its zone)");
        const fs::path Png = Scratch / "preview.png";
        RunChild({ "render-model", Utf8(Found.front()), "--out", Utf8(Png) }, 300, C.Cancel);
        std::vector<uint8_t> Bytes;
        {
            std::ifstream F(Png, std::ios::binary);
            Bytes.assign(std::istreambuf_iterator<char>(F), std::istreambuf_iterator<char>());
        }
        if (Bytes.empty()) throw ToolError("render-model wrote no image for " + Model);
        return Mcp::ToolResult::Image(Bytes, "image/png");
    }

    // ---- the tools' declarations (inputSchema/outputSchema as the prototype's)

    Json Annotations(bool ReadOnly)
    {
        return { { "readOnlyHint", ReadOnly }, { "destructiveHint", false }, { "idempotentHint", true }, { "openWorldHint", false } };
    }

    Json Schema(const std::string& Text) { return Json::parse(Text); }

    std::vector<Mcp::Tool> Tools()
    {
        const std::string PageProps = R"("offset": {"type": "integer", "default": 0}, "limit": {"type": "integer", "default": 100})";
        const std::string PageOut = R"("total": {"type": "integer"}, "offset": {"type": "integer"}, "items": {"type": "array"},
                                       "next_offset": {"type": ["integer", "null"]})";
        const std::string OptStr = R"({"type": ["string", "null"], "default": null})";
        const std::string OptArr = R"({"type": ["array", "null"], "items": {"type": "string"}, "default": null})";
        std::vector<Mcp::Tool> T;
        T.push_back({ "graywolf_list_installs", "List Call of Duty installs",
            "The Call of Duty installs found on this machine (Steam, Battle.net, Xbox, folders), oldest game first: each one's "
            "title, year, game id (what the other tools take as `game`), language and path (what they take as `install`).",
            Schema(R"({"type": "object", "properties": {}})"),
            Schema(R"({"type": "object", "properties": {"installs": {"type": "array", "items": {"type": "object"}}, "output_root": {"type": "string"}}})"),
            Annotations(true), ListInstalls });
        T.push_back({ "graywolf_list_zones", "List an install's zones",
            "An install's zones (fast files) as GrayWolf lists them, each with its level's display name when it is a level "
            "(\"Blood of the Dead\"). contains: substring of the zone or level name, any case. levels_only: only the zones that "
            "are levels. Paged (offset, limit <= 500).",
            Schema(R"({"type": "object", "required": ["install"], "properties": {"install": {"type": "string"}, "game": )" + OptStr +
                   R"(, "contains": )" + OptStr + R"(, "levels_only": {"type": "boolean", "default": false}, )" + PageProps + "}}"),
            Schema(R"({"type": "object", "properties": {"game": {"type": "string"}, )" + PageOut + "}}"),
            Annotations(true), ListZones });
        T.push_back({ "graywolf_find_assets", "Find which zones define assets",
            "Which zones of an install define assets, from GrayWolf's install index (built on first use: can take minutes for a "
            "large install, then kept). names: whole asset names, any case; contains: substrings of names. kinds: model, anim, "
            "image, sound, ... Each row: kind, name, zone, the zone's size in bytes; the smallest zone first for each asset (the "
            "quickest to export it from). Names not found are listed.",
            Schema(R"({"type": "object", "required": ["install"], "properties": {"install": {"type": "string"}, "names": )" + OptArr +
                   R"(, "contains": )" + OptArr + R"(, "kinds": )" + OptArr + R"(, "game": )" + OptStr + ", " + PageProps + "}}"),
            Schema(R"({"type": "object", "properties": {"game": {"type": "string"}, "assets": {"type": "integer"},
                "missing": {"type": "array", "items": {"type": "string"}}, )" + PageOut + "}}"),
            Annotations(true), FindAssets });
        T.push_back({ "graywolf_estimate", "Estimate an export's size",
            "The size an export of these zones would write, as GrayWolf's Load Game panel estimates it (a sample of each kind is "
            "measured: can take a minute). zones: as graywolf_list_zones names them.",
            Schema(R"({"type": "object", "required": ["install", "zones"], "properties": {"install": {"type": "string"},
                "zones": {"type": "array", "items": {"type": "string"}}, "kinds": )" + OptArr + R"(, "game": )" + OptStr + "}}"),
            Schema(R"({"type": "object", "properties": {"game": {"type": "string"}, "bytes": {"type": "integer"},
                "text": {}, "kinds": {"type": "object"}}})"),
            Annotations(true), Estimate });
        T.push_back({ "graywolf_export", "Export assets or zones",
            "Export from an install's zones into <output root>/<out_name>/<game>/, through GrayWolf's own export (what the Load "
            "Game panel runs). zones: as graywolf_list_zones names them (find an asset's zone with graywolf_find_assets). assets: "
            "only these, \"kind:name\" (\"model:wpn_t7_ar_standard\", \"image:i_x\"); else kinds (default model, anim, image, "
            "sound) and name_filter (substrings). Formats: model_format (cast, semodel, xmodel_export, glb, ...; several allowed), "
            "anim_format (cast, seanim, xanim_export), image_format (png, tga, dds), audio_format (opus, wav, flac). Black Ops III "
            "mod tools: material_gdt (material/image GDT entries), sound_aliases (alias CSV beside the audio), map_radiant (a map's "
            ".map). decompile: scripts decompiled (kind script). What the assets need from other zones (textures, rigs) is pulled "
            "too. A whole level takes minutes: wait=false returns a job id at once, followed with graywolf_export_status. One "
            "export runs at a time.",
            Schema(R"({"type": "object", "required": ["install", "zones", "out_name"], "properties": {
                "install": {"type": "string"}, "zones": {"type": "array", "items": {"type": "string"}}, "out_name": {"type": "string"},
                "game": )" + OptStr + R"(, "assets": )" + OptArr + R"(, "kinds": )" + OptArr + R"(, "name_filter": )" + OptArr + R"(,
                "limit": {"type": ["integer", "null"], "default": null},
                "model_format": )" + OptArr + R"(, "anim_format": )" + OptArr + R"(,
                "image_format": )" + OptStr + R"(, "audio_format": )" + OptStr + R"(,
                "material_gdt": {"type": "boolean", "default": false}, "sound_aliases": {"type": "boolean", "default": false},
                "map_radiant": {"type": "boolean", "default": false}, "decompile": {"type": "boolean", "default": false},
                "languages": )" + OptStr + R"(,
                "pull_dependencies": {"type": "boolean", "default": true}, "wait": {"type": "boolean", "default": true}}})"),
            Schema(R"({"type": "object", "properties": {"job": {"type": "string"}, "state": {"type": "string"}, "out": {"type": "string"},
                "files": {"type": "integer"}, "bytes": {"type": "integer"}}})"),
            Annotations(false), Export });
        const std::string JobIn = R"({"type": "object", "required": ["job"], "properties": {"job": {"type": "string"}}})";
        const std::string JobOut = R"({"type": "object", "properties": {"job": {"type": "string"}, "state": {"type": "string"}}})";
        T.push_back({ "graywolf_export_status", "An export's status",
            "Where an export started with wait=false is: running/done/failed/cancelled, the zone it is on, the files and bytes "
            "written so far; once done, the counts per kind, failures, unresolved dependencies and some of the files written.",
            Schema(JobIn), Schema(JobOut), Annotations(true), ExportStatus });
        T.push_back({ "graywolf_export_cancel", "Cancel an export", "Stop a running export (what it wrote so far stays).",
            Schema(JobIn), Schema(JobOut), Annotations(false), ExportCancel });
        T.push_back({ "graywolf_preview_model", "Preview a model",
            "A rendered PNG of one model (shaded and textured, software renderer), to check it is the right one before exporting "
            "it: the model alone is exported from the zone into a scratch folder under the output root, then drawn.",
            Schema(R"({"type": "object", "required": ["install", "zone", "model"], "properties": {
                "install": {"type": "string"}, "zone": {"type": "string"}, "model": {"type": "string"}, "game": )" + OptStr + "}}"),
            Json(), Annotations(false), PreviewModel });
        return T;
    }

    // Output root: --out, else GRAYWOLF_MCP_OUT, else the mcp_out setting, else
    // %LOCALAPPDATA%\GrayWolf\mcp: per user, writable without rights, never synced (OneDrive
    // moves Documents) nor swept like %TEMP%.
    fs::path PickOutRoot(const CliOptions& Options)
    {
        std::wstring Root = Wide(Options.Out);
        std::vector<wchar_t> Buf(32768);
        if (Root.empty() && GetEnvironmentVariableW(L"GRAYWOLF_MCP_OUT", Buf.data(), (DWORD)Buf.size())) Root = Buf.data();
        if (Root.empty()) Root = Wide(SettingsManager::GetSetting("mcp_out", ""));
        if (!Root.empty()) return FullPath(fs::path(Root));
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", Buf.data(), (DWORD)Buf.size())) return FullPath(fs::path(Buf.data()) / L"GrayWolf" / L"mcp");
        GetTempPathW((DWORD)Buf.size(), Buf.data());
        return FullPath(fs::path(Buf.data()) / L"graywolf-mcp");
    }
}

bool IsMcpRun(int argc, char** argv)
{
    for (int I = 1; I < argc; I++)
        if (std::string(argv[I]) == "mcp") return true;
    return false;
}

int RunMcp(const CliOptions& Options)
{
    // No update code runs in mcp mode, and the children are told so too (GRAYWOLF_NO_UPDATE).
    SetEnvironmentVariableW(L"GRAYWOLF_NO_UPDATE", L"1");
    std::vector<wchar_t> Self(32768);
    GetModuleFileNameW(nullptr, Self.data(), (DWORD)Self.size());
    g_Exe = fs::path(std::wstring(Self.data()));
    g_OutRoot = PickOutRoot(Options);
    std::error_code Ec;
    fs::create_directories(g_OutRoot, Ec);

    // Every child joins this job: killed when the server ends, even by a crash (the handle closes).
    g_Job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limits{};
    Limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(g_Job, JobObjectExtendedLimitInformation, &Limits, sizeof(Limits));

    Mcp::ServerInfo Info;
    Info.Name = "graywolf";
    Info.Title = "GrayWolf";
    Info.Version = Updater::AppVersion();
    Info.Instructions =
        "GrayWolf extracts Call of Duty assets (models, anims, images, sounds, maps, scripts) from the games' files, offline. "
        "Start with graywolf_list_installs, then graywolf_list_zones or graywolf_find_assets (by asset name) for an install, "
        "then graywolf_export. Exports write under " + Utf8(g_OutRoot) + ". For Black Ops III mod tools use model_format "
        "xmodel_export, anim_format xanim_export, and material_gdt / sound_aliases.";
    Mcp::Log("mcp: GrayWolf " + Info.Version + ", output root " + Utf8(g_OutRoot));

    const int Code = Mcp::Serve(Info, Tools(), [] {
        // the client is gone: cancel every export, kill every child, give the job threads a moment
        {
            std::lock_guard<std::mutex> G(g_JobsLock);
            for (auto& [Id, J] : g_Jobs) J->Cancel.Fire();
        }
        TerminateJobObject(g_Job, 1);
        for (int I = 0; I < 100 && g_JobThreads > 0; I++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    });
    if (g_JobThreads > 0) ExitProcess((UINT)Code);
    CloseHandle(g_Job);
    return Code;
}

void AddMcpCommands(CliCommandBuilders& B)
{
    B["mcp"] = [](CLI::App& App, CliOptions& Options) {
        App.add_subcommand("mcp", "Serve GrayWolf's tools to an AI agent over stdio (Model Context Protocol): "
                                  "claude mcp add graywolf -- <path>\\GrayWolf.exe mcp. Exports go under --out, else "
                                  "GRAYWOLF_MCP_OUT, else the mcp_out setting, else %LOCALAPPDATA%\\GrayWolf\\mcp")
            ->callback([&Options] { Options.Mcp = true; });
    };
}
