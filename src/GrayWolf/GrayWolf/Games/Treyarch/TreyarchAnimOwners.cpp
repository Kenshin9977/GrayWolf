#include "stdafx.h"

#include "TreyarchAnimOwners.h"

#include "AnimOwners.h"
#include "ExportNaming.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>

#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <windows.h>

namespace fs = std::filesystem;

namespace TreyarchAnimOwners
{
    namespace
    {
        using ExportNaming::At;
        using ExportNaming::Lower;

        // the kept file's first line; a new reader version reads every zone again
        constexpr const char* KeptHeader = "graywolf-anim-owner-facts 3";

        void Add(std::vector<std::string>& To, const std::string& S)
        {
            if (!S.empty() && std::find(To.begin(), To.end(), S) == To.end()) To.push_back(S);
        }

        std::vector<std::string> Split(const std::string& S, char By)
        {
            std::vector<std::string> Out;
            size_t From = 0;
            for (size_t K = S.find(By); K != std::string::npos; K = S.find(By, From))
            {
                Out.push_back(S.substr(From, K - From));
                From = K + 1;
            }
            Out.push_back(S.substr(From));
            return Out;
        }

        std::string Join(const std::vector<std::string>& V)
        {
            std::string S;
            for (const auto& E : V) S += (S.empty() ? "" : "|") + E;
            return S;
        }

        // '*' any run, no case
        bool Fits(const char* N, const char* P)
        {
            if (*P == 0) return *N == 0;
            if (*P == '*') return Fits(N, P + 1) || (*N != 0 && Fits(N + 1, P));
            return *N != 0 && std::tolower((unsigned char)*N) == std::tolower((unsigned char)*P) && Fits(N + 1, P + 1);
        }

        std::string Stem(const std::string& Zone) { return Lower(fs::path(Zone).stem().string()); }

        // The anims Sources' owners play among Anims (the zone's first), each owner's models its own or
        // its characters' (from the first source defining them), its anims its own and its maps'. An
        // anim's owners the most specific first: those playing fewer anims in all (Cold War's generic
        // zombie anims are the catalysts' too, whose maps add their own: zombie before catalyst)
        void Own(const std::vector<const Facts*>& Sources, const std::vector<std::string>& Anims, AnimOwners::Book& Out)
        {
            struct Giving
            {
                const Owner* O = nullptr;
                std::vector<std::string> Models;
                std::vector<const std::string*> Played;
                size_t Total = 0;
            };
            std::vector<Giving> All;
            std::unordered_map<std::string, const std::string*> Wanted;
            for (const auto& A : Anims) Wanted.emplace(Lower(A), &A);
            std::unordered_map<uint64_t, const std::vector<std::string>*> Characters, Maps;
            for (const Facts* S : Sources)
            {
                for (const auto& [H, M] : S->Characters) Characters.emplace(H, &M);
                for (const auto& [H, M] : S->Maps) Maps.emplace(H, &M);
            }
            std::set<std::string> Seen;
            for (const Facts* S : Sources)
                for (const auto& O : S->Owners)
                {
                    if (!Seen.insert(O.Id).second) continue;
                    std::vector<std::string> Models = O.Models;
                    for (const uint64_t C : O.Characters)
                        if (const auto It = Characters.find(C); It != Characters.end())
                            for (const auto& M : *It->second) Add(Models, M);
                    if (Models.empty()) continue;
                    Giving G{ &O, std::move(Models) };
                    const auto Play = [&](const std::string& A) {
                        G.Total++;
                        if (const auto It = Wanted.find(Lower(A)); It != Wanted.end()) G.Played.push_back(It->second);
                    };
                    for (const auto& A : O.Anims) Play(A);
                    for (const uint64_t M : O.Maps)
                        if (const auto It = Maps.find(M); It != Maps.end())
                            for (const auto& A : *It->second) Play(A);
                    if (!G.Played.empty()) All.push_back(std::move(G));
                }
            std::stable_sort(All.begin(), All.end(), [](const Giving& A, const Giving& B) { return A.Total < B.Total; });
            for (auto& G : All)
            {
                Out.Owners.try_emplace(G.O->Id, AnimOwners::Owner{ std::move(G.Models), G.O->Source });
                for (const std::string* A : G.Played) Out.Own(*A, G.O->Id);
            }
        }

