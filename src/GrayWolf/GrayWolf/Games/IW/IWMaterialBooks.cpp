#include "stdafx.h"

#include "Games/IW/IWMaterialBooks.h"

#include "ExportNaming.h"
#include "ExportWriters.h"
#include "FileSystems.h"
#include "MappedFile.h"
#include "FastFile.h"
#include "ZonekitZone.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>
#include <string_view>

namespace fs = std::filesystem;

namespace IWMaterialBooks
{
    namespace
    {
        bool ReadZoneMaterials(const fs::path& Ff, const ExportWriters::LoadedProfile& Loaded, const Layout& L,
                               std::unordered_map<std::string, ImageList>& Out, std::unordered_map<std::string, Extra>& More)
        {
            ZoneBytes Z;
            std::string Error;
            if (!FastFile::Decompress(Ff.string(), Z, Error)) return false;
            const auto& P = Loaded.Profile;
            const auto R = ZoneTables::WalkZone(Z, Loaded.Tables, P.TypeNames, P.Rules, true);
            if (R.ParseFailed) return false;
            // where each pointer field points once relocated (the last link of a field, as served;
            // a hashed title's as a zone served beside resolves its own, IwBesideZones.cpp)
            std::unordered_map<int64_t, int64_t> Target;
            if (L.Hashed)
                for (const auto& [Field, To] : P.Rules.RewindRefs ? R.Links : ResolveLinks(R, P.Rules, false, Z).Links) Target[Field] = To;
            else
                for (const auto& [Field, To] : ResolveLinks(R, P.Rules, true, Z).Links) Target[Field] = To;
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
                    const bool Indexed = L.Slots >= 0;   // else records {u32 semantic, image* SlotImage} at Images
                    const int64_t Slots = Indexed ? Ptr(I.Pos + L.Slots) : -1, Table = Ptr(I.Pos + L.Images);
                    const uint8_t Count = ExportNaming::At<uint8_t>(Z, I.Pos + L.ImageCount);
                    const uint8_t Pointers = Indexed ? ExportNaming::At<uint8_t>(Z, I.Pos + L.TableCount) : 0;
                    std::string Name = Hex(I.Pos);
                    if (Count == 0 || (Indexed && Slots < 0) || Table < 0 || Out.count(Name)) continue;
                    ImageList Images;
                    for (int64_t K = 0; K < Count; K++)
                    {
                        uint32_t Semantic = 0;
                        int64_t Field = -1;
                        if (Indexed)
                        {
                            if (Slots + K * 2 + 2 > (int64_t)Z.size()) break;
                            const uint8_t Index = Z[(size_t)(Slots + K * 2 + 1)];
                            if (Index >= Pointers) continue;
                            Semantic = Z[(size_t)(Slots + K * 2)], Field = Table + Index * 8;
                        }
                        else
                        {
                            const int64_t Slot = Table + K * L.SlotSize;
                            if (Slot + L.SlotSize > (int64_t)Z.size()) break;
                            Semantic = ExportNaming::At<uint32_t>(Z, Slot), Field = Slot + L.SlotImage;
                        }
                        if (Field + 8 > (int64_t)Z.size()) continue;
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

    namespace
    {
        // bumped when what a kept book holds changes
        constexpr char BookMagic[8] = { 'G', 'W', 'B', 'O', 'O', 'K', '1', '\n' };

        // A book's materials as read (a zone's walk, or a text book of before), before it is kept
        struct Materials
        {
            std::unordered_map<std::string, ImageList> Images;
            std::unordered_map<std::string, Extra> More;
        };

        // The text book of before: a line per material, "name<TAB>semantic<TAB>image..." (semantic
        // in hex); "@<TAB>rows,columns,techset" first when it has extras, "%<TAB>key" after it
        bool ReadTextBook(const fs::path& File, Materials& Out)
        {
            std::ifstream In(File);
            if (!In) return false;
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
                        if (const auto E = Out.More.find(Name); E != Out.More.end()) E->second.SortKey = std::stoi(Img);
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
                        Out.More.emplace(Name, std::move(E));
                    }
                }
                Out.Images.emplace(Name, std::move(Images));
            }
            return true;
        }

