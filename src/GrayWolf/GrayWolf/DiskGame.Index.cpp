#include "stdafx.h"

// DiskGame::ZoneIndex: where each asset of an install is defined, Wolff's install index
// (wolff/InstallIndex.h) of the zones this game's readers open. One file per game and install under
// <--cache>/index (or %TEMP%/greyhound_index), <game>_<install hash>.gwi, read in place. Built by
// Wolff's pool of workers (wolff::BuildZones), each zone opened by its reader here, resumable from a
// partial file beside it. The key hashes the format, the game, the install, the zone list and the
// reader's IndexKey (its tables): any change rebuilds the file. The text index of before (.idx) is
// read once and written as one. Its types are the kinds' names, a type's id the kind's value.

#include "DiskGame.h"
#include "DiskGameSizes.h"
#include "ExportNaming.h"
#include "ExportWriters.h"
#include "Siren.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <unordered_map>
#include <string_view>
#include <thread>

namespace fs = std::filesystem;

namespace DiskGame
{
namespace
{
    // bumped when what a reader's Defines lists changes (its tables are in the key already); 3:
    // what each asset weighs (IDiskZone::Weighs)
    constexpr int Format = 3;
    // the format before weights: its index still tells an export where each asset is
    constexpr int Unweighed = 2;
    // the text index of before (InstallIndex::LoadText), read to be written as a kept file
    constexpr int TextFormat = 2;

    using ExportNaming::Fnv1a;
    using ExportNaming::Hex16;
    using ExportNaming::Lower;

    std::mutex& PackageLock()
    {
        return IndexPackageLock();
    }

    using Clock = std::chrono::steady_clock;
    double Since(Clock::time_point From, Clock::time_point To = Clock::now())
    {
        return std::chrono::duration<double>(To - From).count();
    }

    // What one zone's read took, stage by stage, in seconds (the build's report, BuildTimes): its
    // resolve (waiting for the package lock, then its work), its wait to be admitted (zone order,
    // the budget), its open (the reader's own steps: load, walk, links, session...), the string
    // cipher's take, the weights, a sound dry run, its parts, its close
    struct ZoneTimes
    {
        bool Read = false;
        unsigned Worker = 0;
        Clock::time_point Start{}, End{};
        double ResolveWait = 0, Resolve = 0, Admit = 0, Open = 0, Harvest = 0, Weigh = 0, SoundWait = 0, Sounds = 0, Parts = 0, Close = 0;
        bool SoundsRan = false, SoundsStopped = false;
        size_t SoundsHanded = 0;    // the sounds a dry run's WriteSound was handed, and their bytes
        uint64_t SoundBytes = 0;
        std::vector<std::pair<std::string, double>> Steps;
        uint64_t File = 0, Footprint = 0;
        size_t Assets = 0;
    };

    // A sound's name as a key: lower case, '/' between folders, without a sound file's extension
    std::string SoundKey(std::string Name)
    {
        Name = Lower(Name);
        std::replace(Name.begin(), Name.end(), '\\', '/');
        for (const std::string_view E : { ".wav", ".flac", ".mp3", ".ogg" })
            if (Name.size() > E.size() && Name.compare(Name.size() - E.size(), E.size(), E) == 0) Name.resize(Name.size() - E.size());
        return Name;
    }

    // A sound dry run's work, in bytes: its sounds' stored bytes, and DrySoundEach a sound (the
    // fixed cost of reading one object from the packages: 1.3 ms a sound against 10 ms a MB on
    // Cold War's CASC, 2026-11-11). A zone whose work is over DrySoundCap is not weighed: a count
    // of the zone's own data, so the same install gives the same index whatever the machine's load
    // (a time budget did not: a zone near it was weighed in one build and not in the next).
    // GRAYWOLF_INDEX_SOUND_CAP_MB overrides the cap (0: none), for measuring.
    constexpr uint64_t DrySoundEach = 128ull << 10;
    uint64_t DrySoundCap()
    {
        // (measured on Cold War, 2026-11-11, before its sounds came from their headers: 9 of its 540 zones over 384 MB,
        // fr_core_common 10.5 GB of work (73 651 sounds) and 107 s alone; the 531 others 86 s of dry runs)
        constexpr uint64_t Default = 384ull << 20;
        static const uint64_t Cap = [] {
            char Mb[32] = {};
            if (GetEnvironmentVariableA("GRAYWOLF_INDEX_SOUND_CAP_MB", Mb, sizeof(Mb)) == 0) return Default;
            const uint64_t V = std::strtoull(Mb, nullptr, 10);
            return V == 0 ? ~0ull : V << 20;
        }();
        return Cap;
    }