        // The facts of an install's zones, kept in a file: zone stem -> facts, read once per process
        class Kept
        {
        public:
            explicit Kept(std::string Path) : File(std::move(Path))
            {
                std::ifstream In(File, std::ios::binary);
                std::string Line, Zone, Body;
                if (!std::getline(In, Line) || Trim(Line) != KeptHeader) return;
                while (std::getline(In, Line))
                {
                    Trim(Line);
                    if (Line.rfind("zone\t", 0) == 0)
                    {
                        Zone = Line.substr(5);
                        Body.clear();
                    }
                    else if (Line == "end" && !Zone.empty())
                    {
                        Zones[Zone] = std::make_shared<const Facts>(Parse(Body));
                        Zone.clear();
                    }
                    else Body += Line + "\n";
                }
            }
            std::shared_ptr<const Facts> Find(const std::string& Zone, const std::string& Key) const
            {
                const auto It = Zones.find(Zone);
                return It != Zones.end() && It->second->Key == Key ? It->second : nullptr;
            }
            // kept, the file written to a temporary one, then moved over the old one
            void Keep(const std::string& Zone, std::shared_ptr<const Facts> F)
            {
                Zones[Zone] = std::move(F);
                const std::string Temp = File + ".tmp";
                {
                    std::ofstream O(Temp, std::ios::binary | std::ios::trunc);
                    O << KeptHeader << "\n";
                    for (const auto& [Z, Each] : Zones) O << "zone\t" << Z << "\n" << Text(*Each) << "end\n";
                    if (!O) return;
                }
                MoveFileExA(Temp.c_str(), File.c_str(), MOVEFILE_REPLACE_EXISTING);
            }

        private:
            static std::string& Trim(std::string& S)
            {
                if (!S.empty() && S.back() == '\r') S.pop_back();
                return S;
            }
            std::string File;
            std::map<std::string, std::shared_ptr<const Facts>> Zones;
        };

        Kept& KeptFor(const std::string& File)
        {
            static std::map<std::string, std::unique_ptr<Kept>> All;
            auto& K = All[Lower(File)];
            if (!K) K = std::make_unique<Kept>(File);
            return *K;
        }

        const std::string* DocumentOf(const std::vector<std::pair<std::string, std::string>>& Docs)
        {
            for (const auto& [N, T] : Docs)
                if (N == DocumentName) return &T;
            return nullptr;
        }

        // A zone of the family: its stem, and its name as ListZones gives it (or its path, beside the zone)
        struct Candidate
        {
            std::string Stem, Name;
        };
        // The install's zones (without one, those beside Zone) fitting Patterns but Zone, in the
        // patterns' order (zm_common before the maps), then by name
        std::vector<Candidate> Candidates(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& Options,
                                          const std::string& Zone, const std::vector<std::string>& Patterns)
        {
            std::vector<Candidate> Out;
            const std::string Mine = Stem(Zone);
            const auto Rank = [&](const std::string& S) {
                for (const auto& P : Patterns)
                    if (P[0] == '!' && Fits(S.c_str(), P.c_str() + 1)) return Patterns.size();
                for (size_t K = 0; K < Patterns.size(); K++)
                    if (Patterns[K][0] != '!' && Fits(S.c_str(), Patterns[K].c_str())) return K;
                return Patterns.size();
            };
            const auto Wanted = [&](const std::string& S) { return S != Mine && Rank(S) < Patterns.size(); };
            std::vector<std::string> Names;
            std::string Error;
            if (!Options.Install.empty() && Game.ListZones(Options.Install, Options, Names, Error))
            {
                for (const auto& N : Names)
                    if (Wanted(Stem(N))) Out.push_back({ Stem(N), N });
            }
            else
            {
                std::error_code Ec;
                for (auto It = fs::directory_iterator(fs::path(Zone).parent_path(), Ec); !Ec && It != fs::directory_iterator(); It.increment(Ec))
                    if (Lower(It->path().extension().string()) == ".ff" && Wanted(Stem(It->path().string())))
                        Out.push_back({ Stem(It->path().string()), It->path().string() });
            }
            std::stable_sort(Out.begin(), Out.end(), [&](const Candidate& A, const Candidate& B) {
                return Rank(A.Stem) != Rank(B.Stem) ? Rank(A.Stem) < Rank(B.Stem) : A.Stem < B.Stem;
            });
            return Out;
        }

