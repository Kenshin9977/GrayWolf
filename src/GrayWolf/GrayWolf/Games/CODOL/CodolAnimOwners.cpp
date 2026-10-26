#include "stdafx.h"

// Call of Duty Online's anim owners (CodolAnimOwners.h).

#include "Games/CODOL/CodolAnimOwners.h"

#include "AnimOwners.h"
#include "ExportNaming.h"
#include "Games/Treyarch/GscAnimOwners.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace CodolAnimOwners
{
    namespace
    {
        using ExportNaming::Lower;

        // the player's trees (the multiplayer body, the third person's)
        const char* const PlayerTrees[] = { "multiplayer", "player_3rd_person" };
        // the zones every map loads with it: the weapons and trees, the marketplace's tables
        const char* const SharedZoneNames[] = { "common_mp", "common_marketplace_lite", "common_marketplace_lite1" };

        struct Facts
        {
            std::map<std::string, std::vector<std::pair<int64_t, std::string>>> Weapons;   // weapon -> (field, anim)
            std::map<int64_t, std::set<std::pair<std::string, std::string>>> Overrides;    // field -> (from, to); from "": the anim itself
            std::map<std::string, std::pair<std::string, std::string>> Guns;               // weapon -> (view gun, its table)
            std::map<std::string, std::pair<std::string, std::string>> ViewArms;          // han, fja -> (view model, its table)
            std::vector<std::pair<std::string, std::vector<std::string>>> Characters;      // cha -> its part items
            std::map<std::string, std::string> Parts;                                      // part item -> its model
            std::map<std::string, std::set<std::string>> Trees;                            // tree -> its anims
        };

        // a document's name, '/' and '\' as '_', lower case ("mp_coweapmodels_ak47_mp.csv")
        std::string Flat(const std::string& Name)
        {
            std::string K = Lower(Name);
            std::replace(K.begin(), K.end(), '/', '_');
            std::replace(K.begin(), K.end(), '\\', '_');
            return K;
        }

        bool Starts(const std::string& S, const char* P) { return S.rfind(P, 0) == 0; }

        std::vector<std::string> Cells(const std::string& Row)
        {
            std::vector<std::string> Out;
            size_t From = 0;
            for (size_t At; (At = Row.find(',', From)) != std::string::npos; From = At + 1) Out.push_back(Row.substr(From, At - From));
            Out.push_back(Row.substr(From));
            return Out;
        }

        template <typename F> void Lines(const std::string& Text, F&& Each)
        {
            std::stringstream S(Text);
            std::string L;
            while (std::getline(S, L))
            {
                if (!L.empty() && L.back() == '\r') L.pop_back();
                if (!L.empty()) Each(L);
            }
        }

        // "0x<field> <text>" -> (field, text)
        bool FieldLine(const std::string& L, int64_t& Field, std::string& Text)
        {
            const size_t Sp = L.find(' ');
            if (Sp == std::string::npos) return false;
            Field = (int64_t)std::strtoll(L.substr(0, Sp).c_str(), nullptr, 16);
            Text = Lower(L.substr(Sp + 1));
            return !Text.empty();
        }

        // the documents read, by their flat name
        bool Wanted(const std::string& K)
        {
            return Starts(K, "animtrees_") || Starts(K, "weapon_") || Starts(K, "weaponattach_") || Starts(K, "mp_coweapmodels") ||
                   Starts(K, "mp_cocharitems");
        }

        Facts Parse(const std::vector<std::pair<std::string, std::string>>& Docs)
        {
            Facts F;
            std::vector<std::pair<std::string, std::string>> Atr;
            for (const auto& [Name, Text] : Docs)
            {
                const std::string K = Flat(Name);
                if (K.size() > 4 && K.compare(K.size() - 4, 4, ".atr") == 0)
                    Atr.emplace_back(Name, Text);
                else if (Starts(K, "weaponattach_"))
                    Lines(Text, [&](const std::string& L) {
                        int64_t Field;
                        std::string T;
                        if (!FieldLine(L, Field, T)) return;
                        const size_t Sp = T.find(' ');
                        if (Sp == std::string::npos) F.Overrides[Field].insert({ std::string(), T });
                        else if (T.find(' ', Sp + 1) == std::string::npos) F.Overrides[Field].insert({ T.substr(0, Sp), T.substr(Sp + 1) });
                    });
                else if (Starts(K, "weapon_"))
                {
                    auto& W = F.Weapons[K.substr(7)];
                    Lines(Text, [&](const std::string& L) {
                        int64_t Field;
                        std::string A;
                        if (FieldLine(L, Field, A)) W.emplace_back(Field, A);
                    });
                }
                else if (Starts(K, "mp_coweapmodels"))
                    // mp/coweapmodels/<w>.csv: weapon, skin, world model, view model...: skin 0's view gun,
                    // the weapon the table is named after (common_mp's altfire tables name the base weapon
                    // in their rows: ak47ssaltfire_mp's, ak47ss_mp)
                    Lines(Text, [&](const std::string& L) {
                        const auto C = Cells(L);
                        if (C.size() >= 4 && C[1] == "0" && !C[3].empty())
                            F.Guns.emplace(Lower(fs::path(Name).stem().string()), std::make_pair(Lower(C[3]), Name));
                    });
                else if (K == "mp_cocharitemscha.csv")
                    // cha_<n>, name, description, icon, then its parts (pants, boots, jacket, hands, head,
                    // vest, headgear...) up to its height's string
                    Lines(Text, [&](const std::string& L) {
                        const auto C = Cells(L);
                        if (C.empty() || !Starts(Lower(C[0]), "cha_")) return;
                        std::vector<std::string> Items;
                        for (size_t I = 4; I < C.size() && !Starts(Lower(C[I]), "sf_"); I++)
                            if (!C[I].empty()) Items.push_back(Lower(C[I]));
                        F.Characters.emplace_back(Lower(C[0]), std::move(Items));
                    });
                else if (Starts(K, "mp_cocharitems_"))
                    // item, "model", its variant, the model, the view model
                    Lines(Text, [&](const std::string& L) {
                        const auto C = Cells(L);
                        if (C.size() < 4 || C[1] != "model" || C[3].empty()) return;
                        F.Parts.emplace(Lower(C[0]), Lower(C[3]));
                        // the first hands' and jacket's view models: the view arms
                        if ((K == "mp_cocharitems_han.csv" || K == "mp_cocharitems_fja.csv") && C.size() >= 5 && !C[4].empty())
                            F.ViewArms.emplace(K.substr(15, 3), std::make_pair(Lower(C[4]), Name));
                    });
            }
            F.Trees = GscAnimOwners::Parse(Atr).Trees;
            return F;
        }

        // a shared zone's facts, read once per process (by its path: an install's zones are in its archives)
        std::shared_ptr<const Facts> SharedFacts(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& Options, const fs::path& Zone)
        {
            static std::mutex Lock;
            static std::map<std::string, std::shared_ptr<const Facts>> Cache;
            const std::string Key = Lower(Zone.string()) + "|" + Lower(Options.Install);
            {
                std::lock_guard<std::mutex> G(Lock);
                if (const auto It = Cache.find(Key); It != Cache.end()) return It->second;
            }
            std::string Error;
            std::vector<std::pair<std::string, std::string>> Docs;
            if (auto Z = Game.Open(Zone.string(), Options, Error))
            {
                std::vector<std::pair<std::string, std::string>> All;
                Z->Documents(All);
                for (auto& D : All)
                    if (Wanted(Flat(D.first))) Docs.push_back(std::move(D));
            }
            auto F = std::make_shared<const Facts>(Parse(Docs));
            std::lock_guard<std::mutex> G(Lock);
            Cache[Key] = F;
            return F;
        }

        // the zone's facts first, then the shared zones'
        struct View
        {
            std::vector<const Facts*> All;

            const std::pair<std::string, std::string>* Gun(const std::string& W) const
            {
                for (const Facts* F : All)
                    if (const auto It = F->Guns.find(W); It != F->Guns.end()) return &It->second;
                return nullptr;
            }
            // a weapon's view gun; a weapon no table names takes its parent's: an attachment's weapon
            // (<w>_shotgun_attach_mp, gl_<w>_mp, gl_pulseradar_<w>_mp, uni_<w>_mp: the longest run of
            // its words, the first), else a skin's (altfirecsg12ssblkred_mp: the longest weapon its
            // name starts with, else ends with)
            const std::pair<std::string, std::string>* GunOf(const std::string& W) const
            {
                if (const auto* G = Gun(W)) return G;
                const std::string Stem = W.size() > 3 && W.compare(W.size() - 3, 3, "_mp") == 0 ? W.substr(0, W.size() - 3) : W;
                std::vector<std::string> Words;
                for (size_t From = 0, At;; From = At + 1)
                {
                    At = Stem.find('_', From);
                    Words.push_back(Stem.substr(From, At == std::string::npos ? std::string::npos : At - From));
                    if (At == std::string::npos) break;
                }
                for (size_t N = Words.size() - 1; N >= 1; N--)
                    for (size_t First = 0; First + N <= Words.size(); First++)
                    {
                        std::string P;
                        for (size_t K = First; K < First + N; K++) P += (K > First ? "_" : "") + Words[K];
                        if (const auto* G = Gun(P + "_mp")) return G;
                    }
                for (size_t N = Stem.size() - 1; N >= 4; N--)
                    if (const auto* G = Gun(Stem.substr(0, N) + "_mp")) return G;
                // (altfirehoneybadgerss_mp: honeybadgerss_mp's)
                for (size_t N = Stem.size() - 1; N >= 4; N--)
                    if (const auto* G = Gun(Stem.substr(Stem.size() - N) + "_mp")) return G;
                return nullptr;
            }
            // the view arms: the jacket's sleeves, then the hands (their view models, their tables)
            std::vector<std::pair<std::string, std::string>> Arms() const
            {
                std::vector<std::pair<std::string, std::string>> Out;
                for (const char* Part : { "fja", "han" })
                    for (const Facts* F : All)
                        if (const auto It = F->ViewArms.find(Part); It != F->ViewArms.end())
                        {
                            Out.push_back(It->second);
                            break;
                        }
                return Out;
            }
            std::string Part(const std::string& Item) const
            {
                for (const Facts* F : All)
                    if (const auto It = F->Parts.find(Item); It != F->Parts.end()) return It->second;
                return {};
            }
        };

        // how well a weapon's name fits an anim's: viewmodel_<weapon stem>_... best
        size_t Fit(const std::string& Anim, const std::string& Weapon)
        {
            const std::string A = Starts(Anim, "viewmodel_") ? Anim.substr(10) : Anim;
            const std::string W = Weapon.size() > 3 && Weapon.compare(Weapon.size() - 3, 3, "_mp") == 0 ? Weapon.substr(0, Weapon.size() - 3) : Weapon;
            if (Starts(A, (W + "_").c_str())) return 1000 + W.size();
            size_t N = 0;
            while (N < A.size() && N < W.size() && A[N] == W[N]) N++;
            return N;
        }

        // anim -> (0 a field of the weapon, 1 an attachment's; the weapon)
        using Index = std::map<std::string, std::set<std::pair<int, std::string>>>;

        Index WeaponIndex(const View& V)
        {
            // weapon -> its anim fields, the zone's first; the overrides of every source
            std::map<std::string, const std::vector<std::pair<int64_t, std::string>>*> Weapons;
            std::map<int64_t, std::set<std::pair<std::string, std::string>>> Overrides;
            for (const Facts* F : V.All)
            {
                for (const auto& [W, Fields] : F->Weapons) Weapons.emplace(W, &Fields);
                for (const auto& [Field, Set] : F->Overrides) Overrides[Field].insert(Set.begin(), Set.end());
            }
            // an anim field's anim with one or two of its overrides (an attachment each)
            std::map<std::pair<int64_t, std::string>, std::vector<std::string>> Made;
            const auto Derived = [&](int64_t Field, const std::string& Anim) -> const std::vector<std::string>& {
                auto [It, New] = Made.try_emplace({ Field, Anim });
                const auto O = Overrides.find(Field);
                if (!New || O == Overrides.end()) return It->second;
                std::set<std::string> Seen{ Anim }, Level{ Anim };
                for (int Depth = 0; Depth < 2 && !Level.empty(); Depth++)
                {
                    std::set<std::string> Next;
                    for (const auto& A : Level)
                        for (const auto& [From, To] : O->second)
                        {
                            std::string D;
                            if (From.empty()) D = To;
                            else if (const size_t At = A.find(From); At != std::string::npos) D = A.substr(0, At) + To + A.substr(At + From.size());
                            if (!D.empty() && Seen.insert(D).second) Next.insert(D);
                        }
                    Level = std::move(Next);
                }
                Seen.erase(Anim);
                It->second.assign(Seen.begin(), Seen.end());
                return It->second;
            };
            Index Of;
            for (const auto& [W, Fields] : Weapons)
                for (const auto& [Field, Anim] : *Fields)
                {
                    Of[Anim].insert({ 0, W });
                    for (const auto& D : Derived(Field, Anim)) Of[D].insert({ 1, W });
                }
            return Of;
        }

        // the index of the shared zones' weapons alone, made once (a zone with no weapon of its own)
        std::shared_ptr<const Index> SharedIndex(const std::string& Key, const View& V)
        {
            static std::mutex Lock;
            static std::map<std::string, std::shared_ptr<const Index>> Cache;
            std::lock_guard<std::mutex> G(Lock);
            auto& I = Cache[Key];
            if (!I) I = std::make_shared<const Index>(WeaponIndex(V));
            return I;
        }

        void OwnWeapons(const View& V, const Index& Of, const std::map<std::string, std::string>& Exported, AnimOwners::Book& Out)
        {
            const auto Arms = V.Arms();
            struct Candidate
            {
                int Rank;
                size_t Fit;
                std::string W;
                const std::pair<std::string, std::string>* Gun;
            };
            for (const auto& [Anim, As] : Exported)
            {
                const auto It = Of.find(Anim);
                if (It == Of.end()) continue;
                const auto& Set = It->second;
                std::vector<Candidate> C;
                for (const auto& [Rank, W] : Set)
                    if (const auto* G = V.GunOf(W)) C.push_back({ Rank, Fit(Anim, W), W, G });
                std::sort(C.begin(), C.end(), [](const Candidate& A, const Candidate& B) {
                    if (A.Rank != B.Rank) return A.Rank < B.Rank;
                    if (A.Fit != B.Fit) return A.Fit > B.Fit;
                    return A.W < B.W;
                });
                std::set<std::string> Guns;   // one weapon per view gun
                for (const auto& K : C)
                {
                    if (!Guns.insert(K.Gun->first).second) continue;
                    const std::string Id = "weapon:" + K.W;
                    auto& O = Out.Owners[Id];
                    // the arms first (the sleeves hold the skeleton's chain, tag_torso to the fingers), the gun last
                    O.Models.clear();
                    O.Source = "weapon " + K.W;
                    for (const auto& [Model, Table] : Arms) O.Models.push_back(Model), O.Source += " -> " + Table;
                    O.Models.push_back(K.Gun->first);
                    O.Source += " -> " + K.Gun->second;
                    Out.Own(As, Id);
                }
            }
        }

        void OwnCharacters(const View& V, const std::map<std::string, std::string>& Exported, AnimOwners::Book& Out)
        {
            const std::vector<std::pair<std::string, std::vector<std::string>>>* Table = nullptr;
            for (const Facts* F : V.All)
                if (!F->Characters.empty())
                {
                    Table = &F->Characters;
                    break;
                }
            if (Table == nullptr) return;
            // each character's models, its parts in the table's order (one id per set of models)
            std::vector<std::string> Ids;
            std::set<std::vector<std::string>> Done;
            for (const auto& [Cha, Items] : *Table)
            {
                std::vector<std::string> Models;
                for (const auto& I : Items)
                    if (const std::string M = V.Part(I); !M.empty() && std::find(Models.begin(), Models.end(), M) == Models.end()) Models.push_back(M);
                if (Models.empty() || !Done.insert(Models).second) continue;
                const std::string Id = "character:" + Cha;
                Out.Owners[Id] = { Models, "mp/cocharitemscha.csv " + Cha + " -> mp/cocharitems_<part>.csv" };
                Ids.push_back(Id);
            }
            std::set<std::string> Anims;
            for (const Facts* F : V.All)
                for (const char* T : PlayerTrees)
                    if (const auto It = F->Trees.find(T); It != F->Trees.end()) Anims.insert(It->second.begin(), It->second.end());
            for (const auto& A : Anims)
                if (const auto E = Exported.find(A); E != Exported.end())
                    for (const auto& Id : Ids) Out.Own(E->second, Id);
        }
    }

    std::vector<std::string> Documents()
    {
        // (animtrees/<t>.atr, weapon/<w>, weaponattach/<a>, the mp/ tables)
        return { "animtrees*.atr", "weapon*", "mp*coweapmodels*", "mp*cocharitems*" };
    }

    bool AnimField(int64_t Field)
    {
        return Field >= 0x80 && Field <= 0x360 && Field % 4 == 0 && Field != 0x174 && Field != 0x26c;
    }

    void Fill(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& Options,
              const std::vector<DiskGame::IDiskGame::AnimZone>& Zones, AnimOwners::Book& Out)
    {
        for (const auto& Z : Zones)
        {
            const Facts Own = Parse(Z.Documents);
            const fs::path Path(Z.Zone);
            const std::string Stem = Lower(Path.stem().string());
            std::vector<std::shared_ptr<const Facts>> Shared;
            for (const char* N : SharedZoneNames)
                if (Stem != N) Shared.push_back(SharedFacts(Game, Options, Path.parent_path() / (std::string(N) + ".ff")));
            View V, OfShared;
            V.All.push_back(&Own);
            std::string Key = Lower(Options.Install);
            for (const auto& F : Shared) V.All.push_back(F.get()), OfShared.All.push_back(F.get()), Key += "|" + std::to_string((uintptr_t)F.get());
            std::map<std::string, std::string> Exported;   // lower case -> as exported
            for (const auto& A : Z.Anims) Exported.emplace(Lower(A), A);
            if (Own.Weapons.empty() && Own.Overrides.empty()) OwnWeapons(V, *SharedIndex(Key, OfShared), Exported, Out);
            else OwnWeapons(V, WeaponIndex(V), Exported, Out);
            OwnCharacters(V, Exported, Out);
        }
    }
}
