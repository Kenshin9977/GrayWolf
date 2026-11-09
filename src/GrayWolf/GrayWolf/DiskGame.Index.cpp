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

    // What a build's workers take one at a time: ResolveZones (it may extract a zone from a
    // package) and a sound kind's dry run (its reader may extract its banks from the same storage)
    std::mutex& PackageLock()
    {
        static std::mutex Lock;
        return Lock;
    }

    // A sound's name as a key: lower case, '/' between folders, without a sound file's extension
    std::string SoundKey(std::string Name)
    {
        Name = Lower(Name);
        std::replace(Name.begin(), Name.end(), '\\', '/');
        for (const std::string_view E : { ".wav", ".flac", ".mp3", ".ogg" })
            if (Name.size() > E.size() && Name.compare(Name.size() - E.size(), E.size(), E) == 0) Name.resize(Name.size() - E.size());
        return Name;
    }

    // Its sounds with their weights, from a dry run of its sound kind that writes nothing: the
    // headers its reader tells (Context::OnSoundHeader), else the sounds it hands WriteSound (their
    // samples read to know them) while that takes under DryBudget, else none. A sound Out lists
    // already gets the weight heard for it; the others are added.
    void WeighSounds(const IDiskZone& Zone, std::vector<std::pair<AssetId, Weight>>& Out)
    {
        constexpr double DryBudget = 2.0;
        if (!Zone.Supported().count(Kind::Sound)) return;
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
        const auto Start = std::chrono::steady_clock::now();
        Totals T;
        {
            const ExportWriters::DrySounds Dry([&](const ExportWriters::SoundSource& S, const fs::path& Path) {
                if (std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count() > DryBudget)
                {
                    Stop.store(true);
                    return;
                }
                const std::string Name = Path.lexically_relative(SoundDir).replace_extension().string();
                Heard.push_back({ { Kind::Sound, Name }, Sizes::SoundWeight(Sizes::HeaderOf(S, Name)) });
            });
            const std::lock_guard<std::mutex> G(PackageLock());
            Zone.Export(Kind::Sound, C, T);
        }
        fs::remove_all(C.OutRoot, Ec);
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

    // The zone opened alone: what it defines and holds (nothing when it does not open)
    wolff::IndexedZone ReadZone(const IDiskGame& Game, const ExportOptions& Options, const std::string& Path)
    {
        wolff::IndexedZone E;
        E.Done = true;
        std::string Why;
        const auto Zone = Game.Open(Path, Options, Why);
        if (!Zone) return E;
        std::vector<std::pair<AssetId, Weight>> Weighed;
        WeighAll(*Zone, Weighed, !Game.IndexInParallel());
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
        std::sort(E.Parts.begin(), E.Parts.end());
        E.Parts.erase(std::unique(E.Parts.begin(), E.Parts.end()), E.Parts.end());
        return E;
    }
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
    std::vector<std::string> Names;
    if (!Game.IndexedZones(Install, Options, Names, Error)) return nullptr;
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
    return For(Game, Options, Install, ReaderKey, Step, Cancel, Error, false);
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
    const auto KeyOf = [&](int Version) {
        std::string KeyText = "format " + std::to_string(Version) + "\n" + Game.Id() + "\n" + Lower(fs::absolute(Install).string()) +
                              "\n" + ReaderKey + "\n";
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
    // and added in zone order, so the file is the one a serial build writes
    Out->Built = true;
    ExportOptions Indexing = Options;
    Indexing.Indexing = true;
    wolff::IndexBuild B;
    B.Zones = Names;
    B.Types = Types();
    B.Key = Key;
    B.Format = Format;
    B.PartialFile = File + ".partial";
    // (ResolveZones by one worker at a time: it may extract the zone from a package)
    B.Resolve = [&](size_t Z, std::string& Path) {
        std::vector<std::string> Paths;
        std::string Why;
        const std::lock_guard<std::mutex> G(PackageLock());
        if (!Game.ResolveZones(Install, { Names[Z] }, Options, Paths, Why) || Paths.empty()) return false;
        Path = Paths.front();
        return true;
    };
    B.Read = [&](size_t, const std::string& Path) { return ReadZone(Game, Indexing, Path); };
    B.Step = Step;
    B.Cancel = Cancel;
    // 8 workers by default and at most, 1 for a game whose IndexInParallel is false; a budget of
    // GRAYWOLF_INDEX_BUDGET_MB (4096 by default)
    B.Threads = Game.IndexInParallel() ? (Options.Threads ? Options.Threads : 8) : 1;
    if (char Mb[32] = {}; GetEnvironmentVariableA("GRAYWOLF_INDEX_BUDGET_MB", Mb, sizeof(Mb)) && std::strtoull(Mb, nullptr, 10) > 0)
        B.BudgetBytes = std::strtoull(Mb, nullptr, 10) << 20;
    std::vector<wolff::IndexedZone> Read;
    if (!wolff::BuildZones(B, Read))
    {
        Error = "cancelled";
        return nullptr;
    }
    Ix.SetZones(Names);
    wolff::AddZones(Ix, Read, &Out->Renamed);
    Read.clear();
    // read back in place: what every later run reads, the build's memory let go
    if (Ix.SaveMapped(File, Key)) Ix.LoadMapped(File, Key);
    std::error_code Ec;
    fs::remove(File + ".partial", Ec);
    return Done();
}
}