        // A kept book: the magic, u32 count, the u32 offsets of the materials sorted by name, then
        // each material {u16 length, name, u8 1 when extras follow: {u8 rows, u8 columns, i32 sort
        // key, u16 length, techset}, u16 count, {u32 semantic, u16 length, image} per image}
        bool WriteBook(const fs::path& File, const Materials& M)
        {
            std::vector<const std::string*> Names;
            Names.reserve(M.Images.size());
            for (const auto& [Name, Images] : M.Images) Names.push_back(&Name);
            std::sort(Names.begin(), Names.end(), [](const std::string* A, const std::string* B) { return *A < *B; });
            std::string Body;
            std::vector<uint32_t> Offsets;
            const size_t Head = sizeof(BookMagic) + 4 + 4 * Names.size();
            const auto Put = [&](const void* P, size_t N) { Body.append(static_cast<const char*>(P), N); };
            const auto Text = [&](const std::string& S) {
                const uint16_t N = (uint16_t)std::min<size_t>(S.size(), 0xffff);
                Put(&N, 2);
                Put(S.data(), N);
            };
            for (const std::string* Name : Names)
            {
                if (Head + Body.size() > 0xffffffffull) return false;
                Offsets.push_back((uint32_t)(Head + Body.size()));
                Text(*Name);
                const auto E = M.More.find(*Name);
                const uint8_t HasExtra = E != M.More.end();
                Put(&HasExtra, 1);
                if (HasExtra)
                {
                    Put(&E->second.Rows, 1);
                    Put(&E->second.Cols, 1);
                    const int32_t SortKey = E->second.SortKey;
                    Put(&SortKey, 4);
                    Text(E->second.Techset);
                }
                const ImageList& Images = M.Images.at(*Name);
                const uint16_t Count = (uint16_t)std::min<size_t>(Images.size(), 0xffff);
                Put(&Count, 2);
                for (size_t K = 0; K < Count; K++)
                {
                    Put(&Images[K].first, 4);
                    Text(Images[K].second);
                }
            }
            // another process may read the same book meanwhile
            return FileSystems::WriteReplacing(File, [&](std::ostream& O) {
                O.write(BookMagic, sizeof(BookMagic));
                const uint32_t Count = (uint32_t)Offsets.size();
                O.write(reinterpret_cast<const char*>(&Count), 4);
                O.write(reinterpret_cast<const char*>(Offsets.data()), (std::streamsize)(Offsets.size() * 4));
                O.write(Body.data(), (std::streamsize)Body.size());
            });
        }
    }

    // A kept book read in place: a lookup compares the names it meets in a binary search, and reads
    // the one material it finds
    class Book
    {
    public:
        static std::shared_ptr<const Book> Open(const fs::path& File)
        {
            MappedFile Map;
            if (!Map.Open(File) || Map.size() < sizeof(BookMagic) + 4) return nullptr;
            auto B = std::shared_ptr<Book>(new Book(std::move(Map)));
            if (std::memcmp(B->Base, BookMagic, sizeof(BookMagic)) != 0) return nullptr;
            std::memcpy(&B->Count, B->Base + sizeof(BookMagic), 4);
            if (B->Count > (B->Size - sizeof(BookMagic) - 4) / 4) return nullptr;
            return B;
        }
        Book(const Book&) = delete;
        Book& operator=(const Book&) = delete;

        bool Images(const std::string& Name, ImageList& Out) const
        {
            Reader R;
            if (!Entry(Name, R)) return false;
            uint8_t HasExtra = 0;
            // (its extras skipped: rows, columns, sort key, techset)
            if (!R.Take(HasExtra) || (HasExtra && (!R.Skip(6) || !R.Text(nullptr)))) return false;
            uint16_t Count = 0;
            if (!R.Take(Count)) return false;
            ImageList Images;
            Images.reserve(Count);
            for (uint16_t K = 0; K < Count; K++)
            {
                uint32_t Semantic = 0;
                std::string Image;
                if (!R.Take(Semantic) || !R.Text(&Image)) return false;
                Images.emplace_back(Semantic, std::move(Image));
            }
            Out = std::move(Images);
            return true;
        }
        bool Details(const std::string& Name, Extra& Out) const
        {
            Reader R;
            uint8_t HasExtra = 0;
            if (!Entry(Name, R) || !R.Take(HasExtra) || !HasExtra) return false;
            Extra E;
            int32_t SortKey = -1;
            if (!R.Take(E.Rows) || !R.Take(E.Cols) || !R.Take(SortKey) || !R.Text(&E.Techset)) return false;
            E.SortKey = SortKey;
            Out = std::move(E);
            return true;
        }

