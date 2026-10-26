#include "stdafx.h"

// DiskGame::ZoneIndex: where each asset of an install is defined. One text file per game and
// install under <--cache>/index (or %TEMP%/greyhound_index): a header with the key, then a
// "zone" line per zone (its footprint and name) followed by one "<kind>\t<name>" line per asset
// it defines, then one "part\t<type>\t<hash>" line per part it holds (IDiskZone::Parts). Built by a pool of workers (BuildZones), resumable from
// a partial file beside it. The key hashes the format, the game, the install, the zone list and the reader's
// IndexKey (its tables): any change rebuilds the file.

#include "DiskGame.h"
#include "ExportNaming.h"
#include "Siren.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string_view>
#include <thread>

namespace fs = std::filesystem;

namespace DiskGame
{
namespace
{
    // bumped when what a reader's Defines lists changes (its tables are in the key already)
    constexpr int Format = 2;

    using ExportNaming::Fnv1a;
    using ExportNaming::Hex16;
    using ExportNaming::Lower;

    const KindInfo* KindNamed(const std::string& Name)
    {
        for (const KindInfo& I : Kinds)
            if (Name == I.Name) return &I;
        return nullptr;
    }

    // One zone's part of the index, as a build reads it (or a resumed build's partial file kept it)
    struct ZoneEntry
    {
        bool Done = false;
        uint64_t Size = 0;   // its footprint, 0 when it does not open
        std::vector<AssetId> Defined;   // sorted, once each
        std::vector<std::pair<std::string, uint64_t>> Parts;   // sorted, once each
        std::vector<std::string> Renamed;   // ZoneIndex::Renamed's lines for this zone
    };

    // Whether a name is text: UTF-8 without control bytes. A reader whose name pointer resolved
    // into other data (a layout off in a block, MW2019's common_mp before its equipsndtable
    // entries) gives bytes, a tab or a line break among them, which would cut the index's lines.
    bool TextName(const std::string& N)
    {
        for (size_t I = 0; I < N.size();)
        {
            const uint8_t C = (uint8_t)N[I];
            if (C < 0x20 || C == 0x7f) return false;
            const size_t Len = C < 0x80 ? 1 : (C & 0xe0) == 0xc0 ? 2 : (C & 0xf0) == 0xe0 ? 3 : (C & 0xf8) == 0xf0 ? 4 : 0;
            if (Len == 0 || I + Len > N.size()) return false;
            for (size_t K = 1; K < Len; K++)
                if (((uint8_t)N[I + K] & 0xc0) != 0x80) return false;
            I += Len;
        }
        return true;
    }

    // What the index keeps instead of a name that is not text: "#<kind>_<FNV-1a of its bytes>",
    // the same from one build to the next
    std::string FallbackName(Kind K, const std::string& N)
    {
        return "#" + std::string(KindName(K)) + "_" + Hex16(Fnv1a(N));
    }

    // A name's bytes for a report line: printable ASCII as it is, the rest \xNN, the first 48
    std::string Escaped(const std::string& N)
    {
        static const char* Digits = "0123456789abcdef";
        std::string Out;
        for (size_t I = 0; I < N.size() && I < 48; I++)
        {
            const uint8_t C = (uint8_t)N[I];
            if (C >= 0x20 && C < 0x7f && C != '\\') Out += (char)C;
            else Out += std::string("\\x") + Digits[C >> 4] + Digits[C & 15];
        }
        return N.size() > 48 ? Out + "..." : Out;
    }

    // The zone opened alone: what it defines and holds (nothing when it does not open)
    ZoneEntry ReadZone(const IDiskGame& Game, const ExportOptions& Options, const std::string& Path)
    {
        ZoneEntry E;
        E.Done = true;
        std::string Why;
        const auto Zone = Game.Open(Path, Options, Why);
        if (!Zone) return E;
        Zone->Defines(E.Defined);
        for (auto& A : E.Defined)
            if (!TextName(A.Name))
            {
                const std::string Name = FallbackName(A.K, A.Name);
                E.Renamed.push_back(fs::path(Path).filename().string() + ": " + KindName(A.K) + " " + Escaped(A.Name) + " -> " + Name);
                A.Name = Name;
            }
        std::sort(E.Defined.begin(), E.Defined.end());
        E.Defined.erase(std::unique(E.Defined.begin(), E.Defined.end(),
                                    [](const AssetId& A, const AssetId& B) { return !(A < B) && !(B < A); }),
                        E.Defined.end());
        E.Size = Zone->Footprint();
        Zone->Parts(E.Parts);
        std::sort(E.Parts.begin(), E.Parts.end());
        E.Parts.erase(std::unique(E.Parts.begin(), E.Parts.end()), E.Parts.end());
        return E;
    }