    // Its sounds with their weights, from a dry run of its sound kind that writes nothing: the
    // headers its reader tells (Context::OnSoundHeader), else the sounds it hands WriteSound (their
    // samples read to know them) while their work, counted as they come, is within DrySoundCap,
    // else none. A sound Out lists already gets the weight heard for it; the others are added.
    void WeighSounds(const IDiskZone& Zone, std::vector<std::pair<AssetId, Weight>>& Out, ZoneTimes* Times = nullptr)
    {
        const uint64_t Cap = DrySoundCap();
        if (!Zone.Supported().count(Kind::Sound)) return;
        if (Times) Times->SoundsRan = true;
        std::atomic<bool> Stop{ false };
        Context C;
        std::error_code Ec;
        C.OutRoot = (fs::temp_directory_path(Ec) / "greyhound_index" / ("weigh_" + std::to_string(GetCurrentProcessId()) + "_" +
                                                                         std::to_string(std::hash<std::thread::id>()(std::this_thread::get_id())))).string();
        C.Kinds = { Kind::Sound };
        C.PullDependencies = false;
        C.Cancel = &Stop;
        std::vector<std::pair<AssetId, Weight>> Heard;
        C.OnSoundHeader = [&](const SoundHeader& H) { Heard.push_back({ { Kind::Sound, H.Name }, Sizes::SoundWeight(H) }); };
        const fs::path SoundDir = fs::path(C.OutRoot) / KindFolder(Kind::Sound);
        Totals T;
        size_t Handed = 0;
        uint64_t Bytes = 0;
        {
            const ExportWriters::DrySounds Dry([&](const ExportWriters::SoundSource& S, const fs::path& Path) {
                if (Stop.load()) return;
                Handed++;
                Bytes += S.Size;
                if (Bytes + Handed * DrySoundEach > Cap)
                {
                    Stop.store(true);
                    return;
                }
                const std::string Name = Path.lexically_relative(SoundDir).replace_extension().string();
                Heard.push_back({ { Kind::Sound, Name }, Sizes::SoundWeight(Sizes::HeaderOf(S, Name)) });
            });
            const auto Asked = Clock::now();
            const std::lock_guard<std::mutex> G(PackageLock());
            if (Times) Times->SoundWait = Since(Asked);
            Zone.Export(Kind::Sound, C, T);
        }
        fs::remove_all(C.OutRoot, Ec);
        if (Times)
        {
            Times->SoundsStopped = Stop.load();
            Times->SoundsHanded = Handed;
            Times->SoundBytes = Bytes;
        }
        if (Stop.load()) return;
        // (a name listed more than once, each)
        std::unordered_multimap<std::string, size_t> Listed;
        for (size_t I = 0; I < Out.size(); I++)
            if (Out[I].first.K == Kind::Sound) Listed.emplace(SoundKey(Out[I].first.Name), I);
        for (auto& [A, W] : Heard)
        {
            const auto [From, To] = Listed.equal_range(SoundKey(A.Name));
            for (auto It = From; It != To; ++It) Out[It->second].second = W;
            if (From == To) Out.push_back({ std::move(A), W });
        }
    }

