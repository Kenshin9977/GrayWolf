#include "stdafx.h"

#include "Games/IW/IWMaterialBooks.h"

#include "ExportNaming.h"
#include "ExportWriters.h"
#include "FastFile.h"
#include "ZonekitZone.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace IWMaterialBooks
{
    namespace
    {
        bool ReadZoneMaterials(const fs::path& Ff, const ExportWriters::LoadedProfile& Loaded, const Layout& L, Book& Out, Extras& More)
        {
            std::vector<uint8_t> Z;
            std::string Error;
            if (!FastFile::Decompress(Ff.string(), Z, Error)) return false;
            const auto& P = Loaded.Profile;
            const auto R = ZoneTables::WalkZone(Z, Loaded.Tables, P.TypeNames, P.Rules, true);
            if (R.ParseFailed) return false;
            // where each pointer field points once relocated (the last link of a field, as served;
            // a hashed title's as a zone served beside resolves its own, IwBesideZones.cpp)
            std::unordered_map<int64_t, int64_t> Target;
            if (L.Hashed)
                for (const auto& [Field, To] : P.Rules.RewindRefs ? R.Links : ResolveLinks(R, P.Rules, false, &Z).Links) Target[Field] = To;
            else
                for (const auto& [Field, To] : ResolveLinks(R, P.Rules, true, &Z).Links) Target[Field] = To;
            const auto Ptr = [&](int64_t Field) {
                const auto It = Target.find(Field);
                return It == Target.end() ? (int64_t)-1 : It->second;
            };
            const auto Str = [&](int64_t Field) {
                std::string S;
                for (int64_t K = Ptr(Field); K >= 0 && K < (int64_t)Z.size() && Z[(size_t)K]; K++) S.push_back((char)Z[(size_t)K]);
                return S;
            };
            const auto Unprefixed = [](std::string S) { return !S.empty() && S[0] == ',' ? S.substr(1) : S; };
            const auto Hex = [&](int64_t Pos) { return std::format("{:x}", ExportNaming::At<uint64_t>(Z, Pos) & L.HashMask); };
            for (const auto& I : R.Instances)
            {
                if (I.Owner >= R.OwnerNames.size() || R.OwnerNames[I.Owner] != "material") continue;
                if (L.Hashed)
                {
                    const int64_t Slots = Ptr(I.Pos + L.Slots), Table = Ptr(I.Pos + L.Images);
                    const uint8_t Count = ExportNaming::At<uint8_t>(Z, I.Pos + L.ImageCount), Pointers = ExportNaming::At<uint8_t>(Z, I.Pos + L.TableCount);
                    std::string Name = Hex(I.Pos);
                    if (Count == 0 || Slots < 0 || Table < 0 || Out.count(Name)) continue;
                    ImageList Images;
                    for (int64_t K = 0; K < Count && Slots + K * 2 + 2 <= (int64_t)Z.size(); K++)
                    {
                        const uint8_t Semantic = Z[(size_t)(Slots + K * 2)], Index = Z[(size_t)(Slots + K * 2 + 1)];
                        const int64_t Field = Table + Index * 8;
                        if (Index >= Pointers || Field + 8 > (int64_t)Z.size()) continue;
                        // the image's hash (a pointer the zone leaves unresolved: the value), all ones for none
                        const int64_t Img = Ptr(Field);
                        const uint64_t V = ExportNaming::At<uint64_t>(Z, Img >= 0 ? Img : Field);
                        if ((Img >= 0 || V >> 63) && (V & L.HashMask) != L.HashMask) Images.emplace_back(Semantic, Hex(Img >= 0 ? Img : Field));
                    }
                    if (L.WithExtras())
                    {
                        Extra E;
                        if (const int64_t T = L.Techset >= 0 ? Ptr(I.Pos + L.Techset) : -1; T >= 0) E.Techset = Hex(T);
                        E.SortKey = ExportNaming::At<uint8_t>(Z, I.Pos + L.SortKey);
                        More.emplace(Name, std::move(E));
                    }
                    Out.emplace(std::move(Name), std::move(Images));
                    continue;
                }
                const int64_t Table = Ptr(I.Pos + L.Images);
                const uint8_t Count = ExportNaming::At<uint8_t>(Z, I.Pos + L.ImageCount);
                std::string Name = Str(I.Pos);
                if (L.WithSlotsOnly)
                {
                    if (Count == 0 || Table < 0) continue;
                    Name = Unprefixed(Name);
                }
                else if (!Name.empty() && Name[0] == ',')
                    continue;
                if (Name.empty() || Out.count(Name)) continue;
                ImageList Images;
                for (int64_t K = 0; Table >= 0 && K < Count; K++)
                {
                    const int64_t Slot = Table + K * L.SlotSize;
                    const int64_t Img = Ptr(Slot + L.SlotImage);
                    std::string N = Img < 0 ? std::string() : Unprefixed(Str(Img + L.ImageName));
                    if (!N.empty()) Images.emplace_back(ExportNaming::At<uint32_t>(Z, Slot), std::move(N));
                }
                if (L.WithExtras())
                {
                    Extra E;
                    E.Rows = ExportNaming::At<uint8_t>(Z, I.Pos + L.AtlasRows), E.Cols = ExportNaming::At<uint8_t>(Z, I.Pos + L.AtlasCols);
                    if (const int64_t T = L.Techset >= 0 ? Ptr(I.Pos + L.Techset) : -1; T >= 0) E.Techset = Unprefixed(Str(T));
                    if (L.SortKey >= 0) E.SortKey = ExportNaming::At<uint8_t>(Z, I.Pos + L.SortKey);
                    More.emplace(Name, std::move(E));
                }
                Out.emplace(std::move(Name), std::move(Images));
            }
            return true;
        }
    }

    std::shared_ptr<const Book> ZoneBook(const fs::path& Ff, const std::string& CacheDir, const Layout& L)
    {
        std::shared_ptr<const Extras> More;
        return ZoneBook(Ff, CacheDir, L, More);
    }

    // A line per material, "name<TAB>semantic<TAB>image..." (semantic in hex); a layout reading the
    // extras puts "@<TAB>rows,columns,techset" first and keeps its books under another name ("_x1"),
    // one reading the sort key "%<TAB>key" after it ("_x2"); a hashed one's names are hex ("_h2", "_h3" with its extras)
    std::shared_ptr<const Book> ZoneBook(const fs::path& Ff, const std::string& CacheDir, const Layout& L, std::shared_ptr<const Extras>& MoreOut)
    {
        static std::mutex Lock;
        static std::map<std::string, std::shared_ptr<const Book>> Read;
        static std::map<std::string, std::shared_ptr<const Extras>> ReadMore;
        std::lock_guard<std::mutex> Guard(Lock);
        std::string Error;
        const auto* Loaded = ExportWriters::ZoneProfile(L.Profile, Error);
        std::error_code Ec;
        const auto Size = fs::file_size(Ff, Ec);
        if (Loaded == nullptr || Ec) return nullptr;
        const auto Time = fs::last_write_time(Ff, Ec).time_since_epoch().count();
        const fs::path File = fs::path(CacheDir) / std::format("{}_{:x}_{:x}_{}{}.tsv", Ff.stem().string().c_str(), (unsigned long long)Size,
                                                                   (unsigned long long)Time, ProfileKey(*Loaded).c_str(),
                                                                   L.Hashed ? (L.WithExtras() ? "_h3" : "_h2") : !L.WithExtras() ? "" : L.SortKey >= 0 ? "_x2" : "_x1");
        if (const auto It = Read.find(File.string()); It != Read.end())
        {
            MoreOut = ReadMore[File.string()];
            return It->second;
        }
        auto Out = std::make_shared<Book>();
        auto More = std::make_shared<Extras>();
        if (std::ifstream In(File); In)
        {
            for (std::string Line; std::getline(In, Line);)
            {
                std::istringstream Fields(Line);
                std::string Name, Sem, Img;
                if (!std::getline(Fields, Name, '\t')) continue;
                ImageList Images;
                while (std::getline(Fields, Sem, '\t') && std::getline(Fields, Img, '\t'))
                {
                    if (Sem == "%")
                    {
                        if (const auto E = More->find(Name); E != More->end()) E->second.SortKey = std::stoi(Img);
                        continue;
                    }
                    if (Sem != "@")
                    {
                        Images.emplace_back((uint32_t)std::stoul(Sem, nullptr, 16), Img);
                        continue;
                    }
                    // rows,columns,techset
                    Extra E;
                    std::istringstream Parts(Img);
                    std::string Rows, Cols;
                    if (std::getline(Parts, Rows, ',') && std::getline(Parts, Cols, ','))
                    {
                        std::getline(Parts, E.Techset);
                        E.Rows = (uint8_t)std::stoul(Rows), E.Cols = (uint8_t)std::stoul(Cols);
                        More->emplace(Name, std::move(E));
                    }
                }
                Out->emplace(Name, std::move(Images));
            }
            MoreOut = ReadMore[File.string()] = More;
            return Read[File.string()] = Out;
        }
        if (!ReadZoneMaterials(Ff, *Loaded, L, *Out, *More)) return nullptr;
        // written aside then renamed: another process may read the same book meanwhile
        fs::create_directories(CacheDir, Ec);
        if (std::ofstream O(File.string() + ".tmp", std::ios::binary); O)
        {
            for (const auto& [Name, Images] : *Out)
            {
                O << Name;
                if (const auto E = More->find(Name); E != More->end())
                {
                    O << '\t' << '@' << '\t' << (unsigned)E->second.Rows << ',' << (unsigned)E->second.Cols << ',' << E->second.Techset;
                    if (E->second.SortKey >= 0) O << "\t%\t" << E->second.SortKey;
                }
                for (const auto& [Sem, Img] : Images) O << '\t' << std::format("{:x}", Sem) << '\t' << Img;
                O << '\n';
            }
            O.close();
            fs::rename(File.string() + ".tmp", File, Ec);
        }
        MoreOut = ReadMore[File.string()] = More;
        return Read[File.string()] = Out;
    }

    std::vector<fs::path> ZonesMatching(const fs::path& Dir, const std::string& Pattern)
    {
        const std::regex Match(Pattern, std::regex::icase);
        std::vector<fs::path> Zones;
        std::error_code Ec;
        for (const auto& E : fs::directory_iterator(Dir, Ec))
            if (E.is_regular_file() && std::regex_match(E.path().filename().string(), Match)) Zones.push_back(E.path());
        std::sort(Zones.begin(), Zones.end(), [](const fs::path& A, const fs::path& B) {
            const bool Pa = A.filename().string().rfind("patch_", 0) == 0, Pb = B.filename().string().rfind("patch_", 0) == 0;
            return Pa != Pb ? Pa : A.filename() < B.filename();
        });
        return Zones;
    }

    fs::path InstallDir(const std::string& FastFile, const char* Marker)
    {
        const fs::path Dir = fs::path(FastFile).parent_path();
        std::error_code Ec;
        if (!fs::exists(Dir / Marker, Ec) && fs::exists(Dir.parent_path() / Marker, Ec)) return Dir.parent_path();
        return Dir;
    }

    Lookup::Lookup(Layout L, std::string CacheDir, ZoneList Zones) : L(L), CacheDir(std::move(CacheDir)), Zones(std::move(Zones)) {}

    void Lookup::Open()
    {
        if (Read) return;
        Read = true;
        for (const auto& F : Zones())
        {
            std::shared_ptr<const Extras> E;
            if (auto B = ZoneBook(F, CacheDir, L, E))
            {
                Books.push_back(std::move(B));
                if (E) More.push_back(std::move(E));
            }
        }
    }

    bool Lookup::Details(const std::string& Name, Extra& Out)
    {
        Open();
        for (const auto& E : More)
            if (const auto It = E->find(Name); It != E->end())
            {
                Out = It->second;
                return true;
            }
        return false;
    }

    bool Lookup::Find(const std::string& Name, ImageList& Out)
    {
        Open();
        for (const auto& B : Books)
            if (const auto It = B->find(Name); It != B->end())
            {
                Out = It->second;
                Booked.insert(Name);
                return true;
            }
        NotBooked.insert(Name);
        return false;
    }

    void Lookup::Measure(DiskGame::Totals& T, const char* Filled, const char* Unfilled) const
    {
        if (!Booked.empty()) T.Counters[Filled] += Booked.size();
        if (!NotBooked.empty()) T.Counters[Unfilled] += NotBooked.size();
    }
}