    // A build's partial file: each zone read so far, in the order they finished, so a build that
    // stopped (killed, cancelled) resumes where it was. "zone <n> <size> <name>", its asset and
    // part lines as in the index, then "end"; a record without its end is read again.
    const std::string PartialMagic = "greyhound-zone-index-partial ";

    void WriteEntry(std::ofstream& Out, size_t Z, const std::string& Name, const ZoneEntry& E)
    {
        Out << "zone\t" << Z << "\t" << E.Size << "\t" << Name << "\n";
        for (const auto& A : E.Defined) Out << KindName(A.K) << "\t" << A.Name << "\n";
        for (const auto& [Type, Hash] : E.Parts) Out << "part\t" << Type << "\t" << std::hex << Hash << std::dec << "\n";
        Out << "end\n";
        Out.flush();
    }

    // The complete records of a partial file with this key, then the file rewritten with them only
    // (a torn last record dropped) and left open to append to
    void ResumePartial(const std::string& Path, const std::string& Key, const std::vector<std::string>& Names,
                       std::vector<ZoneEntry>& Read, std::ofstream& Out)
    {
        {
            std::ifstream In(Path, std::ios::binary);
            std::string Line;
            if (In && std::getline(In, Line) && Line == PartialMagic + std::to_string(Format) && std::getline(In, Line) && Line == "key " + Key)
            {
                size_t Z = SIZE_MAX;
                ZoneEntry E;
                while (std::getline(In, Line))
                {
                    const size_t Tab = Line.find('\t');
                    if (Line == "end")
                    {
                        if (Z < Read.size())
                        {
                            E.Done = true;
                            Read[Z] = std::move(E);
                        }
                        Z = SIZE_MAX;
                        E = ZoneEntry();
                    }
                    else if (Tab == std::string::npos) Z = SIZE_MAX;
                    else if (Line.compare(0, Tab, "zone") == 0)
                    {
                        E = ZoneEntry();
                        const size_t Tab2 = Line.find('\t', Tab + 1), Tab3 = Tab2 == std::string::npos ? Tab2 : Line.find('\t', Tab2 + 1);
                        Z = Tab3 == std::string::npos ? SIZE_MAX : std::strtoull(Line.c_str() + Tab + 1, nullptr, 10);
                        if (Z < Names.size() && Line.substr(Tab3 + 1) == Names[Z]) E.Size = std::strtoull(Line.c_str() + Tab2 + 1, nullptr, 10);
                        else Z = SIZE_MAX;
                    }
                    else if (Z == SIZE_MAX) continue;
                    else if (Line.compare(0, Tab, "part") == 0)
                    {
                        const size_t Tab2 = Line.find('\t', Tab + 1);
                        if (Tab2 == std::string::npos) Z = SIZE_MAX;
                        else E.Parts.emplace_back(Line.substr(Tab + 1, Tab2 - Tab - 1), std::strtoull(Line.c_str() + Tab2 + 1, nullptr, 16));
                    }
                    else if (const KindInfo* K = KindNamed(Line.substr(0, Tab)); K != nullptr)
                    {
                        std::string Name = Line.substr(Tab + 1);
                        if (!TextName(Name)) Name = FallbackName(K->Id, Name);
                        E.Defined.push_back({ K->Id, std::move(Name) });
                    }
                    else Z = SIZE_MAX;
                }
            }
        }
        Out.open(Path, std::ios::binary | std::ios::trunc);
        Out << PartialMagic << Format << "\nkey " << Key << "\n";
        for (size_t Z = 0; Z < Read.size(); Z++)
            if (Read[Z].Done) WriteEntry(Out, Z, Names[Z], Read[Z]);
        Out.flush();
    }