    // Whether File is on a drive that seeks (a hard disk): zones loaded side by side from one make it
    // seek between them, slower than one at a time (Infinite Warfare on a hard disk, 2026-11-11: its
    // loads 515 s summed on 1 worker, 4 978 s on 8 workers; the build 953 s against 1 123 s)
    bool OnSpinningDisk(const std::string& File)
    {
        wchar_t Volume[MAX_PATH] = {};
        if (!GetVolumePathNameW(fs::absolute(File).wstring().c_str(), Volume, MAX_PATH)) return false;
        std::wstring Device = Volume;
        if (Device.size() < 2 || Device[1] != L':') return false;
        Device = L"\\\\.\\" + Device.substr(0, 2);
        const HANDLE H = CreateFileW(Device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (H == INVALID_HANDLE_VALUE) return false;
        STORAGE_PROPERTY_QUERY Query{};
        Query.PropertyId = StorageDeviceSeekPenaltyProperty;
        Query.QueryType = PropertyStandardQuery;
        DEVICE_SEEK_PENALTY_DESCRIPTOR Seek{};
        DWORD Got = 0;
        const bool Asked = DeviceIoControl(H, IOCTL_STORAGE_QUERY_PROPERTY, &Query, sizeof(Query), &Seek, sizeof(Seek), &Got, nullptr) && Got >= sizeof(Seek);
        CloseHandle(H);
        return Asked && Seek.IncursSeekPenalty;
    }

    // The part a zone's entry carries while it waits to be read again (IDiskZone::IndexAgain)
    constexpr const char* AgainPart = "#index_again";
    bool WaitsAgain(const wolff::IndexedZone& E)
    {
        return std::any_of(E.Parts.begin(), E.Parts.end(), [](const auto& P) { return P.first == AgainPart; });
    }

    // The zone opened alone: what it defines and holds (nothing when it does not open); its bytes
    // handed to the string cipher's rebuild (Harvest) while open, as the zone Name
    wolff::IndexedZone ReadZone(const IDiskGame& Game, const ExportOptions& Options, const std::string& Path, CipherHarvest* Harvest = nullptr,
                                const std::string& Name = std::string(), ZoneTimes* Times = nullptr, IDiskGame::IndexRide* Ride = nullptr)
    {
        wolff::IndexedZone E;
        E.Done = true;
        std::string Why;
        auto At = Clock::now();
        const auto Lap = [&](double ZoneTimes::*Field) {
            const auto Now = Clock::now();
            if (Times) Times->*Field += Since(At, Now);
            At = Now;
        };
        auto Zone = Game.Open(Path, Options, Why);
        Lap(&ZoneTimes::Open);
        if (!Zone) return E;
        if (Times) Zone->OpenTimes(Times->Steps);
        if (Harvest) Harvest->Add(Name, Zone->Decoded());
        if (Ride) Ride->Opened(*Zone);
        Lap(&ZoneTimes::Harvest);
        std::vector<std::pair<AssetId, Weight>> Weighed;
        // (WeighAll, timed)
        Zone->Weighs(Weighed);
        Lap(&ZoneTimes::Weigh);
        if (Game.IndexWeighsSounds() &&
            std::none_of(Weighed.begin(), Weighed.end(), [](const auto& A) { return A.first.K == Kind::Sound && A.second.A != 0; }))
            WeighSounds(*Zone, Weighed, Times);
        Lap(&ZoneTimes::Sounds);
        for (auto& [A, W] : Weighed)
            if (!wolff::InstallIndex::TextName(A.Name))
            {
                const std::string Name = wolff::InstallIndex::FallbackName(KindName(A.K), A.Name);
                E.Renamed.push_back(fs::path(Path).filename().string() + ": " + KindName(A.K) + " " + wolff::InstallIndex::Escaped(A.Name) + " -> " + Name);
                A.Name = Name;
            }
        // once each, the heaviest of a name listed twice
        std::sort(Weighed.begin(), Weighed.end(), [](const auto& X, const auto& Y) {
            return X.first < Y.first || (!(Y.first < X.first) && X.second.Bytes > Y.second.Bytes);
        });
        for (auto& [A, W] : Weighed)
            if (wolff::IndexedAsset Id{ (uint32_t)A.K, std::move(A.Name) }; E.Defined.empty() || E.Defined.back() < Id)
            {
                E.Defined.push_back(std::move(Id));
                E.Weights.push_back(W);
            }
        E.Size = Zone->Footprint();
        Zone->Parts(E.Parts);
        // (marked in what the partial file keeps: a resumed build reads it again too)
        if (Zone->IndexAgain()) E.Parts.emplace_back(AgainPart, 0);
        std::sort(E.Parts.begin(), E.Parts.end());
        E.Parts.erase(std::unique(E.Parts.begin(), E.Parts.end()), E.Parts.end());
        Lap(&ZoneTimes::Parts);
        Zone.reset();
        Lap(&ZoneTimes::Close);
        if (Times)
        {
            if (Times->SoundsRan) Times->Sounds -= Times->SoundWait;
            Times->Footprint = E.Size;
            Times->Assets = E.Defined.size();
        }
        return E;
    }

    // The build's report: each stage summed over the zones (and the reader's own open steps), the
    // workers' use of the build's wall time (busy, waiting for the package lock, for admission, for
    // the session lock), the slowest zones with their stages
    std::string TimesReport(const std::vector<std::string>& Names, const std::vector<ZoneTimes>& Times, unsigned Threads, double Wall,
                            const std::vector<std::pair<std::string, double>>& Around, const std::function<void(const std::string&)>& Log)
    {
        std::string Out;
        const auto Fixed = [](double V, int Digits = 1) {
            char B[64];
            snprintf(B, sizeof(B), "%.*f", Digits, V);
            return std::string(B);
        };
        const auto Line = [&](const std::string& Text) {
            Out += "log " + Text + "\n";
            if (Log) Log(Text);
        };
        std::vector<std::pair<std::string, double>> Stages = { { "resolve_wait", 0 }, { "resolve", 0 }, { "admit_wait", 0 }, { "open", 0 },
                                                               { "harvest", 0 }, { "weigh", 0 }, { "sound_wait", 0 }, { "sounds", 0 },
                                                               { "parts", 0 }, { "close", 0 } };
        std::vector<std::pair<std::string, double>> Steps;   // the readers' open steps, in their first order
        size_t Count = 0, SoundsRan = 0, SoundsStopped = 0;
        uint64_t Files = 0;
        double SessionWait = 0, DiskWait = 0;
        std::vector<double> Busy(Threads ? Threads : 1);
        for (const ZoneTimes& T : Times)
        {
            if (!T.Read) continue;
            Count++;
            Files += T.File;
            SoundsRan += T.SoundsRan;
            SoundsStopped += T.SoundsStopped;
            const double V[] = { T.ResolveWait, T.Resolve, T.Admit, T.Open, T.Harvest, T.Weigh, T.SoundWait, T.Sounds, T.Parts, T.Close };
            for (size_t I = 0; I < Stages.size(); I++) Stages[I].second += V[I];
            for (const auto& [S, D] : T.Steps)
            {
                const auto It = std::find_if(Steps.begin(), Steps.end(), [&](const auto& P) { return P.first == S; });
                if (It == Steps.end()) Steps.emplace_back(S, D);
                else It->second += D;
                if (S == "session_wait") SessionWait += D;
                if (S == "disk_wait") DiskWait += D;
            }
            if (T.Worker < Busy.size()) Busy[T.Worker] += Since(T.Start, T.End);
        }
        Out += "index_threads=" + std::to_string(Threads) + "\nindex_zones_read=" + std::to_string(Count) + "\nindex_file_mb=" + std::to_string(Files >> 20) +
               "\nindex_seconds_zones=" + Fixed(Wall, 3) + "\n";
        for (const auto& [S, D] : Around) Out += "index_seconds_" + S + "=" + Fixed(D, 3) + "\n";
        for (const auto& [S, D] : Stages) Out += "index_stage_" + S + "_s=" + Fixed(D, 3) + "\n";
        for (const auto& [S, D] : Steps) Out += "index_open_" + S + "_s=" + Fixed(D, 3) + "\n";
        Out += "index_sounds_dry_runs=" + std::to_string(SoundsRan) + "\nindex_sounds_dry_stopped=" + std::to_string(SoundsStopped) + "\n";
        // the workers: time inside a zone (resolve to close) against the zones' wall time
        double InZone = 0;
        for (double B : Busy) InZone += B;
        const double Capacity = Wall * (double)Busy.size();
        const double Waits = Stages[0].second + Stages[2].second + Stages[6].second + SessionWait + DiskWait;
        const auto Pct = [&](double V) { return Capacity > 0 ? Fixed(100.0 * V / Capacity) + " %" : std::string("-"); };
        Out += "index_workers_in_zone_pct=" + (Capacity > 0 ? Fixed(100.0 * InZone / Capacity) : std::string("0")) + "\n";
        Out += "index_workers_working_pct=" + (Capacity > 0 ? Fixed(100.0 * (InZone - Waits) / Capacity) : std::string("0")) + "\n";
        std::string Around2;
        for (const auto& [S, D] : Around) Around2 += ", " + S + " " + Fixed(D) + " s";
        Line("install index build: " + std::to_string(Count) + " zones (" + std::to_string(Files >> 20) + " MB of files) in " + Fixed(Wall) +
             " s on " + std::to_string(Busy.size()) + " workers" + Around2);
        std::string S1 = "install index stages (summed over the zones):";
        for (const auto& [S, D] : Stages) S1 += " " + S + " " + Fixed(D) + " s,";
        S1.pop_back();
        Line(S1);
        std::string S2 = "install index open steps (summed):";
        for (const auto& [S, D] : Steps) S2 += " " + S + " " + Fixed(D) + " s,";
        if (!Steps.empty()) S2.pop_back();
        Line(S2);
        Line("install index workers: in a zone " + Pct(InZone) + " of " + Fixed(Capacity) + " worker-seconds, working " + Pct(InZone - Waits) +
             " (waiting: package lock " + Pct(Stages[0].second + Stages[6].second) + ", admission " + Pct(Stages[2].second) + ", session lock " +
             Pct(SessionWait) + ", hard disk " + Pct(DiskWait) + "); sound dry runs " + std::to_string(SoundsRan) + " (" + std::to_string(SoundsStopped) + " stopped at their cap of " + std::to_string(DrySoundCap() >> 20) + " MB)");
        // the slowest
        std::vector<size_t> Order;
        for (size_t Z = 0; Z < Times.size(); Z++)
            if (Times[Z].Read) Order.push_back(Z);
        const auto Total = [&](size_t Z) { return Since(Times[Z].Start, Times[Z].End); };
        std::sort(Order.begin(), Order.end(), [&](size_t A, size_t B) { return Total(A) > Total(B); });
        if (Order.size() > 20) Order.resize(20);
        for (size_t Z : Order)
        {
            const ZoneTimes& T = Times[Z];
            std::string L = "install index slow zone " + Names[Z] + ": " + Fixed(Total(Z), 2) + " s, file " + Fixed((double)T.File / 1048576.0) +
                            " MB, footprint " + Fixed((double)T.Footprint / 1048576.0) + " MB, " + std::to_string(T.Assets) + " assets;";
            const double V[] = { T.ResolveWait, T.Resolve, T.Admit, T.Open, T.Harvest, T.Weigh, T.SoundWait, T.Sounds, T.Parts, T.Close };
            for (size_t I = 0; I < Stages.size(); I++)
                if (V[I] >= 0.0005) L += " " + Stages[I].first + " " + Fixed(V[I], 3);
            L += " | open:";
            for (const auto& [S, D] : T.Steps)
                if (D >= 0.0005) L += " " + S + " " + Fixed(D, 3);
            Line(L);
        }
        return Out;
    }
}

std::mutex& IndexPackageLock()
{
    static std::mutex Lock;
    return Lock;
}

namespace
{
    std::mutex DiskLock;
    std::atomic<int> HardDiskBuilds{ 0 };   // the builds reading from a hard disk now
}

std::mutex* IndexDiskLock()
{
    return HardDiskBuilds.load() > 0 ? &DiskLock : nullptr;
}

void WeighAll(const IDiskZone& Zone, std::vector<std::pair<AssetId, Weight>>& Out, bool Sounds)
{
    Zone.Weighs(Out);
    // (a reader that lists its sounds with their shape needs no dry run)
    if (Sounds && std::none_of(Out.begin(), Out.end(), [](const auto& A) { return A.first.K == Kind::Sound && A.second.A != 0; })) WeighSounds(Zone, Out);
}

std::string KeptFile(const IDiskGame& Game, const ExportOptions& Options, const std::string& Install, const char* Extension)
{
    const fs::path Dir = Options.Cache.empty() ? fs::temp_directory_path() / "greyhound_index" : fs::path(Options.Cache) / "index";
    std::error_code Ec;
    fs::create_directories(Dir, Ec);
    return (Dir / (std::string(Game.Id()) + "_" + Hex16(Fnv1a(Lower(fs::absolute(Install).string()))) + Extension)).string();
}

std::vector<std::string> KeptInstalls(const IDiskGame& Game, const ExportOptions& Options)
{
    std::vector<std::string> Out;
    const fs::path Dir = fs::path(KeptFile(Game, Options, ".", ".install")).parent_path();
    const std::string Prefix = Lower(std::string(Game.Id()) + "_");
    std::error_code Ec;
    for (auto It = fs::directory_iterator(Dir, Ec); !Ec && It != fs::directory_iterator(); It.increment(Ec))
    {
        if (Lower(It->path().filename().string()).rfind(Prefix, 0) != 0 || It->path().extension() != ".install") continue;
        std::ifstream In(It->path(), std::ios::binary);
        std::string Install;
        std::getline(In, Install);
        if (!Install.empty() && Install.back() == '\r') Install.pop_back();
        // its own (the file's name holds the folder's hash), and still there
        std::error_code Ec2;
        if (!Install.empty() && Lower(KeptFile(Game, Options, Install, ".install")) == Lower(It->path().string()) && fs::is_directory(Install, Ec2))
            Out.push_back(Install);
    }
    return Out;
}

wolff::TypeList ZoneIndex::Types()
{
    wolff::TypeList Out;
    for (const KindInfo& I : Kinds) Out.emplace_back(I.Name);
    return Out;
}

void ZoneIndex::Took()
{
    Assets.assign(Ix.Zones().size(), {});
    AssetWeights.assign(Ix.Zones().size(), {});
    Entries = Ix.Entries();
    Weighed = Ix.Weighed();
}

const std::vector<AssetId>& ZoneIndex::Defined(size_t Zone) const
{
    std::lock_guard<std::mutex> G(*Lock);
    std::vector<AssetId>& Out = Assets[Zone];
    if (!Out.empty() || Zone >= Ix.Zones().size()) return Out;
    std::vector<wolff::IndexedAsset> Read;
    std::vector<Weight> Ws;
    Ix.Read(Zone, Read, &Ws);
    std::vector<Weight>& Kept = AssetWeights[Zone];
    Kept.clear();
    for (size_t I = 0; I < Read.size(); I++)
    {
        if (Read[I].Type >= std::size(Kinds)) continue;   // (a kind this build does not know)
        Out.push_back({ (Kind)Read[I].Type, std::move(Read[I].Name) });
        Kept.push_back(I < Ws.size() ? Ws[I] : Weight());
    }
    return Out;
}

const std::vector<Weight>& ZoneIndex::Weights(size_t Zone) const
{
    Defined(Zone);   // read with the assets
    return AssetWeights[Zone];
}

std::vector<size_t> ZoneIndex::DefinedIn(const AssetId& A) const
{
    return Ix.DefinedIn((uint32_t)A.K, A.Name);
}

std::vector<size_t> ZoneIndex::PartIn(const std::string& Type, uint64_t Hash) const
{
    return Ix.PartIn(Type, Hash);
}

std::unique_ptr<ZoneIndex> ZoneIndex::Kept(const IDiskGame& Game, const ExportOptions& Options, const std::string& Install)
{
    const auto Start = std::chrono::steady_clock::now();
    std::vector<std::string> Names;
    std::string Error;
    if (!Game.IndexedZones(Install, Options, Names, Error)) return nullptr;
    auto Out = std::unique_ptr<ZoneIndex>(new ZoneIndex());
    Out->File = KeptFile(Game, Options, Install, ".gwi");
    if (!Out->Ix.LoadMapped(Out->File, std::string()) || Out->Ix.Zones() != Names)
    {
        Out->File = KeptFile(Game, Options, Install, ".idx");
        if (!Out->Ix.LoadText(Out->File, std::string(), TextFormat) || Out->Ix.Zones() != Names) return nullptr;
    }
    Out->Took();
    Out->Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
    return Out;
}

std::unique_ptr<ZoneIndex> ZoneIndex::Build(const IDiskGame& Game, const ExportOptions& Options, const std::string& Install,
                                            const std::function<void(size_t, size_t)>& Step, const std::atomic<bool>* Cancel,
                                            std::string& Error)
{
    const auto Start = Clock::now();
    std::vector<std::string> Names;
    if (!Game.IndexedZones(Install, Options, Names, Error)) return nullptr;
    const double Listed = Since(Start);
    Siren::AddSearchDir(fs::path(Install).wstring());   // the game's own Oodle, when none is beside the exe
    // the reader's key, as Export takes it: from the first zone that opens
    std::string ReaderKey;
    bool Opened = false;
    std::string FirstWhy;
    for (const auto& N : Names)
    {
        std::vector<std::string> Paths;
        std::string Why;
        if (!Game.ResolveZones(Install, { N }, Options, Paths, Why) || Paths.empty()) continue;
        if (const auto Zone = Game.Open(Paths.front(), Options, Why))
        {
            ReaderKey = Zone->IndexKey();
            Opened = true;
            break;
        }
        if (FirstWhy.empty()) FirstWhy = N + ": " + Why;
        if (Cancel != nullptr && Cancel->load()) break;
    }
    // no zone of the install opens (a missing Oodle DLL): said, not an index with nothing in it
    if (!Opened && !FirstWhy.empty() && !(Cancel != nullptr && Cancel->load()))
    {
        Error = "no zone opened (" + FirstWhy + ")";
        return nullptr;
    }
    const double Keyed = Since(Start) - Listed;
    auto Out = For(Game, Options, Install, ReaderKey, Step, Cancel, Error, false);
    if (Out && Out->Built)
        Out->BuildTimes = "index_seconds_list=" + std::to_string(Listed) + "\nindex_seconds_key=" + std::to_string(Keyed) + "\n" + Out->BuildTimes;
    return Out;
}

namespace
{
    std::mutex ReporterLock;
    std::shared_ptr<const ZoneIndex::Reporter> CurrentReporter;

