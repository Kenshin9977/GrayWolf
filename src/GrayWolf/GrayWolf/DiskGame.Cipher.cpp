#include "stdafx.h"

// DiskGame::EnsureStringCipher: a game's string cipher table (StringCipher.h) rebuilt from the
// install's own zones (CipherFit.h) when its profile has no exe-made strings.cipher.txt, which is
// never shipped. Kept beside the install index, <game>_<install hash>.cipher: a key line (the
// zones' files), then the table in strings.cipher.txt's format. Made while the install index is
// built (CipherHarvest: the zones it opens give their strings as it has them, the others are read
// after it), else at the start of a run whose kinds read the strings (EnsureStringCipher).

#include "DiskGame.h"
#include "ExportNaming.h"
#include "ExportWriters.h"
#include "FastFile.h"
#include "FileSystems.h"
#include "Games/Common/CipherFit.h"
#include "Games/Common/StringCipher.h"
#include "WraithNameIndex.h"

#include <Windows.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace DiskGame
{
namespace
{
    // bumped when the fit or what is read of the zones changes: every kept table is made again
    constexpr int Format = 1;

    using ExportNaming::Fnv1a;
    using ExportNaming::Hex16;
    using ExportNaming::Lower;

    bool Env(const char* Name) { return GetEnvironmentVariableA(Name, nullptr, 0) != 0; }
    std::string EnvText(const char* Name)
    {
        char Buf[1024] = {};
        const DWORD N = GetEnvironmentVariableA(Name, Buf, sizeof(Buf));
        return N > 0 && N < sizeof(Buf) ? std::string(Buf, N) : std::string();
    }

    // the process's peak working set, and its peak private bytes (commit)
    size_t PeakMb(bool Commit = false)
    {
        PROCESS_MEMORY_COUNTERS M{};
        M.cb = sizeof(M);
        if (!GetProcessMemoryInfo(GetCurrentProcess(), &M, sizeof(M))) return 0;
        return (Commit ? M.PeakPagefileUsage : M.PeakWorkingSetSize) >> 20;
    }

    // a kept file's key (its first lines: a comment, "key <hex>"), empty when it has none
    std::string KeptKey(const fs::path& File)
    {
        std::ifstream In(File);
        std::string Line;
        for (int N = 0; N < 4 && std::getline(In, Line); N++)
            if (Line.rfind("key ", 0) == 0) return Line.substr(4);
        return std::string();
    }

    // The names the name databases beside the exe hold of the kind the strings are (bones,
    // notetracks, script strings: package_index's fnv1a_bones, fnv1a_string): the fit's first
    // character model. None when they are not there (the fit's own words then).
    std::vector<std::string> NameCorpus()
    {
        std::vector<std::string> Out;
        const fs::path Dir = fs::path(FileSystems::GetApplicationPath()) / "package_index";
        for (const char* F : { "fnv1a_bones.wni", "fnv1a_string.wni" })
        {
            std::error_code Ec;
            if (!fs::is_regular_file(Dir / F, Ec)) continue;
            WraithNameIndex Names;
            Names.LoadIndex((Dir / F).string());
            for (const auto& [Hash, Name] : Names.NameDatabase)
                if (!Name.empty() && Name.size() < 254 && std::all_of(Name.begin(), Name.end(), [](char C) { return C >= 0x20 && C < 0x7F; }))
                    Out.push_back(Name);
        }
        std::sort(Out.begin(), Out.end());
        Out.erase(std::unique(Out.begin(), Out.end()), Out.end());
        return Out;
    }

    // The zones' encrypted strings, the zones they were read from
    struct Collected
    {
        std::vector<std::string> Script, Localized;
        size_t Zones = 0, Failed = 0;
    };

    // What a rebuild reads: the cipher's zones (Reads, one language's localized ones) of an install,
    // the table's key (their files) and where it is kept
    struct Plan
    {
        const CipherSource* Src = nullptr;
        std::string Dir;        // the profile's folder
        std::string Install;    // where the zones are listed
        std::vector<std::pair<std::string, bool>> Zones;   // name, of the language read
        std::unordered_map<std::string, size_t> Of;        // a zone's place in Zones
        std::string Key;
        fs::path File;
        std::string Once, OnceGiven;   // the install as found and as given (Rebuilt)
    };

    // the installs whose table this process has (kept, rebuilt, or not makeable): not looked at again
    std::mutex OnceLock;
    std::set<std::string> Rebuilt;
    bool RebuiltAlready(const std::string& Of)
    {
        const std::lock_guard<std::mutex> G(OnceLock);
        return Rebuilt.count(Of) != 0;
    }
    void MarkRebuilt(const Plan& P)
    {
        const std::lock_guard<std::mutex> G(OnceLock);
        Rebuilt.insert(P.Once);
        Rebuilt.insert(P.OnceGiven);
    }

    // The plan of Game's cipher for the install Given; false with nothing to do (no cipher, the
    // profile's own table, done already in this process: Error empty) or none possible (Error)
    bool MakePlan(const IDiskGame& Game, const ExportOptions& Options, const std::string& Given, Plan& P, std::string& Error)
    {
        P.Src = Game.CipherStrings();
        if (P.Src == nullptr || Given.empty() || Env("GRAYWOLF_NO_ZONE_CIPHER")) return false;
        const auto* Profile = ExportWriters::ZoneProfile(P.Src->Profile, Error);
        if (Profile == nullptr) return false;
        P.Dir = Profile->Profile.Dir;
        if (StringCipher::HasOwn(P.Dir)) return false;   // the exe-made table wins
        P.OnceGiven = Lower(P.Dir) + "|" + Lower(fs::absolute(Given).string());
        if (RebuiltAlready(P.OnceGiven)) return false;
        // a zone's folder given as the install (MWIII's cod23 when no --install names it): the one above
        P.Install = Given;
        std::vector<std::string> Names;
        if (!Game.ListZones(P.Install, Options, Names, Error))
        {
            std::string Why;
            P.Install = fs::path(Given).parent_path().string();
            if (P.Install.empty() || !Game.ListZones(P.Install, Options, Names, Why)) return false;
            Error.clear();
        }
        P.Once = Lower(P.Dir) + "|" + Lower(fs::absolute(P.Install).string());
        if (RebuiltAlready(P.Once)) return false;
        std::erase_if(Names, [&](const std::string& N) { return P.Src->Reads && !P.Src->Reads(N); });
        // the localized zones of one language (English when there, else the most common); those of the
        // others are left out (the same texts in other words, and accents the hash check misses)
        std::map<std::string, size_t> Spoken;
        for (const auto& N : Names)
            if (const std::string L = Game.LanguageOfZone(N); !L.empty()) Spoken[L]++;
        std::string Language = Spoken.count("en") ? "en" : std::string();
        for (const auto& [L, N] : Spoken)
            if (Language.empty() || (Language != "en" && N > Spoken[Language])) Language = L;
        for (const auto& N : Names)
            if (const std::string L = Game.LanguageOfZone(N); L.empty() || L == Language) P.Zones.push_back({ N, !L.empty() });
        if (P.Zones.empty())
        {
            Error = "no zone to rebuild the string cipher from";
            return false;
        }
        std::vector<std::string> Listed;
        for (size_t Z = 0; Z < P.Zones.size(); Z++)
        {
            Listed.push_back(P.Zones[Z].first);
            P.Of.emplace(P.Zones[Z].first, Z);
        }
        P.File = KeptFile(Game, Options, P.Install, ".cipher");
        std::string KeyText = "cipher format " + std::to_string(Format) + "\n" + Game.Id() + "\n" + Lower(fs::absolute(P.Install).string()) +
                              "\nfiles " + ZoneFilesStamp(P.Install, Listed) + "\n";
        for (const auto& N : Listed) KeyText += N + "\n";
        P.Key = Hex16(Fnv1a(KeyText));
        return true;
    }

    // The kept table, when its key is the plan's (GRAYWOLF_CIPHER_REBUILD=1: never): given to
    // StringCipher::Load
    bool UseKept(const Plan& P)
    {
        if (Env("GRAYWOLF_CIPHER_REBUILD") || KeptKey(P.File) != P.Key) return false;
        std::string Error;
        auto T = StringCipher::ReadFile(P.File.string(), Error);
        if (T == nullptr) return false;
        StringCipher::UseRebuilt(P.Dir, std::move(T));
        MarkRebuilt(P);
        return true;
    }
}

// The strings gathered, from the zones the install index opens (Add) and those read here
struct CipherHarvest::State
{
    Plan P;
    std::mutex Lock;
    std::unordered_set<std::string> Script, Localized;
    std::vector<char> Read;   // per zone of the plan: its strings are in
    size_t Zones = 0, Failed = 0, Riding = 0;
    double RidingSeconds = 0;   // spent in Add, on the index's workers (summed)

    // a zone's strings, from its bytes as decompressed
    void Take(size_t Z, ByteView Bytes)
    {
        std::vector<std::string> S, L;
        P.Src->Strings(P.Zones[Z].first, Bytes, P.Zones[Z].second, S, L);
        const std::lock_guard<std::mutex> G(Lock);
        if (Read[Z]) return;
        Read[Z] = 1;
        Zones++;
        for (auto& T : S)
            if (CipherFit::Encrypted(T)) Script.insert(std::move(T));
        if (P.Zones[Z].second)
            for (auto& T : L)
                if (CipherFit::Encrypted(T)) Localized.insert(std::move(T));
    }

    // The plan's zones not read yet, each decompressed, read and let go, by GRAYWOLF_CIPHER_THREADS
    // workers (4 by default: MWIII's 496 zones in 12 s, 3 GB at most with 8); false when cancelled
    bool ReadRest(const IDiskGame& Game, const ExportOptions& Options, const CipherProgress& Progress)
    {
        std::vector<size_t> Left;
        for (size_t Z = 0; Z < P.Zones.size(); Z++)
            if (!Read[Z]) Left.push_back(Z);
        if (Left.empty()) return true;
        // GRAYWOLF_CIPHER_ZONES=<folder>: decompressed zones (<name>.bin) read instead (a test's)
        const std::string Bins = EnvText("GRAYWOLF_CIPHER_ZONES");
        std::mutex ResolveLock;
        std::atomic<size_t> Next{ 0 }, Done{ 0 };
        const auto Cancelled = [&] { return Progress.Cancel != nullptr && Progress.Cancel->load(); };
        const auto Work = [&] {
            for (size_t K; (K = Next++) < Left.size() && !Cancelled();)
            {
                const size_t Z = Left[K];
                const std::string& Name = P.Zones[Z].first;
                std::string Error;
                bool Got = false;
                if (!Bins.empty())
                {
                    std::ifstream In(fs::path(Bins) / (fs::path(Name).stem().string() + ".bin"), std::ios::binary);
                    std::vector<uint8_t> Bytes((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
                    if ((Got = !Bytes.empty())) Take(Z, ByteView(Bytes));
                }
                else
                {
                    std::vector<std::string> Paths;
                    bool Resolved;
                    {
                        const std::lock_guard<std::mutex> G(ResolveLock);
                        Resolved = Game.ResolveZones(P.Install, { Name }, Options, Paths, Error) && !Paths.empty();
                    }
                    // (its section torn down when Bytes goes, whatever the worker: wolff::ZoneMemory::Free)
                    ZoneBytes Bytes;
                    if (Resolved && FastFile::Decompress(Paths.front(), Bytes, Error))
                    {
                        Got = true;
                        Take(Z, ByteView(Bytes));
                    }
                }
                if (!Got)
                {
                    const std::lock_guard<std::mutex> G(Lock);
                    Failed++;
                }
                const size_t D = ++Done;
                if (Progress.Step) Progress.Step(D, Left.size());
            }
        };
        unsigned Threads = 4;
        if (const std::string T = EnvText("GRAYWOLF_CIPHER_THREADS"); !T.empty()) Threads = std::max(1, std::atoi(T.c_str()));
        Threads = std::min<unsigned>(Threads, (unsigned)Left.size());
        std::vector<std::thread> Ts;
        for (unsigned T = 1; T < Threads; T++) Ts.emplace_back(Work);
        Work();
        for (auto& T : Ts) T.join();
        return !Cancelled();
    }
};

namespace
{
    // GRAYWOLF_CIPHER_SCORE: the rebuilt table against an exe-made one in Dir (read whatever
    // GRAYWOLF_NO_STRING_CIPHER says), over the strings it was fitted on; counts only
    void Score(const StringCipher::Table* Mine, const std::string& Dir, const Collected& C, std::map<std::string, size_t>& Counters,
               const CipherProgress& Progress)
    {
        std::string Error;
        const auto Theirs = StringCipher::ReadFile((fs::path(Dir) / "strings.cipher.txt").string(), Error);
        if (Theirs == nullptr)
        {
            if (Progress.Log) Progress.Log("string cipher score: no table of the profile's to score against (" + Error + ")");
            return;
        }
        for (const auto& [What, Strings] : { std::pair<const char*, const std::vector<std::string>*>{ "script", &C.Script },
                                             std::pair<const char*, const std::vector<std::string>*>{ "localize", &C.Localized } })
        {
            size_t Known = 0, Same = 0, Bytes = 0, SameBytes = 0;
            for (const auto& Raw : *Strings)
            {
                std::string A, B;
                if (!StringCipher::Decrypt(Theirs.get(), Raw, A)) continue;
                Known++;
                const bool Got = StringCipher::Decrypt(Mine, Raw, B);
                Same += Got && A == B;
                Bytes += A.size();
                if (Got)
                    for (size_t I = 0; I < A.size() && I < B.size(); I++) SameBytes += A[I] == B[I];
            }
            const std::string P = std::string("cipher_score_") + What;
            Counters[P + "_strings"] = Known;
            Counters[P + "_identical"] = Same;
            Counters[P + "_bytes"] = Bytes;
            Counters[P + "_bytes_same"] = SameBytes;
            if (Progress.Log && Known)
            {
                char Line[200];
                std::snprintf(Line, sizeof(Line), "string cipher score, %s: %.1f %% of %zu strings identical, %.1f %% of bytes", What,
                              100.0 * (double)Same / (double)Known, Known, Bytes ? 100.0 * (double)SameBytes / (double)Bytes : 0.0);
                Progress.Log(Line);
            }
        }
    }
}

std::unique_ptr<CipherHarvest> CipherHarvest::Begin(const IDiskGame& Game, const ExportOptions& Options, const std::string& Install,
                                                    std::map<std::string, size_t>* Counters)
{
    auto S = std::make_unique<State>();
    std::string Error;
    if (!MakePlan(Game, Options, Install, S->P, Error)) return nullptr;
    if (UseKept(S->P))
    {
        if (Counters) (*Counters)["cipher_rebuilt_kept"]++;
        return nullptr;
    }
    S->Read.assign(S->P.Zones.size(), 0);
    auto H = std::unique_ptr<CipherHarvest>(new CipherHarvest());
    H->S = std::move(S);
    return H;
}

CipherHarvest::~CipherHarvest() = default;

void CipherHarvest::Add(const std::string& Zone, ByteView Bytes)
{
    const auto It = S->P.Of.find(Zone);
    if (It == S->P.Of.end() || Bytes.empty()) return;
    const auto Start = std::chrono::steady_clock::now();
    S->Take(It->second, Bytes);
    const double Took = std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
    const std::lock_guard<std::mutex> G(S->Lock);
    S->Riding++;
    S->RidingSeconds += Took;
}

bool CipherHarvest::Finish(const IDiskGame& Game, const ExportOptions& Options, const CipherProgress& Progress,
                           std::map<std::string, size_t>& Counters, std::string& Error)
{
    // (one rebuild at a time: a second one waits, then finds the table kept)
    static std::mutex Fitting;
    const std::lock_guard<std::mutex> Guard(Fitting);
    Plan& P = S->P;
    const auto Log = [&](const std::string& Line) {
        if (Progress.Log) Progress.Log(Line);
    };
    if (S->Riding == 0 && UseKept(P))
    {
        Counters["cipher_rebuilt_kept"]++;
        return true;
    }
    const auto Start = std::chrono::steady_clock::now();
    size_t Left = 0;
    for (const char R : S->Read) Left += !R;
    Log("rebuilding the string cipher from the install's zones, once for this install: " + std::to_string(P.Zones.size()) + " zones" +
        (S->Riding ? " (" + std::to_string(S->Riding) + " read by the install index, " + std::to_string(Left) + " to read)" : std::string()) +
        "; later runs read it, made again only when the game's files change");
    Collected C;
    // GRAYWOLF_CIPHER_STRINGS=<file>: the encrypted strings read from it when it exists, else
    // written to it (a test's: the zones read once for many fits)
    const std::string StringsFile = EnvText("GRAYWOLF_CIPHER_STRINGS");
    if (std::ifstream In(StringsFile, std::ios::binary); !StringsFile.empty() && In)
    {
        std::string Line;
        while (std::getline(In, Line))
        {
            if (Line.size() < 3) continue;
            std::string T;
            for (size_t I = 2; I + 1 < Line.size(); I += 2) T.push_back((char)std::stoi(Line.substr(I, 2), nullptr, 16));
            (Line[0] == 'l' ? C.Localized : C.Script).push_back(std::move(T));
        }
    }
    else
    {
        // GRAYWOLF_CIPHER_READ_ONLY (a test's): the system's commit sampled while the zones are read
        // (a zone's section is commit, not this process's private bytes)
        std::atomic<bool> Sampling{ Env("GRAYWOLF_CIPHER_READ_ONLY") };
        size_t CommitBase = 0, CommitPeak = 0;
        const auto SystemCommit = [] {
            PERFORMANCE_INFORMATION I{};
            I.cb = sizeof(I);
            return GetPerformanceInfo(&I, sizeof(I)) ? (size_t)I.CommitTotal * I.PageSize : (size_t)0;
        };
        std::thread Sampler;
        if (Sampling.load())
        {
            CommitBase = CommitPeak = SystemCommit();
            Sampler = std::thread([&] {
                while (Sampling.load())
                {
                    CommitPeak = std::max(CommitPeak, SystemCommit());
                    Sleep(20);
                }
            });
        }
        const bool Got = S->ReadRest(Game, Options, Progress);
        if (Sampler.joinable())
        {
            Sampling = false;
            Sampler.join();
            Counters["cipher_system_commit_growth_mb"] = (CommitPeak - CommitBase) >> 20;
        }
        if (!Got)
        {
            Error = "cancelled";
            return false;
        }
        // in one order whatever order the zones were read in (the fit's input)
        C.Script.assign(S->Script.begin(), S->Script.end());
        C.Localized.assign(S->Localized.begin(), S->Localized.end());
        std::sort(C.Script.begin(), C.Script.end());
        std::sort(C.Localized.begin(), C.Localized.end());
        C.Zones = S->Zones;
        C.Failed = S->Failed;
        if (!StringsFile.empty())
        {
            std::ofstream Out(StringsFile, std::ios::binary);
            char H[3];
            for (const auto& [Kind, V] : { std::pair<char, const std::vector<std::string>*>{ 's', &C.Script }, { 'l', &C.Localized } })
                for (const auto& T : *V)
                {
                    Out << Kind << ' ';
                    for (const char Ch : T)
                    {
                        std::snprintf(H, sizeof(H), "%02x", (uint8_t)Ch);
                        Out << H;
                    }
                    Out << '\n';
                }
        }
    }
    const double ReadS = std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
    Counters["cipher_zones"] = C.Zones;
    Counters["cipher_zones_riding"] = S->Riding;
    Counters["cipher_ms_riding"] = (size_t)(S->RidingSeconds * 1000);
    Counters["cipher_seconds_read"] = (size_t)ReadS;
    Counters["cipher_ms_read"] = (size_t)(ReadS * 1000);
    if (Env("GRAYWOLF_CIPHER_READ_ONLY"))
    {
        // (a test's: the zones' reading measured alone, no fit)
        Counters["cipher_strings"] = C.Script.size() + C.Localized.size();
        Counters["cipher_peak_mb"] = PeakMb();
        Counters["cipher_peak_commit_mb"] = PeakMb(true);
        return true;
    }
    Log("string cipher: " + std::to_string(C.Script.size()) + " script strings, " + std::to_string(C.Localized.size()) + " localized texts from " +
        std::to_string(C.Zones) + " zones (" + std::to_string(S->Riding) + " as the install index read them, " + std::to_string(C.Failed) +
        " not read), " + std::to_string((int)ReadS) + " s here, peak " + std::to_string(PeakMb()) + " MB (" + std::to_string(PeakMb(true)) +
        " MB committed)");
    CipherFit::Options Fit;
    Fit.HashChecked = P.Src->HashChecked;
    if (!Env("GRAYWOLF_CIPHER_NO_NAMES")) Fit.Corpus = NameCorpus();
    Log("string cipher fit: " + std::to_string(Fit.Corpus.size()) + " names of the name databases as its first model");
    Fit.Log = Progress.Log;
    Fit.Step = Progress.Step;
    Fit.Cancel = Progress.Cancel;
    std::vector<std::string> All = C.Script;
    All.insert(All.end(), C.Localized.begin(), C.Localized.end());
    CipherFit::Result R;
    if (!CipherFit::Fit(All, Fit, R))
    {
        const bool Cancelled = Progress.Cancel != nullptr && Progress.Cancel->load();
        Error = Cancelled ? "cancelled" : "no encrypted string in the zones";
        if (!Cancelled) MarkRebuilt(P);   // (not read again on every run)
        return false;
    }
    All.clear();
    All.shrink_to_fit();
    // written whole, then moved in place
    {
        std::ofstream Outf(P.File.string() + ".tmp", std::ios::binary);
        Outf << "# GrayWolf: the string cipher rebuilt from this install's zones (DiskGame.Cipher.cpp)\nkey " << P.Key << "\n" << R.Table;
    }
    std::error_code Ec;
    fs::rename(P.File.string() + ".tmp", P.File, Ec);
    auto T = StringCipher::ReadFile(Ec ? P.File.string() + ".tmp" : P.File.string(), Error);
    if (T == nullptr) return false;
    const double Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
    Counters["cipher_rebuilt"]++;
    Counters["cipher_strings"] = R.Strings;
    Counters["cipher_strings_checked"] = R.Checked;
    Counters["cipher_slots"] = R.Slots;
    Counters["cipher_seconds"] = (size_t)Seconds;
    Counters["cipher_peak_mb"] = PeakMb();
    Counters["cipher_peak_commit_mb"] = PeakMb(true);
    Log("string cipher rebuilt: " + std::to_string(R.Strings) + " strings, " + std::to_string(R.Checked) + " checked, " + std::to_string(R.Slots) +
        " ciphers, in " + std::to_string((int)Seconds) + " s (zones " + std::to_string((int)ReadS) + " s), peak " + std::to_string(PeakMb()) +
        " MB; kept in " + P.File.string());
    if (Env("GRAYWOLF_CIPHER_SCORE"))
    {
        // GRAYWOLF_CIPHER_SCORE=<folder of profiles>: the table there, else the profile's own
        const fs::path Root = EnvText("GRAYWOLF_CIPHER_SCORE");
        Score(T.get(), fs::is_directory(Root / P.Src->Profile) ? (Root / P.Src->Profile).string() : P.Dir, C, Counters, Progress);
    }
    StringCipher::UseRebuilt(P.Dir, std::move(T));
    MarkRebuilt(P);
    return true;
}

bool EnsureStringCipher(const IDiskGame& Game, const ExportOptions& Options, const std::string& Given, const CipherProgress& Progress,
                        std::map<std::string, size_t>& Counters, std::string& Error)
{
    Error.clear();
    auto H = CipherHarvest::Begin(Game, Options, Given, &Counters);
    if (H == nullptr)
    {
        // (nothing to do, or a plan that could not be made: said)
        Plan P;
        std::string Why;
        if (MakePlan(Game, Options, Given, P, Why) || Why.empty()) return true;
        Error = Why;
        return false;
    }
    return H->Finish(Game, Options, Progress, Counters, Error);
}

bool NeedsStringCipher(const std::set<Kind>& Kinds)
{
    // the kinds whose names or texts never come from the zone's strings
    static const std::set<Kind> Without = { Kind::Image, Kind::Sound, Kind::SoundLinks, Kind::MaterialLinks, Kind::Techsets };
    return std::any_of(Kinds.begin(), Kinds.end(), [](Kind K) { return !Without.count(K); });
}
}
