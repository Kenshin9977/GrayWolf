#include "stdafx.h"

// Context::MaterialsElsewhere, as Export gives it (DiskGame.Materials.h): a zone's name-only
// material headers looked up in the zones the install index says define those materials. Each
// such zone's book (every material it defines whole and its images, IDiskZone::MaterialImages) is
// read once, its zone opened alone (the asking zone parked meanwhile), and kept on disk under
// <--cache>/material_books (or %TEMP%/greyhound_material_books), a file per game and zone keyed
// by the index (its file and time: a rebuilt index reads the books anew) and the reader's key.

#include "DiskGame.Materials.h"

#include "ExportNaming.h"
#include "MemoryBudget.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <process.h>
#include <sstream>

namespace fs = std::filesystem;

namespace DiskGame
{
namespace
{
    std::string Stem(const std::string& Zone)
    {
        return ExportNaming::Lower(fs::path(Zone).stem().string());
    }

    // A line per material: its name, then (usage, hash in hex, image) triples, tab separated
    bool ReadBook(const fs::path& File, MaterialBook& Out)
    {
        std::ifstream In(File);
        if (!In) return false;
        for (std::string Line; std::getline(In, Line);)
        {
            std::istringstream Fields(Line);
            std::string Name, Usage, Hash, Image;
            if (!std::getline(Fields, Name, '\t') || Name.empty()) continue;
            auto& Images = Out[Name];
            while (std::getline(Fields, Usage, '\t') && std::getline(Fields, Hash, '\t') && std::getline(Fields, Image, '\t'))
                Images.push_back({ (uint8_t)std::stoul(Usage), (uint32_t)std::stoul(Hash, nullptr, 16), Image });
        }
        return true;
    }

    void WriteBook(const fs::path& File, const MaterialBook& Book)
    {
        std::error_code Ec;
        fs::create_directories(File.parent_path(), Ec);
        // written aside then renamed: another process may read or write the same book meanwhile
        const std::string Aside = File.string() + "." + std::to_string(_getpid()) + ".tmp";
        {
            std::ofstream O(Aside, std::ios::binary);
            if (!O) return;
            for (const auto& [Name, Images] : Book)
            {
                O << Name;
                for (const auto& I : Images) O << '\t' << (unsigned)I.Usage << '\t' << std::format("{:x}", I.Hash) << '\t' << I.Image;
                O << '\n';
            }
        }
        fs::rename(Aside, File, Ec);
        if (Ec) fs::remove(Aside, Ec);
    }
}

struct MaterialLookup::State
{
    const IDiskGame& Game;
    ExportOptions Options;
    std::string Install;
    const std::string& ReaderKey;
    Totals& T;
    std::mutex Lock;
    bool IndexTried = false;
    std::unique_ptr<ZoneIndex> Index;
    std::map<size_t, MaterialBook> Books;   // by the index's zone number, read or tried once
    State(const IDiskGame& G, const ExportOptions& O, std::string I, const std::string& K, Totals& T)
        : Game(G), Options(O), Install(std::move(I)), ReaderKey(K), T(T) {}

    fs::path BookFile(size_t Z) const
    {
        std::error_code Ec;
        const auto Time = fs::last_write_time(Index->File, Ec).time_since_epoch().count();
        const uint64_t Key = ExportNaming::Fnv1a(std::format("{}|{}|{}|{}|{}", Index->File, (long long)Time, ReaderKey, Index->Zones()[Z],
                                                             (unsigned long long)Index->Footprint(Z)));
        const fs::path Root = !Options.Cache.empty() ? fs::path(Options.Cache) / "material_books" : fs::temp_directory_path() / "greyhound_material_books";
        return Root / Game.Id() / std::format("{}_{:016x}.tsv", ExportNaming::SafeFileName(Stem(Index->Zones()[Z])), Key);
    }