    private:
        explicit Book(MappedFile M) : Map(std::move(M)), Base(Map.data()), Size(Map.size()) {}
        struct Reader
        {
            const uint8_t* At = nullptr;
            const uint8_t* End = nullptr;
            template <class T>
            bool Take(T& Out)
            {
                if ((size_t)(End - At) < sizeof(T)) return false;
                std::memcpy(&Out, At, sizeof(T));
                At += sizeof(T);
                return true;
            }
            bool Skip(size_t N)
            {
                if ((size_t)(End - At) < N) return false;
                At += N;
                return true;
            }
            // a u16-prefixed string, into Out when given
            bool Text(std::string* Out)
            {
                uint16_t N = 0;
                if (!Take(N) || (size_t)(End - At) < N) return false;
                if (Out != nullptr) Out->assign(reinterpret_cast<const char*>(At), N);
                At += N;
                return true;
            }
        };
        // the material named Name, R after its name
        bool Entry(const std::string& Name, Reader& R) const
        {
            const uint8_t* Offsets = Base + sizeof(BookMagic) + 4;
            size_t Low = 0, High = Count;
            while (Low < High)
            {
                const size_t Mid = (Low + High) / 2;
                uint32_t Offset = 0;
                std::memcpy(&Offset, Offsets + Mid * 4, 4);
                R.At = Base + std::min<size_t>(Offset, Size);
                R.End = Base + Size;
                uint16_t N = 0;
                if (!R.Take(N) || (size_t)(R.End - R.At) < N) return false;
                const int C = std::string_view(reinterpret_cast<const char*>(R.At), N).compare(Name);
                if (C == 0)
                {
                    R.At += N;
                    return true;
                }
                if (C < 0) Low = Mid + 1;
                else High = Mid;
            }
            return false;
        }
        MappedFile Map;
        const uint8_t* Base;
        size_t Size;
        uint32_t Count = 0;
    };

    std::shared_ptr<const Book> ZoneBook(const fs::path& Ff, const std::string& CacheDir, const Layout& L)
    {
        static std::mutex Lock;
        static std::map<std::string, std::shared_ptr<const Book>> Read;
        std::lock_guard<std::mutex> Guard(Lock);
        std::string Error;
        const auto* Loaded = ExportWriters::ZoneProfile(L.Profile, Error);
        std::error_code Ec;
        const auto Size = fs::file_size(Ff, Ec);
        if (Loaded == nullptr || Ec) return nullptr;
        const auto Time = fs::last_write_time(Ff, Ec).time_since_epoch().count();
        const std::string Stem = std::format("{}_{:x}_{:x}_{}{}", Ff.stem().string().c_str(), (unsigned long long)Size, (unsigned long long)Time,
                                             ProfileKey(*Loaded).c_str(),
                                             L.Hashed ? (L.WithExtras() ? "_h3" : "_h2") : !L.WithExtras() ? "" : L.SortKey >= 0 ? "_x2" : "_x1");
        const fs::path File = fs::path(CacheDir) / (Stem + ".gwb");
        if (const auto It = Read.find(File.string()); It != Read.end()) return It->second;
        if (auto B = Book::Open(File)) return Read[File.string()] = B;
        // the zone's walk, or the text book a run of before kept, then kept as a book read in place
        Materials M;
        if (!ReadTextBook(fs::path(CacheDir) / (Stem + ".tsv"), M) && !ReadZoneMaterials(Ff, *Loaded, L, M.Images, M.More)) return nullptr;
        if (!WriteBook(File, M)) return nullptr;
        auto B = Book::Open(File);
        if (B != nullptr) Read[File.string()] = B;
        return B;
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
            if (auto B = ZoneBook(F, CacheDir, L)) Books.push_back(std::move(B));
    }

    bool Lookup::Details(const std::string& Name, Extra& Out)
    {
        Open();
        for (const auto& B : Books)
            if (B->Details(Name, Out)) return true;
        return false;
    }

    bool Lookup::Find(const std::string& Name, ImageList& Out)
    {
        Open();
        for (const auto& B : Books)
            if (B->Images(Name, Out))
            {
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
