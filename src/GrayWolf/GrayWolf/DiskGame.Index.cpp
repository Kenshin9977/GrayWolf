#include "stdafx.h"

// DiskGame::ZoneIndex: where each asset of an install is defined. One file per game and install
// under <--cache>/index (or %TEMP%/greyhound_index), <game>_<install hash>.gwi, read in place
// (ZoneIndex::Mapped, below): its zones, the assets each defines, and hash tables over the assets
// and the parts (IDiskZone::Parts) a lookup reads a slot and a list of. Built by a pool of workers
// (BuildZones), resumable from a partial file beside it. The key hashes the format, the game, the
// install, the zone list and the reader's IndexKey (its tables): any change rebuilds the file. The
// text index of before (.idx: a "zone" line per zone, a "<kind>\t<name>" line per asset, a
// "part\t<type>\t<hash>" line per part) is read once and written as one.

#include "DiskGame.h"
#include "DiskGameSizes.h"
#include "MappedFile.h"
#include "ExportNaming.h"
#include "ExportWriters.h"
#include "Siren.h"

#define XXH_INLINE_ALL   // header-only: no xxhash.lib to link
#include <xxhash.h>

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
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
    // the text index of before (ZoneIndex::Load), read to be written as a kept file
    constexpr int TextFormat = 2;

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
        std::vector<Weight> Weights;    // beside Defined
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
    ZoneEntry ReadZone(const IDiskGame& Game, const ExportOptions& Options, const std::string& Path)
    {
        ZoneEntry E;
        E.Done = true;
        std::string Why;
        const auto Zone = Game.Open(Path, Options, Why);
        if (!Zone) return E;
        std::vector<std::pair<AssetId, Weight>> Weighed;
        WeighAll(*Zone, Weighed, !Game.IndexInParallel());
        for (auto& [A, W] : Weighed)
            if (!TextName(A.Name))
            {
                const std::string Name = FallbackName(A.K, A.Name);
                E.Renamed.push_back(fs::path(Path).filename().string() + ": " + KindName(A.K) + " " + Escaped(A.Name) + " -> " + Name);
                A.Name = Name;
            }
        // once each, the heaviest of a name listed twice
        std::sort(Weighed.begin(), Weighed.end(), [](const auto& X, const auto& Y) {
            return X.first < Y.first || (!(Y.first < X.first) && X.second.Bytes > Y.second.Bytes);
        });
        for (auto& [A, W] : Weighed)
            if (E.Defined.empty() || E.Defined.back() < A)
            {
                E.Defined.push_back(std::move(A));
                E.Weights.push_back(W);
            }
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
        for (size_t I = 0; I < E.Defined.size(); I++)
        {
            const Weight W = I < E.Weights.size() ? E.Weights[I] : Weight();
            Out << KindName(E.Defined[I].K) << "\t" << W.Bytes << "\t" << W.A << "\t" << W.B << "\t" << W.C << "\t" << E.Defined[I].Name << "\n";
        }
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
                        // <kind> <bytes> <a> <b> <c> <name>
                        size_t From = Tab + 1;
                        uint64_t Fields[4] = {};
                        bool Whole = true;
                        for (uint64_t& F : Fields)
                        {
                            const size_t Next = Line.find('\t', From);
                            if (Next == std::string::npos) { Whole = false; break; }
                            F = std::strtoull(Line.c_str() + From, nullptr, 10);
                            From = Next + 1;
                        }
                        if (!Whole) { Z = SIZE_MAX; continue; }
                        std::string Name = Line.substr(From);
                        if (!TextName(Name)) Name = FallbackName(K->Id, Name);
                        E.Defined.push_back({ K->Id, std::move(Name) });
                        E.Weights.push_back({ Fields[0], (uint32_t)Fields[1], (uint32_t)Fields[2], (uint32_t)Fields[3] });
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
    // (PackageLock: it may extract the zone from a package). Step and the partial file are written on the
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
        std::mutex Lock;
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
                        std::lock_guard<std::mutex> G(PackageLock());
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

// The kept index as a file read in place: a header, the zones, the assets in zone order, and two
// open-addressing hash tables (linear probing, as cdb's) over the assets (kind + lower-case name)
// and the parts (type + hash), each slot naming a list of zones. A lookup reads one slot or a few
// and one list; nothing is loaded whole. Little-endian; offsets from the file's start.
namespace
{
    constexpr char IndexMagic[4] = { 'G', 'W', 'Z', 'I' };
    // 2: each asset's weight after its name (1: none, read as unknown)
    constexpr uint32_t IndexVersion = 2;

#pragma pack(push, 1)
    struct FileHeader
    {
        char Magic[4];
        uint32_t Version;
        uint64_t KeyAt, KeyLength;
        uint64_t ZonesAt, ZoneCount;
        uint64_t AssetsAt, AssetCount;
        uint64_t AssetSlotsAt, AssetSlotCount;   // a power of two
        uint64_t PartSlotsAt, PartSlotCount;     // a power of two
        uint64_t ListsAt, ListsSize;
        uint64_t StringsAt, StringsSize;
    };
    struct FileZone
    {
        uint64_t Size, NameAt;
        uint32_t NameLength, FirstAsset, AssetCount, Pad;
    };
    struct FileAsset   // version 1's: the first 16 bytes
    {
        uint64_t NameAt;
        uint32_t NameLength;
        uint8_t Kind, Pad[3];
        uint64_t Bytes;
        uint32_t A, B, C, Pad2;
    };
    constexpr uint64_t AssetStride1 = 16;
    struct FileSlot
    {
        uint64_t Hash;   // 0: empty
        uint64_t List;   // from ListsAt: an asset's {u32 asset, u32 count, count u32 zones}; a part's
                         // {u64 type at, u32 type length, u32 pad, u64 hash, u32 count, count u32 zones}
    };
#pragma pack(pop)

    uint64_t NameKey(Kind K, std::string_view Name)
    {
        std::string L(Name);
        for (char& C : L) C = (char)(C >= 'A' && C <= 'Z' ? C - 'A' + 'a' : C);
        const uint64_t H = XXH3_64bits_withSeed(L.data(), L.size(), (uint64_t)K);
        return H ? H : 1;
    }
    uint64_t PartKey(std::string_view Type, uint64_t Hash)
    {
        const uint64_t H = XXH3_64bits_withSeed(Type.data(), Type.size(), Hash);
        return H ? H : 1;
    }
    uint64_t SlotsFor(size_t N)
    {
        uint64_t S = 16;
        while (S < (uint64_t)N * 2) S <<= 1;
        return S;
    }
    bool SameName(std::string_view A, std::string_view B)
    {
        if (A.size() != B.size()) return false;
        for (size_t K = 0; K < A.size(); K++)
        {
            const char X = A[K] >= 'A' && A[K] <= 'Z' ? A[K] - 'A' + 'a' : A[K];
            const char Y = B[K] >= 'A' && B[K] <= 'Z' ? B[K] - 'A' + 'a' : B[K];
            if (X != Y) return false;
        }
        return true;
    }
}

struct ZoneIndex::Mapped
{
    MappedFile File;
    const uint8_t* Base = nullptr;
    uint64_t Size = 0;
    const FileHeader* Head = nullptr;
    uint64_t Stride = sizeof(FileAsset);   // a version 1 file's are shorter
    // the asset at Index, nullptr past the file (of a version 1 file, only its first 16 bytes)
    const FileAsset* Asset(uint64_t Index) const
    {
        const uint64_t Off = Head->AssetsAt + Index * Stride;
        return Holds(Off, Stride) ? reinterpret_cast<const FileAsset*>(Base + Off) : nullptr;
    }
    bool Holds(uint64_t At, uint64_t N) const { return At <= Size && N <= Size - At; }
    std::string_view Text(uint64_t At, uint64_t N) const
    {
        const uint64_t From = Head->StringsAt + At;
        return Holds(From, N) ? std::string_view((const char*)Base + From, (size_t)N) : std::string_view();
    }
    template <class T>
    const T* At(uint64_t Off, uint64_t Count = 1) const
    {
        return Holds(Off, Count * sizeof(T)) ? reinterpret_cast<const T*>(Base + Off) : nullptr;
    }
    // the slot list a key's probe ends at, 0 when the key is not there; Match checks a list
    template <class F>
    uint64_t Find(uint64_t SlotsAt, uint64_t Count, uint64_t Key, const F& Match) const
    {
        const FileSlot* Slots = At<FileSlot>(SlotsAt, Count);
        if (Slots == nullptr || Count == 0) return 0;
        for (uint64_t K = 0, S = Key & (Count - 1); K < Count; K++, S = (S + 1) & (Count - 1))
        {
            if (Slots[S].Hash == 0) return 0;
            if (Slots[S].Hash == Key && Match(Head->ListsAt + Slots[S].List)) return Head->ListsAt + Slots[S].List;
        }
        return 0;
    }
};

bool ZoneIndex::LoadMapped(const std::string& Path, const std::string& Key)
{
    auto M = std::make_shared<Mapped>();
    if (!M->File.Open(fs::path(Path)) || M->File.size() < sizeof(FileHeader)) return false;
    M->Base = M->File.data();
    M->Size = M->File.size();
    M->Head = reinterpret_cast<const FileHeader*>(M->Base);
    const FileHeader& H = *M->Head;
    if (std::memcmp(H.Magic, IndexMagic, 4) != 0 || (H.Version != IndexVersion && H.Version != 1)) return false;
    M->Stride = H.Version == 1 ? AssetStride1 : sizeof(FileAsset);
    if (!M->Holds(H.StringsAt, H.StringsSize) || !M->Holds(H.ListsAt, H.ListsSize) || M->At<FileZone>(H.ZonesAt, H.ZoneCount) == nullptr ||
        !M->Holds(H.AssetsAt, H.AssetCount * M->Stride) || M->At<FileSlot>(H.AssetSlotsAt, H.AssetSlotCount) == nullptr ||
        M->At<FileSlot>(H.PartSlotsAt, H.PartSlotCount) == nullptr)
        return false;
    if (!Key.empty() && M->Text(H.KeyAt, H.KeyLength) != Key) return false;
    const FileZone* Z = M->At<FileZone>(H.ZonesAt, H.ZoneCount);
    Names.clear();
    Sizes.clear();
    for (uint64_t K = 0; K < H.ZoneCount; K++)
    {
        Names.emplace_back(M->Text(Z[K].NameAt, Z[K].NameLength));
        Sizes.push_back(Z[K].Size);
    }
    Assets.assign(Names.size(), {});
    AssetWeights.assign(Names.size(), {});
    Entries = (size_t)H.AssetCount;
    Map = std::move(M);
    return true;
}

bool ZoneIndex::SaveMapped(const std::string& Path, const std::string& Key) const
{
    FileHeader H{};
    std::memcpy(H.Magic, IndexMagic, 4);
    H.Version = IndexVersion;
    std::string Strings;
    const auto Text = [&](std::string_view S) {
        const uint64_t At = Strings.size();
        Strings.append(S);
        return At;
    };
    H.KeyAt = Text(Key), H.KeyLength = Key.size();
    std::vector<FileZone> Zs(Names.size());
    std::vector<FileAsset> As;
    As.reserve(Entries);
    for (size_t Z = 0; Z < Names.size(); Z++)
    {
        Zs[Z].Size = Sizes[Z];
        Zs[Z].NameAt = Text(Names[Z]), Zs[Z].NameLength = (uint32_t)Names[Z].size();
        Zs[Z].FirstAsset = (uint32_t)As.size(), Zs[Z].AssetCount = (uint32_t)Assets[Z].size();
        for (size_t I = 0; I < Assets[Z].size(); I++)
        {
            const AssetId& A = Assets[Z][I];
            const Weight W = I < AssetWeights[Z].size() ? AssetWeights[Z][I] : Weight();
            As.push_back({ Text(A.Name), (uint32_t)A.Name.size(), (uint8_t)A.K, {}, W.Bytes, W.A, W.B, W.C, 0 });
        }
    }
    // the lists, then the slots: each asset's first record (its name, to check) and its zones
    std::string Lists;
    const auto U32 = [&](uint32_t V) { Lists.append(reinterpret_cast<const char*>(&V), 4); };
    const auto U64 = [&](uint64_t V) { Lists.append(reinterpret_cast<const char*>(&V), 8); };
    std::vector<FileSlot> AssetSlots(SlotsFor(Where.size())), PartSlots(SlotsFor(PartsWhere.size()));
    const auto Place = [](std::vector<FileSlot>& Slots, uint64_t Key, uint64_t List) {
        for (uint64_t S = Key & (Slots.size() - 1);; S = (S + 1) & (Slots.size() - 1))
            if (Slots[S].Hash == 0)
            {
                Slots[S] = { Key, List };
                return;
            }
    };
    {
        // an asset's first record: the first zone defining it, its place among that zone's assets
        std::unordered_map<AssetId, uint32_t, AssetHash, AssetSame> First;
        First.reserve(Where.size());
        for (size_t Z = 0, R = 0; Z < Names.size(); Z++)
            for (const AssetId& A : Assets[Z]) First.try_emplace(A, (uint32_t)R++);
        for (const auto& [A, Zones] : Where)
        {
            const uint64_t List = Lists.size();
            U32(First[A]);
            U32((uint32_t)Zones.size());
            for (const size_t Z : Zones) U32((uint32_t)Z);
            Place(AssetSlots, NameKey(A.K, A.Name), List);
        }
    }
    for (const auto& [Part, Zones] : PartsWhere)
    {
        const uint64_t List = Lists.size();
        U64(Text(Part.first));
        U32((uint32_t)Part.first.size());
        U32(0);
        U64(Part.second);
        U32((uint32_t)Zones.size());
        for (const size_t Z : Zones) U32((uint32_t)Z);
        Place(PartSlots, PartKey(Part.first, Part.second), List);
    }
    uint64_t At = sizeof(FileHeader);
    const auto Section = [&](uint64_t Bytes) {
        const uint64_t Here = (At + 7) & ~7ull;
        At = Here + Bytes;
        return Here;
    };
    H.ZonesAt = Section(Zs.size() * sizeof(FileZone)), H.ZoneCount = Zs.size();
    H.AssetsAt = Section(As.size() * sizeof(FileAsset)), H.AssetCount = As.size();
    H.AssetSlotsAt = Section(AssetSlots.size() * sizeof(FileSlot)), H.AssetSlotCount = AssetSlots.size();
    H.PartSlotsAt = Section(PartSlots.size() * sizeof(FileSlot)), H.PartSlotCount = PartSlots.size();
    H.ListsAt = Section(Lists.size()), H.ListsSize = Lists.size();
    H.StringsAt = Section(Strings.size()), H.StringsSize = Strings.size();
    std::string Out((size_t)At, '\0');
    const auto Put = [&](uint64_t Off, const void* P, size_t N) {
        if (N) std::memcpy(Out.data() + Off, P, N);
    };
    Put(0, &H, sizeof(H));
    Put(H.ZonesAt, Zs.data(), Zs.size() * sizeof(FileZone));
    Put(H.AssetsAt, As.data(), As.size() * sizeof(FileAsset));
    Put(H.AssetSlotsAt, AssetSlots.data(), AssetSlots.size() * sizeof(FileSlot));
    Put(H.PartSlotsAt, PartSlots.data(), PartSlots.size() * sizeof(FileSlot));
    Put(H.ListsAt, Lists.data(), Lists.size());
    Put(H.StringsAt, Strings.data(), Strings.size());
    const std::string Temp = Path + ".tmp";
    {
        std::ofstream F(Temp, std::ios::binary | std::ios::trunc);
        if (!F.write(Out.data(), (std::streamsize)Out.size())) return false;
    }
    std::error_code Ec;
    fs::rename(Temp, Path, Ec);
    return !Ec;
}

const std::vector<AssetId>& ZoneIndex::Defined(size_t Zone) const
{
    if (!Map) return Assets[Zone];
    std::lock_guard<std::mutex> G(*MappedLock);
    std::vector<AssetId>& Out = Assets[Zone];
    if (!Out.empty() || Zone >= Names.size()) return Out;
    std::vector<Weight>& Ws = AssetWeights[Zone];
    Ws.clear();
    const FileZone* Z = Map->At<FileZone>(Map->Head->ZonesAt + Zone * sizeof(FileZone));
    for (uint32_t K = 0; Z != nullptr && K < Z->AssetCount; K++)
    {
        const FileAsset* A = Map->Asset((uint64_t)Z->FirstAsset + K);
        if (A == nullptr) break;
        Out.push_back({ (Kind)A->Kind, std::string(Map->Text(A->NameAt, A->NameLength)) });
        Ws.push_back(Map->Stride == sizeof(FileAsset) ? Weight{ A->Bytes, A->A, A->B, A->C } : Weight());
    }
    return Out;
}

const std::vector<Weight>& ZoneIndex::Weights(size_t Zone) const
{
    if (Map) Defined(Zone);   // read with the assets
    return AssetWeights[Zone];
}

std::vector<size_t> ZoneIndex::DefinedIn(const AssetId& A) const
{
    if (!Map)
    {
        const auto It = Where.find(A);
        return It == Where.end() ? std::vector<size_t>() : It->second;
    }
    const FileHeader& H = *Map->Head;
    const auto Zones = [&](uint64_t List, uint64_t Skip) {
        std::vector<size_t> Out;
        const uint32_t* N = Map->At<uint32_t>(List + Skip);
        const uint32_t* Z = N ? Map->At<uint32_t>(List + Skip + 4, *N) : nullptr;
        for (uint32_t K = 0; Z != nullptr && K < *N; K++) Out.push_back(Z[K]);
        return Out;
    };
    const uint64_t List = Map->Find(H.AssetSlotsAt, H.AssetSlotCount, NameKey(A.K, A.Name), [&](uint64_t L) {
        const uint32_t* First = Map->At<uint32_t>(L);
        const FileAsset* R = First ? Map->Asset(*First) : nullptr;
        return R != nullptr && (Kind)R->Kind == A.K && SameName(Map->Text(R->NameAt, R->NameLength), A.Name);
    });
    return List ? Zones(List, 4) : std::vector<size_t>();
}

std::vector<size_t> ZoneIndex::PartIn(const std::string& Type, uint64_t Hash) const
{
    if (!Map)
    {
        const auto It = PartsWhere.find({ Type, Hash });
        return It == PartsWhere.end() ? std::vector<size_t>() : It->second;
    }
    const FileHeader& H = *Map->Head;
    const uint64_t List = Map->Find(H.PartSlotsAt, H.PartSlotCount, PartKey(Type, Hash), [&](uint64_t L) {
        const uint64_t* TypeAt = Map->At<uint64_t>(L);
        const uint32_t* TypeLength = Map->At<uint32_t>(L + 8);
        const uint64_t* Value = Map->At<uint64_t>(L + 16);
        return TypeAt && TypeLength && Value && *Value == Hash && Map->Text(*TypeAt, *TypeLength) == Type;
    });
    std::vector<size_t> Out;
    const uint32_t* N = List ? Map->At<uint32_t>(List + 24) : nullptr;
    const uint32_t* Z = N ? Map->At<uint32_t>(List + 28, *N) : nullptr;
    for (uint32_t K = 0; Z != nullptr && K < *N; K++) Out.push_back(Z[K]);
    return Out;
}

void ZoneIndex::Add(size_t Zone, AssetId A, Weight W)
{
    Where[A].push_back(Zone);
    AssetWeights[Zone].push_back(W);
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
    if (!NextLine(Line) || Line != "greyhound-zone-index " + std::to_string(TextFormat) || !NextLine(Line) || Line.rfind("key ", 0) != 0 ||
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
            AssetWeights.emplace_back();
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
    Ix->File = KeptFile(Game, Options, Install, ".gwi");
    if (!Ix->LoadMapped(Ix->File, std::string()) || Ix->Names != Names)
    {
        *Ix = ZoneIndex();
        Ix->File = KeptFile(Game, Options, Install, ".idx");
        if (!Ix->Load(std::string()) || Ix->Names != Names) return nullptr;
    }
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
    return For(Game, Options, Install, ReaderKey, Step, Cancel, Error, false);
}

std::unique_ptr<ZoneIndex> ZoneIndex::For(const IDiskGame& Game, const ExportOptions& Options, const std::string& Install,
                                          const std::string& ReaderKey, const std::function<void(size_t, size_t)>& Step,
                                          const std::atomic<bool>* Cancel, std::string& Error, bool AcceptUnweighed)
{
    const auto Start = std::chrono::steady_clock::now();
    auto Ix = std::unique_ptr<ZoneIndex>(new ZoneIndex());
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
    Ix->File = File;

    const auto Done = [&] {
        Ix->Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
        // the install's folder beside its index (KeptInstalls): an install no detection finds is found again
        const std::string Beside = KeptFile(Game, Options, Install, ".install");
        std::error_code Ec;
        if (!fs::exists(Beside, Ec)) std::ofstream(Beside, std::ios::binary) << fs::absolute(Install).string() << "\n";
        return std::move(Ix);
    };

    // the kept file, when its key is this one: read in place; a text index of before, read and
    // written as one
    if (Ix->LoadMapped(File, Key) && Ix->Names.size() == Names.size()) return Done();
    // one kept before weights: what an export asks of it, until the install is indexed again
    // (Build: the weights of the size estimate)
    *Ix = ZoneIndex();
    Ix->File = File;
    if (AcceptUnweighed && Ix->LoadMapped(File, KeyOf(Unweighed)) && Ix->Names.size() == Names.size()) return Done();
    *Ix = ZoneIndex();
    Ix->File = TextFile;
    if (Ix->Load(Key) && Ix->Names.size() == Names.size())
    {
        Ix->File = File;
        Ix->SaveMapped(File, Key);
        return Done();
    }

    // built: every zone opened by a pool of workers (BuildZones), each result kept per zone and
    // added in zone order, so the file is the one a serial build writes
    *Ix = ZoneIndex();
    Ix->File = File;
    Ix->Built = true;
    Ix->Names = Names;
    Ix->Sizes.assign(Names.size(), 0);
    Ix->Assets.resize(Names.size());
    Ix->AssetWeights.resize(Names.size());
    std::vector<ZoneEntry> Read(Names.size());
    if (!BuildZones(Game, Options, Install, Names, Key, File + ".partial", Read, Step, Cancel))
    {
        Error = "cancelled";
        return nullptr;
    }
    for (size_t Z = 0; Z < Names.size(); Z++)
    {
        Ix->Sizes[Z] = Read[Z].Size;
        for (size_t I = 0; I < Read[Z].Defined.size(); I++)
            Ix->Add(Z, std::move(Read[Z].Defined[I]), I < Read[Z].Weights.size() ? Read[Z].Weights[I] : Weight());
        for (auto& N : Read[Z].Renamed) Ix->Renamed.push_back(std::move(N));
        for (const auto& P : Read[Z].Parts) Ix->PartsWhere[P].push_back(Z);
    }

    Ix->SaveMapped(Ix->File, Key);
    std::error_code Ec;
    fs::remove(File + ".partial", Ec);
    return Done();
}
}