    // the zone's book, from disk or its zone opened now (the asking zone is parked)
    const MaterialBook& Book(size_t Z)
    {
        if (const auto It = Books.find(Z); It != Books.end()) return It->second;
        MaterialBook& Out = Books[Z];
        const fs::path File = BookFile(Z);
        if (ReadBook(File, Out))
        {
            T.Counters["material_books_read"]++;
            return Out;
        }
        const std::string& ZoneName = Index->Zones()[Z];
        std::string Error;
        if (!MemoryBudget::CheckFits(Index->Footprint(Z), ZoneName).empty()) return Out;
        std::vector<std::string> Paths;
        if (!Game.ResolveZones(Install, { ZoneName }, Options, Paths, Error) || Paths.empty()) return Out;
        auto Zone = Game.Open(Paths.front(), Options, Error);
        if (!Zone || !Zone->MaterialImages(Out)) return Out;
        Zone.reset();
        T.Counters["material_books_made"]++;
        WriteBook(File, Out);
        return Out;
    }
};

MaterialLookup::MaterialLookup(const IDiskGame& Game, const ExportOptions& Options, const std::string& Install,
                               const std::string& ReaderKey, Totals& T)
    : S(std::make_shared<State>(Game, Options, Install, ReaderKey, T))
{
}

void MaterialLookup::operator()(IDiskZone& Asking, const std::set<std::string>& Names, MaterialBook& Out) const
{
    State& St = *S;
    std::lock_guard<std::mutex> Guard(St.Lock);
    if (Names.empty() || St.Install.empty()) return;
    // the asking zone set aside while the index or another zone opens: one session at a time
    bool Parked = false;
    const auto Park = [&] {
        if (!Parked) Parked = Asking.Park();
        return Parked;
    };
    if (!St.IndexTried)
    {
        St.IndexTried = true;
        if (!Park()) return;
        std::string Error;
        St.Index = ZoneIndex::For(St.Game, St.Options, St.Install, St.ReaderKey, nullptr, nullptr, Error);
    }
    if (St.Index)
    {
        // each name's defining zones, the asking one left out; then the zone giving the most first
        const std::string Self = Stem(Asking.Name());
        std::map<std::string, std::vector<size_t>> Candidates;
        for (const auto& N : Names)
        {
            std::vector<size_t> Zs;
            for (size_t Z : St.Index->DefinedIn({ Kind::Material, N }))
                if (Stem(St.Index->Zones()[Z]) != Self) Zs.push_back(Z);
            if (Zs.empty()) St.T.Counters["material_headers_not_indexed"]++;
            else Candidates.emplace(N, std::move(Zs));
        }
        while (!Candidates.empty())
        {
            // a book already read first, else the zone that defines the most of them
            std::map<size_t, size_t> Gives;
            for (const auto& [N, Zs] : Candidates)
                for (size_t Z : Zs) Gives[Z] += St.Books.count(Z) ? Names.size() + 1 : 1;
            const size_t Best = std::max_element(Gives.begin(), Gives.end(), [](const auto& A, const auto& B) { return A.second < B.second; })->first;
            if (!St.Books.count(Best) && !fs::exists(St.BookFile(Best)) && !Park()) break;
            const MaterialBook& Book = St.Book(Best);
            for (auto It = Candidates.begin(); It != Candidates.end();)
            {
                auto& Zs = It->second;
                if (std::find(Zs.begin(), Zs.end(), Best) == Zs.end())
                {
                    ++It;
                    continue;
                }
                if (const auto B = Book.find(It->first); B != Book.end() && !B->second.empty())
                {
                    Out[It->first] = B->second;
                    It = Candidates.erase(It);
                    continue;
                }
                // not read there: another zone that defines it, if any
                Zs.erase(std::find(Zs.begin(), Zs.end(), Best));
                It = Zs.empty() ? Candidates.erase(It) : std::next(It);
            }
        }
    }
    if (Parked) Asking.Resume();
    St.T.Counters["material_headers_filled"] += Out.size();
}
}