    // Every zone of Names not read yet, read by a pool of workers: at most Options.Threads (8 by
    // default and at most, 1 for a game whose IndexInParallel is false), each opening one zone at
    // a time, a zone admitted, in zone order, only while the zones open stay under the memory
    // budget (a zone's footprint estimated at 5 times its file; GRAYWOLF_INDEX_BUDGET_MB, 4096 by
    // default; a zone alone is always admitted). ResolveZones is called by one worker at a time
    // (it may extract the zone from a package). Step and the partial file are written on the
    // calling thread. False when cancelled.
    bool BuildZones(const IDiskGame& Game, const ExportOptions& Options, const std::string& Install, const std::vector<std::string>& Names,
                    const std::string& Key, const std::string& PartialPath, std::vector<ZoneEntry>& Read,
                    const std::function<void(size_t, size_t)>& Step, const std::atomic<bool>* Cancel)
    {
        std::ofstream Partial;
        ResumePartial(PartialPath, Key, Names, Read, Partial);
        std::vector<size_t> Todo;
        for (size_t Z = 0; Z < Names.size(); Z++)
            if (!Read[Z].Done) Todo.push_back(Z);
        size_t Finished = Names.size() - Todo.size();

        unsigned Threads = Options.Threads ? Options.Threads : 8;
        Threads = std::min<unsigned>({ Threads, 8u, (unsigned)std::max<size_t>(Todo.size(), 1) });
        if (!Game.IndexInParallel()) Threads = 1;
        uint64_t Budget = 4096ull << 20;
        if (char Mb[32] = {}; GetEnvironmentVariableA("GRAYWOLF_INDEX_BUDGET_MB", Mb, sizeof(Mb)) && std::strtoull(Mb, nullptr, 10) > 0)
            Budget = std::strtoull(Mb, nullptr, 10) << 20;

        ExportOptions Indexing = Options;
        Indexing.Indexing = true;
        std::mutex Lock, ResolveLock;
        std::condition_variable Changed;
        size_t Next = 0, Admitted = 0, Running = Threads;
        uint64_t Open = 0;
        std::vector<std::pair<size_t, ZoneEntry>> Ready;
        std::exception_ptr Failed;
        const auto Stopped = [&] { return (Cancel != nullptr && Cancel->load()) || Failed != nullptr; };

        const auto Work = [&] {
            for (;;)
            {
                size_t K;
                {
                    std::lock_guard<std::mutex> G(Lock);
                    if (Next >= Todo.size() || Stopped()) break;
                    K = Next++;
                }
                const size_t Z = Todo[K];
                try
                {
                    std::vector<std::string> Paths;
                    std::string Why;
                    bool Resolved;
                    {
                        std::lock_guard<std::mutex> G(ResolveLock);
                        Resolved = Game.ResolveZones(Install, { Names[Z] }, Options, Paths, Why) && !Paths.empty();
                    }
                    std::error_code Ec;
                    const uint64_t Bytes = Resolved ? fs::file_size(Paths.front(), Ec) : 0;
                    const uint64_t Estimate = Ec ? 0 : Bytes * 5;
                    {
                        // admitted in zone order, within the budget
                        std::unique_lock<std::mutex> G(Lock);
                        Changed.wait(G, [&] { return Stopped() || (Admitted == K && (Open == 0 || Open + Estimate <= Budget)); });
                        if (Admitted != K || Stopped()) break;
                        Admitted++;
                        Open += Estimate;
                    }
                    Changed.notify_all();
                    ZoneEntry E;
                    E.Done = true;
                    if (Resolved) E = ReadZone(Game, Indexing, Paths.front());
                    {
                        std::lock_guard<std::mutex> G(Lock);
                        Open -= Estimate;
                        Ready.emplace_back(Z, std::move(E));
                    }
                }
                catch (...)
                {
                    std::lock_guard<std::mutex> G(Lock);
                    if (!Failed) Failed = std::current_exception();
                }
                Changed.notify_all();
            }
            {
                std::lock_guard<std::mutex> G(Lock);
                Running--;
            }
            Changed.notify_all();
        };

        if (Step) Step(Finished, Names.size());
        std::vector<std::thread> Pool;
        if (Todo.empty()) Running = 0;
        else
            for (unsigned T = 0; T < Threads; T++) Pool.emplace_back(Work);
        for (bool More = !Pool.empty(); More;)
        {
            std::vector<std::pair<size_t, ZoneEntry>> Got;
            {
                std::unique_lock<std::mutex> G(Lock);
                // (the cancel flag is set from elsewhere: looked at every half second)
                Changed.wait_for(G, std::chrono::milliseconds(500), [&] { return !Ready.empty() || Running == 0; });
                Got.swap(Ready);
                More = Running != 0;
            }
            if (Cancel != nullptr && Cancel->load()) Changed.notify_all();
            for (auto& [Z, E] : Got)
            {
                WriteEntry(Partial, Z, Names[Z], E);
                Read[Z] = std::move(E);
                if (Step) Step(++Finished, Names.size());
            }
        }
        for (auto& T : Pool) T.join();
        if (Failed) std::rethrow_exception(Failed);
        if (Cancel != nullptr && Cancel->load()) return false;
        if (Step) Step(Names.size(), Names.size());
        return true;
    }
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

size_t ZoneIndex::AssetHash::operator()(const AssetId& A) const
{
    // FNV-1a over the name's lower-case letters, mixed with the kind
    uint64_t H = 0xcbf29ce484222325ull ^ (uint64_t)A.K;
    for (const char C : A.Name)
    {
        H ^= (uint8_t)(C >= 'A' && C <= 'Z' ? C - 'A' + 'a' : C);
        H *= 0x100000001b3ull;
    }
    return (size_t)H;
}

std::vector<size_t> ZoneIndex::DefinedIn(const AssetId& A) const
{
    const auto It = Where.find(A);
    return It == Where.end() ? std::vector<size_t>() : It->second;
}

std::vector<size_t> ZoneIndex::PartIn(const std::string& Type, uint64_t Hash) const
{
    const auto It = PartsWhere.find({ Type, Hash });
    return It == PartsWhere.end() ? std::vector<size_t>() : It->second;
}

void ZoneIndex::Add(size_t Zone, AssetId A)
{
    Where[A].push_back(Zone);
    Assets[Zone].push_back(std::move(A));
    Entries++;
}

bool ZoneIndex::Load(const std::string& Key)
{
    // read whole and cut in place (an install's index can hold millions of lines)
    std::string Text;
    {
        std::ifstream In(File, std::ios::binary);
        if (!In) return false;
        In.seekg(0, std::ios::end);
        const std::streamoff Size = In.tellg();
        if (Size <= 0) return false;
        Text.resize((size_t)Size);
        In.seekg(0);
        if (!In.read(Text.data(), Size)) return false;
    }
    size_t At = 0;
    const auto NextLine = [&](std::string_view& Line) {
        if (At >= Text.size()) return false;
        size_t End = Text.find('\n', At);
        if (End == std::string::npos) End = Text.size();
        Line = std::string_view(Text).substr(At, End - At);
        At = End + 1;
        return true;
    };
    std::string_view Line;
    if (!NextLine(Line) || Line != "greyhound-zone-index " + std::to_string(Format) || !NextLine(Line) || Line.rfind("key ", 0) != 0 ||
        (!Key.empty() && Line != "key " + Key))
        return false;
    // the kinds by name, and the part types met (each line's type text kept once)
    std::vector<std::pair<std::string_view, Kind>> KindNames;
    for (const KindInfo& I : Kinds) KindNames.emplace_back(I.Name, I.Id);
    std::string Type;
    {
        size_t PartLines = 0, Lines = 0;
        for (size_t P = Text.find('\n'); P != std::string::npos; P = Text.find('\n', P + 1))
        {
            Lines++;
            PartLines += Text.compare(P + 1, 5, "part\t") == 0;
        }
        PartsWhere.reserve(PartLines);
        Where.reserve(Lines - PartLines);
    }
    while (NextLine(Line))
    {
        const size_t Tab = Line.find('\t');
        if (Tab == std::string_view::npos) continue;
        const std::string_view Head = Line.substr(0, Tab);
        if (Head == "zone")
        {
            const size_t Tab2 = Line.find('\t', Tab + 1);
            if (Tab2 == std::string_view::npos) continue;
            Sizes.push_back(std::strtoull(Line.data() + Tab + 1, nullptr, 10));
            Names.emplace_back(Line.substr(Tab2 + 1));
            Assets.emplace_back();
        }
        else if (Head == "part")
        {
            // part<TAB><type><TAB><hash, hex>
            const size_t Tab2 = Line.find('\t', Tab + 1);
            if (Tab2 == std::string_view::npos || Names.empty()) continue;
            Type.assign(Line.substr(Tab + 1, Tab2 - Tab - 1));
            PartsWhere[{ Type, std::strtoull(Line.data() + Tab2 + 1, nullptr, 16) }].push_back(Names.size() - 1);
        }
        else if (!Names.empty())
            for (const auto& [N, K] : KindNames)
                if (Head == N)
                {
                    // (a file built before names were checked: the fallback a build gives now)
                    std::string Name(Line.substr(Tab + 1));
                    if (!TextName(Name)) Name = FallbackName(K, Name);
                    Add(Names.size() - 1, { K, std::move(Name) });
                    break;
                }
    }
    return true;
}

std::unique_ptr<ZoneIndex> ZoneIndex::Kept(const IDiskGame& Game, const ExportOptions& Options, const std::string& Install)
{
    const auto Start = std::chrono::steady_clock::now();
    std::vector<std::string> Names;
    std::string Error;
    if (!Game.IndexedZones(Install, Options, Names, Error)) return nullptr;
    auto Ix = std::unique_ptr<ZoneIndex>(new ZoneIndex());
    Ix->File = KeptFile(Game, Options, Install, ".idx");
    if (!Ix->Load(std::string()) || Ix->Names != Names) return nullptr;
    Ix->Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
    return Ix;
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
    return For(Game, Options, Install, ReaderKey, Step, Cancel, Error);
}

std::unique_ptr<ZoneIndex> ZoneIndex::For(const IDiskGame& Game, const ExportOptions& Options, const std::string& Install,
                                          const std::string& ReaderKey, const std::function<void(size_t, size_t)>& Step,
                                          const std::atomic<bool>* Cancel, std::string& Error)
{
    const auto Start = std::chrono::steady_clock::now();
    auto Ix = std::unique_ptr<ZoneIndex>(new ZoneIndex());
    std::vector<std::string> Names;
    if (!Game.IndexedZones(Install, Options, Names, Error)) return nullptr;
    Siren::AddSearchDir(fs::path(Install).wstring());
    std::string KeyText = "format " + std::to_string(Format) + "\n" + Game.Id() + "\n" + Lower(fs::absolute(Install).string()) +
                          "\n" + ReaderKey + "\n";
    for (const auto& N : Names) KeyText += N + "\n";
    const std::string Key = Hex16(Fnv1a(KeyText));
    const std::string File = KeptFile(Game, Options, Install, ".idx");
    Ix->File = File;

    const auto Done = [&] {
        Ix->Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
        // the install's folder beside its index (KeptInstalls): an install no detection finds is found again
        const std::string Beside = KeptFile(Game, Options, Install, ".install");
        std::error_code Ec;
        if (!fs::exists(Beside, Ec)) std::ofstream(Beside, std::ios::binary) << fs::absolute(Install).string() << "\n";
        return std::move(Ix);
    };

    // the kept file, when its key is this one
    if (Ix->Load(Key) && Ix->Names.size() == Names.size()) return Done();

    // built: every zone opened by a pool of workers (BuildZones), each result kept per zone and
    // added in zone order, so the file is the one a serial build writes
    *Ix = ZoneIndex();
    Ix->File = File;
    Ix->Built = true;
    Ix->Names = Names;
    Ix->Sizes.assign(Names.size(), 0);
    Ix->Assets.resize(Names.size());
    std::vector<ZoneEntry> Read(Names.size());
    if (!BuildZones(Game, Options, Install, Names, Key, File + ".partial", Read, Step, Cancel))
    {
        Error = "cancelled";
        return nullptr;
    }
    std::vector<std::vector<std::pair<std::string, uint64_t>>> Parts(Names.size());
    for (size_t Z = 0; Z < Names.size(); Z++)
    {
        Ix->Sizes[Z] = Read[Z].Size;
        for (auto& A : Read[Z].Defined) Ix->Add(Z, std::move(A));
        for (auto& N : Read[Z].Renamed) Ix->Renamed.push_back(std::move(N));
        Parts[Z] = std::move(Read[Z].Parts);
        for (const auto& P : Parts[Z]) Ix->PartsWhere[P].push_back(Z);
    }

    const std::string Temp = Ix->File + ".tmp";
    {
        std::ofstream Out(Temp, std::ios::binary | std::ios::trunc);
        Out << "greyhound-zone-index " << Format << "\nkey " << Key << "\n";
        for (size_t Z = 0; Z < Ix->Names.size(); Z++)
        {
            Out << "zone\t" << Ix->Sizes[Z] << "\t" << Ix->Names[Z] << "\n";
            for (const auto& A : Ix->Assets[Z]) Out << KindName(A.K) << "\t" << A.Name << "\n";
            for (const auto& [Type, Hash] : Parts[Z]) Out << "part\t" << Type << "\t" << std::hex << Hash << std::dec << "\n";
        }
    }
    std::error_code Ec;
    fs::rename(Temp, Ix->File, Ec);
    fs::remove(File + ".partial", Ec);
    return Done();
}
}