        // a candidate's facts: kept, else its zone pulled (with --install), opened and read
        std::shared_ptr<const Facts> FactsOf(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& Options, Kept& K,
                                             const Candidate& C, const std::string& Key)
        {
            if (auto F = K.Find(C.Stem, Key)) return F;
            std::string Path = C.Name, Error;
            if (!Options.Install.empty())
            {
                std::vector<std::string> Paths;
                if (!Game.ResolveZones(Options.Install, { C.Name }, Options, Paths, Error) || Paths.empty()) return nullptr;
                Path = Paths.front();
            }
            std::vector<std::pair<std::string, std::string>> Docs;
            if (auto Z = Game.Open(Path, Options, Error)) Z->Documents(Docs);
            const std::string* D = DocumentOf(Docs);
            if (D == nullptr) return nullptr;
            auto F = std::make_shared<const Facts>(Parse(*D));
            K.Keep(C.Stem, F);
            return F;
        }
    }

    Facts Read(const ZoneView& Z, const Layout& L)
    {
        Facts F;
        const auto& Img = *Z.Image;
        const auto U64 = [&](int64_t P) { return At<uint64_t>(Img, P); };
        const auto Set = [&](const char* Type) {
            const auto V = Z.Assets(Type);
            return std::unordered_set<int64_t>(V.begin(), V.end());
        };
        const auto Models = Set("xmodel"), Anims = Set("xanim"), Characters = Set("character"), Aliases = Set("xmodelalias"),
                   MapSet = Set("animmappingtable"), Uniques = Set(L.UniqueType);
        // the asset of a set a pointer names: the asset, or the slot holding its pointer
        const auto Find = [&](uint64_t Address, const std::unordered_set<int64_t>& S) -> int64_t {
            if (Address == 0) return -1;
            const int64_t P = Z.Off(Address);
            if (P < 0) return -1;
            if (S.count(P)) return P;
            const int64_t Q = Z.Off(U64(P));
            return Q >= 0 && S.count(Q) ? Q : -1;
        };
        const auto ModelOf = [&](uint64_t Address) {
            const int64_t P = Find(Address, Models);
            return P < 0 ? std::string() : Z.Model(P);
        };
        const auto AliasModels = [&](uint64_t Address, std::vector<std::string>& Out, bool First) {
            const int64_t A = Find(Address, Aliases);
            for (int K = 0; A >= 0 && K < L.AliasSlots; K++)
                if (const std::string M = ModelOf(U64(A + L.AliasModels + 8 * K)); !M.empty())
                {
                    Add(Out, M);
                    if (First) return;
                }
        };

        // characters: the body (its model, else its list's first), then the head(s)
        for (const int64_t C : Characters)
        {
            std::vector<std::string> M;
            if (const std::string B = ModelOf(U64(C + L.Body)); !B.empty()) M.push_back(B);
            else AliasModels(U64(C + L.BodyAlias), M, true);
            if (M.empty()) continue;
            Add(M, ModelOf(U64(C + L.Head)));
            AliasModels(U64(C + L.HeadAlias), M, false);
            F.Characters[U64(C + L.CharacterHash) & Z.HashMask] = std::move(M);
        }

        // anim maps: every entry's xanims
        for (const int64_t M : MapSet)
        {
            std::vector<std::string> A;
            const int64_t Entries = Z.Off(U64(M + L.MapEntries));
            const uint32_t Count = std::min<uint32_t>(At<uint32_t>(Img, M + L.MapCount), 100000);
            for (uint32_t E = 0; Entries >= 0 && E < Count; E++)
            {
                const int64_t Entry = Entries + (int64_t)E * L.EntrySize;
                const int64_t List = Z.Off(U64(Entry + L.EntryAnims));
                const uint32_t N = std::min<uint32_t>(At<uint32_t>(Img, Entry + L.EntryCount), 4096);
                for (uint32_t K = 0; List >= 0 && K < N; K++)
                {
                    const uint64_t V = U64(List + (int64_t)K * L.AnimStride);
                    if (L.AnimsHashed)
                    {
                        if (V & Z.HashMask) Add(A, Z.AnimHash(V & Z.HashMask));
                    }
                    else if (const int64_t P = Find(V, Anims); P >= 0) Add(A, Z.Anim(P));
                }
            }
            if (!A.empty()) F.Maps[U64(M + L.MapHash) & Z.HashMask] = std::move(A);
        }

        // AI types: their characters and maps, by name hash (another zone's when only named here)
        for (const int64_t A : Set("aitype"))
        {
            std::string Name = L.AiText >= 0 ? Z.Text(U64(A + L.AiText)) : std::string();
            if (Name.empty()) Name = Z.Hashed(U64(A + L.AiHash) & Z.HashMask, "aitype");
            Owner O{ "aitype:" + Name, "aitype " + Name };
            for (int K = 0; K < L.CharacterSlots; K++)
                if (const int64_t C = Find(U64(A + L.Characters + 8 * K), Characters); C >= 0)
                    O.Characters.push_back(U64(C + L.CharacterHash) & Z.HashMask);
            for (int K = 0; K < L.AnimMapSlots; K++)
                if (const int64_t M = Find(U64(A + L.AnimMaps + 8 * K), MapSet); M >= 0) O.Maps.push_back(U64(M + L.MapHash) & Z.HashMask);
            if (!O.Characters.empty() && !O.Maps.empty()) F.Owners.push_back(std::move(O));
        }

        // weapons: their view model, their anim tables but the third person's poses some hold (pt_, pb_:
        // the player's, Cold War's finger poses in the view table)
        const auto ThirdPerson = [](const std::string& N) { return N.rfind("pt_", 0) == 0 || N.rfind("pb_", 0) == 0; };
        for (const int64_t W : L.GunModel >= 0 ? Set("weapon") : std::unordered_set<int64_t>())
        {
            const std::string Gun = ModelOf(U64(W + L.GunModel));
            if (Gun.empty()) continue;
            const std::string Name = Z.Hashed(U64(W + L.WeaponHash) & Z.HashMask, "weapon");
            Owner O{ "weapon:" + Name, "weapon " + Name, { Gun } };
            const auto Tables = [&](int64_t Of, const std::vector<std::pair<int64_t, int>>& Fields) {
                for (const auto& [Field, Slots] : Fields)
                {
                    const int64_t Table = Z.Off(U64(Of + Field));
                    for (int K = 0; Table >= 0 && K < Slots; K++)
                        if (const int64_t P = Find(U64(Table + 8 * K), Anims); P >= 0)
                            if (const std::string N = Z.Anim(P); !ThirdPerson(N)) Add(O.Anims, N);
                }
            };
            Tables(W, L.WeaponAnims);
            const int64_t List = L.Uniques >= 0 ? Z.Off(U64(W + L.Uniques)) : -1;
            for (int K = 0; List >= 0 && K < L.UniqueSlots; K++)
                if (const int64_t U = Find(U64(List + 8 * K), Uniques); U >= 0) Tables(U, L.UniqueAnims);
            if (!O.Anims.empty()) F.Owners.push_back(std::move(O));
        }
        // in a stable order: the walk's sets are not
        std::sort(F.Owners.begin(), F.Owners.end(), [](const Owner& A, const Owner& B) { return A.Id < B.Id; });
        F.Key = Z.Key;
        return F;
    }