    std::shared_ptr<const ZoneIndex::Reporter> Reporting()
    {
        const std::lock_guard<std::mutex> G(ReporterLock);
        return CurrentReporter;
    }

    // The install's zone files as they are now: each listed zone's size and write time, with its
    // update patch's (.fp, .fd) beside, and a CASC install's build file. A game update that
    // rewrites a zone changes it, and so the index's key: the index is made again. Kept a few
    // minutes per install (the readers ask for the index more than once a run; 16 000 files).
    std::string FilesStamp(const std::string& Install, const std::vector<std::string>& Names)
    {
        static std::mutex Lock;
        static std::map<std::string, std::pair<std::chrono::steady_clock::time_point, std::string>> Kept;
        std::string Of = Lower(fs::absolute(Install).string()) + "|" + std::to_string(Names.size());
        if (!Names.empty()) Of += "|" + Names.front() + "|" + Names.back();
        const auto Now = std::chrono::steady_clock::now();
        {
            const std::lock_guard<std::mutex> G(Lock);
            if (const auto It = Kept.find(Of); It != Kept.end() && Now - It->second.first < std::chrono::minutes(5)) return It->second.second;
        }
        std::string Text;
        std::error_code Ec;
        const auto Add = [&](const fs::path& P) {
            const auto Size = fs::file_size(P, Ec);
            if (Ec) return false;
            const auto When = fs::last_write_time(P, Ec);
            if (Ec) return false;
            Text += P.filename().string() + " " + std::to_string(Size) + " " + std::to_string(When.time_since_epoch().count()) + "\n";
            return true;
        };
        const fs::path Root(Install);
        for (const auto& N : Names)
            for (fs::path P : { Root / N, Root / (N + ".ff") })
                if (fs::is_regular_file(P, Ec) && Add(P))
                {
                    for (const char* Patch : { ".fp", ".fd" }) Add(fs::path(P).replace_extension(Patch));
                    break;
                }
        for (const char* Build : { ".build.info", ".product.db" }) Add(Root / Build);
        const std::string Stamp = Hex16(Fnv1a(Text));
        const std::lock_guard<std::mutex> G(Lock);
        Kept[Of] = { Now, Stamp };
        return Stamp;
    }
}

std::string ZoneFilesStamp(const std::string& Install, const std::vector<std::string>& Names)
{
    return FilesStamp(Install, Names);
}

ZoneIndex::ReporterScope::ReporterScope(Reporter R)
{
    const std::lock_guard<std::mutex> G(ReporterLock);
    CurrentReporter = std::make_shared<const Reporter>(std::move(R));
}

ZoneIndex::ReporterScope::~ReporterScope()
{
    const std::lock_guard<std::mutex> G(ReporterLock);
    CurrentReporter.reset();
}

std::unique_ptr<ZoneIndex> ZoneIndex::For(const IDiskGame& Game, const ExportOptions& Options, const std::string& Install,
                                          const std::string& ReaderKey, const std::function<void(size_t, size_t)>& Step,
                                          const std::atomic<bool>* Cancel, std::string& Error, bool AcceptUnweighed)
{
    const auto Start = std::chrono::steady_clock::now();
    auto Out = std::unique_ptr<ZoneIndex>(new ZoneIndex());
    std::vector<std::string> Names;
    if (!Game.IndexedZones(Install, Options, Names, Error)) return nullptr;
    Siren::AddSearchDir(fs::path(Install).wstring());
    const std::string Stamp = FilesStamp(Install, Names);
    const auto KeyOf = [&](int Version) {
        std::string KeyText = "format " + std::to_string(Version) + "\n" + Game.Id() + "\n" + Lower(fs::absolute(Install).string()) +
                              "\n" + ReaderKey + "\nfiles " + Stamp + "\n";
        for (const auto& N : Names) KeyText += N + "\n";
        return Hex16(Fnv1a(KeyText));
    };
    const std::string Key = KeyOf(Format);
    const std::string File = KeptFile(Game, Options, Install, ".gwi");
    const std::string TextFile = KeptFile(Game, Options, Install, ".idx");   // the text index of before
    Out->File = File;

    const auto Done = [&] {
        Out->Took();
        Out->Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
        // the install's folder beside its index (KeptInstalls): an install no detection finds is found again
        const std::string Beside = KeptFile(Game, Options, Install, ".install");
        std::error_code Ec;
        if (!fs::exists(Beside, Ec)) std::ofstream(Beside, std::ios::binary) << fs::absolute(Install).string() << "\n";
        return std::move(Out);
    };
    wolff::InstallIndex& Ix = Out->Ix;

    // the kept file, when its key is this one: read in place; one kept before weights: what an
    // export asks of it, until the install is indexed again (Build: the weights of the size estimate)
    if (Ix.LoadMapped(File, Key) && Ix.Zones().size() == Names.size()) return Done();
    if (AcceptUnweighed && Ix.LoadMapped(File, KeyOf(Unweighed)) && Ix.Zones().size() == Names.size()) return Done();
    // a text index of before, read and written as one (then read in place, as any later run reads it)
    if (Ix.LoadText(TextFile, Key, TextFormat) && Ix.Zones().size() == Names.size())
    {
        if (Ix.SaveMapped(File, Key)) Ix.LoadMapped(File, Key);
        return Done();
    }

    // built: every zone opened by a pool of workers (wolff::BuildZones), each result kept per zone
    // and added in zone order, so the file is the one a serial build writes. Said in the run's log
    // first: made once per install, again only when the game's files (or GrayWolf's readers) change
    Out->Built = true;
    std::error_code Had;
    const bool Stale = fs::exists(File, Had) || fs::exists(TextFile, Had);
    const auto Report = Reporting();
    if (Report && Report->Log)
        Report->Log(Stale ? "install index out of date (the game's files changed since it was made, or GrayWolf's readers did): "
                            "made again, once, " + std::to_string(Names.size()) + " zones"
                          : "making the install index, once for this install: " + std::to_string(Names.size()) +
                            " zones; later exports read it, made again only when the game's files change");
    const auto& Stepping = Step || !Report ? Step : Report->Step;
    const std::atomic<bool>* Stopping = Cancel || !Report ? Cancel : Report->Cancel;
    // stepped through the run's reporter: told when the build ends, whatever way (its phase cleared)
    struct Ending
    {
        const Reporter* R;
        ~Ending()
        {
            if (R && R->Finished) R->Finished();
        }
    } End{ !Step && Report ? Report.get() : nullptr };
    ExportOptions Indexing = Options;
    Indexing.Indexing = true;
    wolff::IndexBuild B;
    B.Zones = Names;
    B.Types = Types();
    B.Key = Key;
    B.Format = Format;
    B.PartialFile = File + ".partial";
    // (ResolveZones by one worker at a time: it may extract the zone from a package)
    // each zone's stages (TimesReport), the workers numbered as they first take one
    std::vector<ZoneTimes> Times(Names.size());
    std::atomic<unsigned> Workers{ 0 };
    const auto WorkerId = [&] {
        thread_local const void* Of = nullptr;
        thread_local unsigned Id = 0;
        if (Of != &Workers)
        {
            Of = &Workers;
            Id = Workers++;
        }
        return Id;
    };
    B.Resolve = [&](size_t Z, std::string& Path) {
        ZoneTimes& T = Times[Z];
        T.Read = true;
        T.Worker = WorkerId();
        T.Start = Clock::now();
        std::vector<std::string> Paths;
        std::string Why;
        const std::lock_guard<std::mutex> G(PackageLock());
        const auto Locked = Clock::now();
        T.ResolveWait = Since(T.Start, Locked);
        const bool Ok = Game.ResolveZones(Install, { Names[Z] }, Options, Paths, Why) && !Paths.empty();
        T.End = Clock::now();
        T.Resolve = Since(Locked, T.End);
        if (!Ok) return false;
        Path = Paths.front();
        std::error_code Ec;
        T.File = fs::file_size(Path, Ec);
        return true;
    };
    // the string cipher's rebuild (DiskGame.Cipher.cpp) riding this build when the install has no
    // table kept: each zone's strings taken while the index has it open, the rest read after
    std::map<std::string, size_t> CipherCounters;
    auto Harvest = CipherHarvest::Begin(Game, Options, Install, &CipherCounters);
    // what the game gathers from the zones besides (IndexRides)
    auto Ride = Game.IndexRides(Install, Indexing);
    B.Read = [&](size_t Z, const std::string& Path) {
        ZoneTimes& T = Times[Z];
        T.Admit = Since(T.End);
        auto E = ReadZone(Game, Indexing, Path, Harvest.get(), Names[Z], &T, Ride.get());
        T.End = Clock::now();
        return E;
    };
    B.Step = Stepping;
    B.Cancel = Stopping;
    // 8 workers by default and at most, 1 for a game whose IndexInParallel is false; a budget of
    // GRAYWOLF_INDEX_BUDGET_MB (4096 by default). Zones read from a hard disk (OnSpinningDisk) are
    // loaded one at a time (IndexDiskLock), the rest of their reading side by side.
    B.Threads = Game.IndexInParallel() ? (Options.Threads ? Options.Threads : 8) : 1;
    struct HardDisk
    {
        bool On = false;
        ~HardDisk()
        {
            if (On) HardDiskBuilds--;
        }
    } Spinning;
    if (B.Threads > 1 && !Names.empty())
    {
        std::vector<std::string> First;
        std::string Why;
        const std::lock_guard<std::mutex> G(PackageLock());
        if (Game.ResolveZones(Install, { Names.front() }, Options, First, Why) && !First.empty() && OnSpinningDisk(First.front()))
        {
            Spinning.On = true;
            HardDiskBuilds++;
        }
    }
    if (char Mb[32] = {}; GetEnvironmentVariableA("GRAYWOLF_INDEX_BUDGET_MB", Mb, sizeof(Mb)) && std::strtoull(Mb, nullptr, 10) > 0)
        B.BudgetBytes = std::strtoull(Mb, nullptr, 10) << 20;
    std::vector<wolff::IndexedZone> Read;
    const auto Zones = Clock::now();
    const double Before = Since(Start, Zones);
    if (!wolff::BuildZones(B, Read))
    {
        Error = "cancelled";
        return nullptr;
    }
    // what the ride gathered complete: the zones that waited on it read again, one at a time
    if (Ride) Ride->Finished();
    size_t ReadAgain = 0;
    for (size_t Z = 0; Z < Read.size(); Z++)
    {
        if (!WaitsAgain(Read[Z])) continue;
        if (Stopping != nullptr && Stopping->load())
        {
            Error = "cancelled";
            return nullptr;
        }
        std::string Path;
        if (B.Resolve(Z, Path))
        {
            const double Admitted = Times[Z].Admit;
            Read[Z] = B.Read(Z, Path);
            Times[Z].Admit = Admitted;
        }
        Read[Z].Done = true;
        std::erase_if(Read[Z].Parts, [](const auto& P) { return P.first == AgainPart; });
        ReadAgain++;
    }
    const auto Built = Clock::now();
    // (GRAYWOLF_INDEX_DUMP=<file>: what each zone gave, as text, to compare two builds)
    if (char Dump[MAX_PATH] = {}; GetEnvironmentVariableA("GRAYWOLF_INDEX_DUMP", Dump, sizeof(Dump)))
    {
        std::ofstream D(Dump, std::ios::binary | std::ios::trunc);
        for (size_t Z = 0; Z < Read.size(); Z++)
        {
            const auto& E = Read[Z];
            D << "zone\t" << Names[Z] << "\t" << E.Size << (Times[Z].SoundsStopped ? "\tsounds_stopped" : "") << "\n";
            // (the dry run's work, apart: the measure a cap is chosen by)
            if (Times[Z].SoundsRan)
                D << "\tsounds_dry\t" << Times[Z].SoundsHanded << "\t" << Times[Z].SoundBytes << "\t" << Times[Z].Sounds << "\n";
            for (size_t I = 0; I < E.Defined.size(); I++)
            {
                const wolff::AssetWeight W = I < E.Weights.size() ? E.Weights[I] : wolff::AssetWeight();
                D << "\t" << E.Defined[I].Type << "\t" << wolff::InstallIndex::Escaped(E.Defined[I].Name) << "\t" << W.Bytes << "\t" << W.A << "\t"
                  << W.B << "\t" << W.C << "\n";
            }
            for (const auto& [T, H] : E.Parts) D << "\tpart\t" << T << "\t" << H << "\n";
        }
    }
    Ix.SetZones(Names);
    wolff::AddZones(Ix, Read, &Out->Renamed);
    Read.clear();
    const auto Added = Clock::now();
    // read back in place: what every later run reads, the build's memory let go
    if (Ix.SaveMapped(File, Key)) Ix.LoadMapped(File, Key);
    std::error_code Ec;
    fs::remove(File + ".partial", Ec);
    Out->BuildTimes = TimesReport(Names, Times, B.Threads, Since(Zones, Built),
                                  { { "before", Before }, { "add", Since(Built, Added) }, { "write", Since(Added) } },
                                  Report && Report->Log ? Report->Log : std::function<void(const std::string&)>());
    Out->BuildTimes += "index_zones_read_again=" + std::to_string(ReadAgain) + "\nindex_hard_disk=" + std::to_string(Spinning.On ? 1 : 0) + "\n";
    const auto Fitting = Clock::now();
    if (Harvest)
    {
        // (its phase on the run's progress: the zones the index does not open, then the fit)
        CipherProgress CP;
        CP.Log = [&](const std::string& Line) {
            Out->CipherLog += Line + "\n";
            if (Report && Report->Log) Report->Log(Line);
        };
        CP.Step = Stepping;
        CP.Cancel = Stopping;
        std::string Why;
        if (!Harvest->Finish(Game, Options, CP, CipherCounters, Why))
            CP.Log("string cipher not rebuilt: " + Why + "; encrypted strings are left out");
        Out->CipherCounters = std::move(CipherCounters);
    }
    Out->BuildTimes += "index_seconds_cipher=" + std::to_string(Since(Fitting)) + "\n";
    return Done();
}
}