    // the key, then per owner "O id source" and its "M model", "C character", "A map", "X anim" lines,
    // then "c hash models" and "m hash anims" ('|' between names)
    std::string Text(const Facts& F)
    {
        std::ostringstream S;
        S << "key\t" << F.Key << "\n";
        for (const auto& O : F.Owners)
        {
            S << "O\t" << O.Id << "\t" << O.Source << "\n";
            for (const auto& M : O.Models) S << "M\t" << M << "\n";
            for (const auto C : O.Characters) S << "C\t" << std::format("{:x}", C) << "\n";
            for (const auto M : O.Maps) S << "A\t" << std::format("{:x}", M) << "\n";
            for (const auto& A : O.Anims) S << "X\t" << A << "\n";
        }
        for (const auto& [H, M] : F.Characters) S << "c\t" << std::format("{:x}", H) << "\t" << Join(M) << "\n";
        for (const auto& [H, A] : F.Maps) S << "m\t" << std::format("{:x}", H) << "\t" << Join(A) << "\n";
        return S.str();
    }

    Facts Parse(const std::string& Text)
    {
        Facts F;
        std::istringstream In(Text);
        std::string Line;
        const auto Hex = [](const std::string& S) { return std::strtoull(S.c_str(), nullptr, 16); };
        while (std::getline(In, Line))
        {
            if (!Line.empty() && Line.back() == '\r') Line.pop_back();
            const auto P = Split(Line, '\t');
            if (P.size() < 2) continue;
            const std::string& T = P[0];
            if (T == "key") F.Key = P[1];
            else if (T == "O") F.Owners.push_back({ P[1], P.size() > 2 ? P[2] : std::string() });
            else if (T == "c" && P.size() > 2) F.Characters[Hex(P[1])] = Split(P[2], '|');
            else if (T == "m" && P.size() > 2) F.Maps[Hex(P[1])] = Split(P[2], '|');
            else if (!F.Owners.empty())
            {
                auto& O = F.Owners.back();
                if (T == "M") O.Models.push_back(P[1]);
                else if (T == "C") O.Characters.push_back(Hex(P[1]));
                else if (T == "A") O.Maps.push_back(Hex(P[1]));
                else if (T == "X") O.Anims.push_back(P[1]);
            }
        }
        return F;
    }

    void Fill(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& Options, const Family& Shared,
              const std::vector<DiskGame::IDiskGame::AnimZone>& Zones, AnimOwners::Book& Out)
    {
        static std::mutex Lock;   // one run at a time reads and keeps the facts
        std::lock_guard<std::mutex> G(Lock);
        for (const auto& Z : Zones)
        {
            const std::string* D = DocumentOf(Z.Documents);
            if (D == nullptr) continue;
            const auto Mine = std::make_shared<const Facts>(Parse(*D));
            const std::string Install = !Options.Install.empty() ? Options.Install : Game.InstallOfZone(Z.Zone);
            Kept& K = KeptFor(DiskGame::KeptFile(Game, Options, Install, ".animowners"));
            // its own facts kept too, for another zone's run
            const std::string MyStem = Stem(Z.Zone);
            if (!K.Find(MyStem, Mine->Key)) K.Keep(MyStem, Mine);
            std::vector<std::shared_ptr<const Facts>> Held{ Mine };
            for (const auto& [Pattern, Patterns] : Shared)
                if (Fits(MyStem.c_str(), Pattern.c_str()))
                {
                    for (const auto& C : Candidates(Game, Options, Z.Zone, Patterns))
                        if (auto F = FactsOf(Game, Options, K, C, Mine->Key)) Held.push_back(std::move(F));
                    break;
                }
            std::vector<const Facts*> Sources;
            for (const auto& F : Held) Sources.push_back(F.get());
            Own(Sources, Z.Anims, Out);
        }
    }
}
