#include "stdafx.h"

// The anim owners of the Infinity Ward zones Ghosts to Infinite Warfare (IwZoneAnimOwners.h).

#include "Games/IW/IwZoneAnimOwners.h"

#include "AnimOwners.h"
#include "ExportNaming.h"
#include "Games/Treyarch/GscAnimOwners.h"

#include <miniz/miniz.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace IwZoneAnimOwners
{
    namespace
    {
        using ExportNaming::At;
        using ExportNaming::Lower;

        const std::string Prefix = "animowners/";
        // the assets whose anims (and references) a document lists
        const char* const OwnerTypes[] = { "weapon", "attachment", "anim_package", "playeranim", "animclass", "gesture", "suit", "suitanimpackage" };
        // the view-hands-only sets (Infinite Warfare's gestures, its suits' default anims)
        const char* const HandsTypes[] = { "gesture", "suit", "suitanimpackage" };

        bool Starts(const std::string& S, const std::string& P) { return S.rfind(P, 0) == 0; }
        bool Ends(const std::string& S, const std::string& E) { return S.size() >= E.size() && S.compare(S.size() - E.size(), E.size(), E) == 0; }

        // a name as the kinds write it: lower case, a reference's ',' dropped
        std::string Clean(std::string N)
        {
            if (!N.empty() && N[0] == ',') N.erase(0, 1);
            if (N.size() > 255 || std::any_of(N.begin(), N.end(), [](char C) { return (unsigned char)C < 0x20 || (unsigned char)C >= 0x7f; })) return {};
            return Lower(N);
        }

        // ---- documents (the zone open)

        struct Zone
        {
            const Layout& L;
            const ZoneView& V;
            const ZoneTables::ZoneResult& R;
            std::unordered_map<int64_t, size_t> AtPos;         // instance by position, the outermost
            std::vector<std::vector<size_t>> Children;         // each instance's direct children

            Zone(const Layout& L_, const ZoneView& V_) : L(L_), V(V_), R(*V_.Walk)
            {
                Children.resize(R.Instances.size());
                for (size_t I = 0; I < R.Instances.size(); I++)
                {
                    AtPos.try_emplace(R.Instances[I].Pos, I);
                    const int32_t P = R.Instances[I].Parent;
                    if (P >= 0 && (size_t)P < R.Instances.size()) Children[(size_t)P].push_back(I);
                }
            }
            const std::string& Type(size_t I) const
            {
                static const std::string None;
                const uint16_t O = R.Instances[I].Owner;
                return O < R.OwnerNames.size() ? R.OwnerNames[O] : None;
            }
            uint64_t Pointer(int64_t Pos) const
            {
                const auto& Img = *V.Image;
                return Pos >= 0 && Pos + 8 <= (int64_t)Img.size() ? At<uint64_t>(Img, Pos) : 0;
            }
            // the instance a pointer field names, -1 none
            int64_t Target(int64_t Field) const
            {
                const uint64_t P = Pointer(Field);
                const int64_t To = P != 0 ? V.Off(P) : -1;
                if (To < 0) return -1;
                const auto It = AtPos.find(To);
                return It == AtPos.end() ? -1 : (int64_t)It->second;
            }
            std::string Name(size_t I) const
            {
                const std::string& T = Type(I);
                if (T == L.AnimType || T == L.ModelType) return Clean(V.Named(T, R.Instances[I].Pos));
                return Clean(V.Text(R.Instances[I].Pos));
            }
            // the instances Owner's own reads point to (its children's reads left out), in read order
            std::vector<size_t> Refers(size_t Owner) const
            {
                const auto& I = R.Instances[Owner];
                std::vector<std::pair<uint32_t, uint32_t>> Skip;
                for (const size_t C : Children[Owner]) Skip.emplace_back(R.Instances[C].FirstRead, R.Instances[C].EndRead);
                std::sort(Skip.begin(), Skip.end());
                std::vector<size_t> Out;
                std::set<size_t> Seen;
                size_t S = 0;
                for (uint32_t Rd = I.FirstRead; Rd < I.EndRead && Rd < R.Reads.size() && Rd < R.ReadPos.size(); Rd++)
                {
                    while (S < Skip.size() && Skip[S].second <= Rd) S++;
                    if (S < Skip.size() && Skip[S].first <= Rd)
                    {
                        Rd = Skip[S].second - 1;
                        continue;
                    }
                    if (R.ReadPos[Rd] < 0 || R.Reads[Rd] <= 0) continue;
                    for (int64_t B = 0; B + 8 <= R.Reads[Rd]; B += 8)
                    {
                        const int64_t T = Target(R.ReadPos[Rd] + B);
                        if (T >= 0 && (size_t)T != Owner && Seen.insert((size_t)T).second) Out.push_back((size_t)T);
                    }
                }
                return Out;
            }
            // the model a weapon's chain of fields names, a composite's parts joined by '|'
            std::string Model(size_t Weapon, const std::vector<int64_t>& Chain) const
            {
                if (Chain.empty()) return {};
                int64_t P = R.Instances[Weapon].Pos;
                for (size_t K = 0; K + 1 < Chain.size() && P >= 0; K++)
                {
                    const uint64_t A = Pointer(P + Chain[K]);
                    P = A != 0 ? V.Off(A) : -1;
                }
                if (P < 0) return {};
                const int64_t T = Target(P + Chain.back());
                if (T < 0 || Type((size_t)T) != L.ModelType) return {};
                const int64_t M = R.Instances[(size_t)T].Pos;
                const auto& Img = *V.Image;
                if (L.CompositeFlagAt < 0 || M + L.CompositeCount + 1 > (int64_t)Img.size() || (At<uint32_t>(Img, M + L.CompositeFlagAt) & L.CompositeFlag) == 0)
                    return Name((size_t)T);
                const uint64_t A = Pointer(M + L.CompositeList);
                const int64_t List = A != 0 ? V.Off(A) : -1;
                std::string Parts;
                for (uint8_t N = 0; List >= 0 && N < At<uint8_t>(Img, M + L.CompositeCount); N++)
                    if (const int64_t Part = Target(List + L.CompositeEntry * N); Part >= 0)
                        if (const std::string Nm = Name((size_t)Part); !Nm.empty()) Parts += (Parts.empty() ? "" : "|") + Nm;
                return Parts;
            }
        };

        // a compiled script's strings: its stack (zlib) inflated, its runs of printable bytes
        std::vector<std::string> ScriptStrings(const ZoneView& V, int64_t Pos)
        {
            const auto& Img = *V.Image;
            if (Pos + 0x20 > (int64_t)Img.size()) return {};
            const uint32_t Packed = At<uint32_t>(Img, Pos + 8), Len = At<uint32_t>(Img, Pos + 0xc);
            if (Packed == 0 || Len == 0 || Len > (1u << 20) || Packed > (1u << 20)) return {};
            std::vector<uint8_t> In, Out(Len);
            if (!V.Bytes(At<uint64_t>(Img, Pos + 0x18), Packed, In)) return {};
            mz_ulong Got = Len;
            if (mz_uncompress(Out.data(), &Got, In.data(), (mz_ulong)In.size()) != MZ_OK) return {};
            std::vector<std::string> S;
            for (size_t I = 0; I < Got;)
            {
                size_t E = I;
                while (E < Got && Out[E] >= 0x20 && Out[E] < 0x7f) E++;
                if (E - I >= 4) S.push_back(Lower(std::string(Out.begin() + I, Out.begin() + E)));
                I = E + 1;
            }
            return S;
        }

        // an animclass's script strings: the u32 slots in its reads that name a string of the zone
        std::vector<std::string> SlotStrings(const Zone& Z, size_t Owner)
        {
            std::vector<std::string> Out;
            if (Z.V.Slots == nullptr || Z.V.Slots->empty()) return Out;
            const auto& Slots = *Z.V.Slots;
            const auto& I = Z.R.Instances[Owner];
            const auto& Img = *Z.V.Image;
            std::set<std::string> Seen;
            for (uint32_t Rd = I.FirstRead; Rd < I.EndRead && Rd < Z.R.Reads.size() && Rd < Z.R.ReadPos.size(); Rd++)
            {
                const int64_t P = Z.R.ReadPos[Rd];
                if (P < 0 || Z.R.Reads[Rd] <= 0) continue;
                for (int64_t B = 0; B + 4 <= Z.R.Reads[Rd] && P + B + 4 <= (int64_t)Img.size(); B += 4)
                {
                    const uint32_t S = At<uint32_t>(Img, P + B);
                    if (S == 0 || S >= Slots.size()) continue;
                    const std::string T = Clean(Slots[S]);
                    if (!T.empty() && Seen.insert(T).second) Out.push_back(T);
                }
            }
            return Out;
        }

        // ---- facts (from the documents)

        struct Weapon
        {
            std::string Gun, Hands;
            std::vector<std::string> Anims, Attachments, Packages;
        };
        struct Facts
        {
            std::map<std::string, Weapon> Weapons;
            // attachment, anim_package, playeranim, animclass -> name -> its anims (or strings)
            std::map<std::string, std::map<std::string, std::vector<std::string>>> Sets;
            std::map<std::string, std::set<std::string>> Trees;                    // an .atr's anims
            std::vector<std::string> ViewHands;                                    // the zone's player view hands
            std::vector<std::string> Bodies, Heads;                                // the level's soldier models
            std::map<std::string, std::vector<std::string>> ClassModels, ClassNear;   // an animclass's models beside it
            std::vector<std::pair<std::string, std::vector<std::string>>> Characters;   // script -> body, head
        };

        template <typename F> void Lines(const std::string& Text, F&& Each)
        {
            std::istringstream S(Text);
            for (std::string Line; std::getline(S, Line);)
            {
                if (!Line.empty() && Line.back() == '\r') Line.pop_back();
                const size_t Sp = Line.find(' ');
                if (Sp != std::string::npos) Each(Line.substr(0, Sp), Line.substr(Sp + 1));
            }
        }

        Facts Parse(const std::vector<std::pair<std::string, std::string>>& Docs)
        {
            Facts F;
            std::vector<std::pair<std::string, std::string>> Atr;
            for (const auto& [Name, Text] : Docs)
            {
                const std::string N = Lower(Name);
                if (Ends(N, ".atr"))
                {
                    Atr.emplace_back(Name, Text);
                    continue;
                }
                if (!Starts(N, Prefix)) continue;
                const std::string Rest = N.substr(Prefix.size());
                const size_t Slash = Rest.find('/');
                if (Slash == std::string::npos) continue;
                const std::string Type = Rest.substr(0, Slash), Own = Rest.substr(Slash + 1);
                if (Type == "weapon")
                {
                    if (F.Weapons.count(Own)) continue;
                    auto& W = F.Weapons[Own];
                    Lines(Text, [&](const std::string& K, const std::string& V) {
                        if (K == "gun") W.Gun = V;
                        else if (K == "hands") W.Hands = V;
                        else if (K == "anim") W.Anims.push_back(V);
                        else if (K == "attachment") W.Attachments.push_back(V);
                        else if (K == "anim_package") W.Packages.push_back(V);
                    });
                }
                else if (Type == "viewhands") F.ViewHands.push_back(Own);
                else if (Type == "soldier") (Starts(Own, "body_") ? F.Bodies : F.Heads).push_back(Own);
                else if (Type == "character")
                {
                    std::vector<std::string> Models;
                    Lines(Text, [&](const std::string& K, const std::string& V) {
                        if (K == "model") Models.push_back(V);
                    });
                    if (!Models.empty()) F.Characters.emplace_back(Own, std::move(Models));
                }
                else
                {
                    auto [It, New] = F.Sets[Type].try_emplace(Own);
                    if (New)
                        Lines(Text, [&](const std::string& K, const std::string& V) {
                            if (K == "anim" || K == "string") It->second.push_back(V);
                            else if (K == "suitanimpackage") It->second.push_back("suitanimpackage " + V);
                            else if (K == "model" && Type == "animclass") F.ClassModels[Own].push_back(V);
                            else if (K == "near" && Type == "animclass") F.ClassNear[Own].push_back(V);
                        });
                }
            }
            for (auto& [T, A] : GscAnimOwners::Parse(Atr).Trees) F.Trees[T] = std::move(A);
            return F;
        }

        // a zone's facts, read once per file (its size and time unchanged)
        struct Kept
        {
            uintmax_t Size = 0;
            fs::file_time_type When;
            std::shared_ptr<const Facts> F;
        };
        std::mutex CacheLock;
        std::map<std::wstring, Kept> Cache;

        std::shared_ptr<const Facts> FactsOf(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& Options, const fs::path& Path,
                                             const std::vector<std::pair<std::string, std::string>>* Known)
        {
            std::error_code Ec, Ec2;
            const uintmax_t Size = fs::file_size(Path, Ec);
            const auto When = fs::last_write_time(Path, Ec2);
            if (Ec || Ec2) return Known ? std::make_shared<const Facts>(Parse(*Known)) : nullptr;
            const std::wstring Key = Path.wstring();
            {
                std::lock_guard<std::mutex> G(CacheLock);
                if (const auto It = Cache.find(Key); It != Cache.end() && It->second.Size == Size && It->second.When == When) return It->second.F;
            }
            std::vector<std::pair<std::string, std::string>> Docs;
            if (Known) Docs = *Known;
            else
            {
                std::string Error;
                if (auto Z = Game.Open(Path.string(), Options, Error))
                {
                    std::vector<std::pair<std::string, std::string>> All;
                    Z->Documents(All);
                    for (auto& D : All)
                        if (Starts(Lower(D.first), Prefix) || Ends(Lower(D.first), ".atr")) Docs.push_back(std::move(D));
                }
            }
            auto F = std::make_shared<const Facts>(Parse(Docs));
            std::lock_guard<std::mutex> G(CacheLock);
            Cache[Key] = { Size, When, F };
            return F;
        }

        std::vector<fs::path> SharedZones(const Layout& L, const fs::path& Zone)
        {
            const std::string Stem = Lower(Zone.stem().string());
            const bool Mp = Starts(Stem, "mp_") || Ends(Stem, "_mp") || Stem.find("_mp_") != std::string::npos;
            std::vector<std::string> Names{ "patch_" + Stem };
            for (const auto& [Word, More] : L.SharedWhen)
                if (Stem.find(Word) != std::string::npos) Names.insert(Names.end(), More.begin(), More.end());
            for (const auto& N : Mp ? L.SharedMp : L.SharedSp) Names.push_back(N);
            std::vector<fs::path> Out;
            std::error_code Ec;
            for (const auto& N : Names)
                if (N != Stem)
                    if (const fs::path P = Zone.parent_path() / (N + ".ff"); fs::exists(P, Ec)) Out.push_back(P);
            return Out;
        }

        // how much of the anim's name the model's (or weapon's) starts with, their prefixes
        // (viewmodel_, vm_, wpn_h1_...) dropped: vm_ak12_reload ~ vm_ak12_loot_base
        size_t Likeness(const std::string& Anim, const std::string& Model)
        {
            const auto Bare = [](const std::string& S) {
                for (const char* P : { "viewmodel_", "vm_", "wpn_h1_", "wpn_", "weapon_", "va_", "iw5_", "iw6_", "iw7_", "h1_" })
                    if (Starts(S, P)) return S.substr(std::strlen(P));
                return S;
            };
            std::string A = Bare(Anim), M = Bare(Model);
            if (Starts(A, "asl_") && Starts(M, "asl_")) A = A.substr(4), M = M.substr(4);
            size_t N = 0;
            while (N < A.size() && N < M.size() && A[N] == M[N]) N++;
            return N;
        }

        std::string ModelsKey(const ::AnimOwners::Owner& O)
        {
            std::string K;
            for (const auto& M : O.Models) K += Lower(M) + "|";
            return K;
        }

        // a model, or a composite's parts joined by '|', as models
        void Split(const std::string& Joined, std::vector<std::string>& Out)
        {
            for (size_t From = 0; From < Joined.size();)
            {
                const size_t Bar = std::min(Joined.find('|', From), Joined.size());
                if (Bar > From) Out.push_back(Joined.substr(From, Bar - From));
                From = Bar + 1;
            }
        }

        void Add(std::map<std::string, std::vector<std::string>>& Ids, const std::string& Anim, const std::string& Id)
        {
            auto& V = Ids[Anim];
            if (std::find(V.begin(), V.end(), Id) == V.end()) V.push_back(Id);
        }
    }

    void Documents(const Layout& L, const ZoneView& V, std::vector<std::pair<std::string, std::string>>& Out)
    {
        if (V.Walk == nullptr || V.Image == nullptr) return;
        const Zone Z(L, V);
        const auto& R = Z.R;
        std::set<std::string> Done;
        // the zone's own models, for an animclass's (the models beside it named after it)
        std::vector<std::string> Models;
        for (size_t I = 0; I < R.Instances.size(); I++)
            if (R.Instances[I].Parent < 0 && Z.Type(I) == L.ModelType)
                if (std::string N = Z.Name(I); !N.empty()) Models.push_back(std::move(N));
        for (size_t I = 0; I < R.Instances.size(); I++)
        {
            const std::string& T = Z.Type(I);
            if (T == "rawfile" && R.Instances[I].Parent < 0)
            {
                std::string N;
                std::vector<uint8_t> Bytes;
                if (V.Rawfile && V.Rawfile(R.Instances[I].Pos, N, Bytes) && Ends(N = Clean(N), ".atr")) Out.emplace_back(N, std::string(Bytes.begin(), Bytes.end()));
                continue;
            }
            if (T == "scriptfile" && R.Instances[I].Parent < 0 && !L.CharacterBodies.empty() && V.Bytes)
            {
                // a player character: its body (a model of the layout's prefixes) and its first head
                const auto S = ScriptStrings(V, R.Instances[I].Pos);
                if (S.size() > 64) continue;
                std::string Body, Head;
                for (const auto& Str : S)
                {
                    if (Body.empty())
                        for (const auto& P : L.CharacterBodies)
                            if (Starts(Str, P)) Body = Str;
                    if (Head.empty() && (Starts(Str, "head_") || Str.find("_head_") != std::string::npos) && Str.find('/') == std::string::npos) Head = Str;
                }
                // (a test character's body left out: mp_body_cloak_test)
                if (Body.empty() || Body.find("test") != std::string::npos) continue;
                const std::string N = Clean(V.Text(R.Instances[I].Pos));
                Out.emplace_back(Prefix + "character/" + (N.empty() ? Body : N), "model " + Body + "\n" + (Head.empty() ? "" : "model " + Head + "\n"));
                continue;
            }
            if (T == L.ModelType && R.Instances[I].Parent < 0)
            {
                // the player's view hands the level sets (a single player weapon names none), the
                // level's soldier models
                const std::string N = Z.Name(I);
                if (Starts(N, "viewhands_player") && Done.insert("viewhands/" + N).second) Out.emplace_back(Prefix + "viewhands/" + N, "model " + N + "\n");
                else if ((Starts(N, "body_") || Starts(N, "head_")) && !L.LevelTrees.empty() && Done.insert("soldier/" + N).second)
                    Out.emplace_back(Prefix + "soldier/" + N, "model " + N + "\n");
                continue;
            }
            if (std::find(std::begin(OwnerTypes), std::end(OwnerTypes), T) == std::end(OwnerTypes)) continue;
            const std::string N = Z.Name(I);
            if (N.empty() || !Done.insert(T + "/" + N).second) continue;
            std::string Text;
            if (T == "weapon")
            {
                if (const std::string G = Z.Model(I, L.Gun); !G.empty()) Text += "gun " + G + "\n";
                if (const std::string H = Z.Model(I, L.Hands); !H.empty()) Text += "hands " + H + "\n";
            }
            for (const size_t To : Z.Refers(I))
            {
                const std::string& U = Z.Type(To);
                const bool Anim = U == L.AnimType;
                if (!Anim && U != "attachment" && U != "anim_package" && U != "suitanimpackage") continue;
                if (const std::string M = Z.Name(To); !M.empty()) Text += (Anim ? std::string("anim") : U) + " " + M + "\n";
            }
            if (T == "animclass")
            {
                for (const auto& S : SlotStrings(Z, I)) Text += "string " + S + "\n";
                // the zone's models named after it: its name first (zombie_ghost_animclass:
                // zombie_ghost_green), else a body holding its name (dog_animclass: mp_fullbody_dog_a)
                std::string Stem = N;
                if (const size_t P = Stem.find("_animclass"); P != std::string::npos) Stem = Stem.substr(0, P);
                std::vector<std::string> Named, Near;
                for (const auto& M : Models)
                    if (M == Stem || Starts(M, Stem + "_")) Named.push_back(M);
                    else if (("_" + M + "_").find("_" + Stem + "_") != std::string::npos && (M.find("body") != std::string::npos || M.find("torso") != std::string::npos))
                        Near.push_back(M);
                const auto Shortest = [](const std::string& A, const std::string& B) { return A.size() < B.size(); };
                std::stable_sort(Named.begin(), Named.end(), Shortest);
                std::stable_sort(Near.begin(), Near.end(), Shortest);
                for (size_t K = 0; K < Named.size() && K < 4; K++) Text += "model " + Named[K] + "\n";
                for (size_t K = 0; K < Near.size() && K < 4; K++) Text += "near " + Near[K] + "\n";
            }
            if (!Text.empty()) Out.emplace_back(Prefix + T + "/" + N, std::move(Text));
        }
    }

    std::vector<std::string> Patterns() { return { Prefix + "*", "*.atr" }; }

    void Fill(const DiskGame::IDiskGame& Game, const Layout& L, const DiskGame::ExportOptions& Options,
              const std::vector<DiskGame::IDiskGame::AnimZone>& Zones, ::AnimOwners::Book& Out)
    {
        std::vector<std::shared_ptr<const Facts>> Own;
        for (const auto& Z : Zones) Own.push_back(FactsOf(Game, Options, fs::path(Z.Zone), &Z.Documents));
        for (size_t Zi = 0; Zi < Zones.size(); Zi++)
        {
            std::vector<std::shared_ptr<const Facts>> Held{ Own[Zi] };
            for (const auto& P : SharedZones(L, fs::path(Zones[Zi].Zone))) Held.push_back(FactsOf(Game, Options, P, nullptr));
            std::vector<const Facts*> Sources;
            for (const auto& F : Held)
                if (F) Sources.push_back(F.get());

            // the sources' weapons and sets, the first naming one winning
            std::map<std::string, const Weapon*> Weapons;
            std::map<std::string, std::map<std::string, const std::vector<std::string>*>> Sets;
            for (const Facts* F : Sources)
            {
                for (const auto& [N, W] : F->Weapons) Weapons.emplace(N, &W);
                for (const auto& [T, Of] : F->Sets)
                    for (const auto& [N, A] : Of) Sets[T].emplace(N, &A);
            }
            const auto Set = [&](const std::string& T, const std::string& N) -> const std::vector<std::string>* {
                const auto It = Sets.find(T);
                if (It == Sets.end()) return nullptr;
                const auto S = It->second.find(N);
                return S == It->second.end() ? nullptr : S->second;
            };

            // the view hands of a weapon naming none: the player's of the first source holding any
            std::string Hands;
            for (const Facts* F : Sources)
                if (!F->ViewHands.empty())
                {
                    Hands = F->ViewHands.front();
                    break;
                }
            // the view hands of the gestures and suits: the zone's player's, else those most weapons name
            std::string ViewHands = Hands;
            if (ViewHands.empty())
            {
                std::map<std::string, size_t> Count;
                for (const auto& [N, W] : Weapons)
                    if (!W->Hands.empty() && ++Count[W->Hands] > Count[ViewHands]) ViewHands = W->Hands;
            }
            std::map<std::string, std::string> Exported;   // lower case -> as exported
            for (const auto& A : Zones[Zi].Anims) Exported.emplace(Lower(A), A);
            std::map<std::string, std::vector<std::string>> Ids;   // lower anim -> its owner ids, in order

            // 1. weapons: their own anims and their packages' (rank 0), their attachments' (rank 1)
            struct Hit
            {
                int Rank;
                size_t Like;
                std::string W;
            };
            std::map<std::string, std::vector<Hit>> Hits;
            for (const auto& [N, W] : Weapons)
            {
                if (W->Gun.empty() && W->Hands.empty()) continue;
                const std::string Face = W->Gun.empty() ? N : W->Gun.substr(0, W->Gun.find('|'));
                const auto Hit_ = [&](const std::string& A, int Rank) {
                    if (Exported.count(A)) Hits[A].push_back({ Rank, Likeness(A, Face), N });
                };
                for (const auto& A : W->Anims) Hit_(A, 0);
                for (const auto& P : W->Packages)
                    if (const auto* S = Set("anim_package", P))
                        for (const auto& A : *S) Hit_(A, 0);
                for (const auto& At_ : W->Attachments)
                    if (const auto* S = Set("attachment", At_))
                        for (const auto& A : *S) Hit_(A, 1);
            }
            for (auto& [A, H] : Hits)
            {
                std::sort(H.begin(), H.end(), [](const Hit& X, const Hit& Y) {
                    if (X.Rank != Y.Rank) return X.Rank < Y.Rank;
                    if (X.Like != Y.Like) return X.Like > Y.Like;
                    if (X.W.size() != Y.W.size()) return X.W.size() < Y.W.size();
                    return X.W < Y.W;
                });
                for (const auto& K : H)
                {
                    const std::string Id = "weapon:" + K.W;
                    if (!Out.Owners.count(Id))
                    {
                        const Weapon& W = *Weapons.at(K.W);
                        const std::string& H = W.Hands.empty() ? Hands : W.Hands;
                        ::AnimOwners::Owner O;
                        // the hands first: they hold the arm chain the view model joins
                        for (const auto& M : { H, W.Gun }) Split(M, O.Models);
                        O.Source = "weapon " + K.W + (H.empty() ? "" : W.Hands.empty() ? " -> the zone's player view hands " + H : " -> hands " + H) +
                                   (W.Gun.empty() ? "" : " -> view model " + W.Gun);
                        Out.Owners[Id] = std::move(O);
                    }
                    Add(Ids, A, Id);
                }
            }

            // 2. gestures and suits (Infinite Warfare): on the view hands (a suit's packages' anims too)
            for (const char* T : HandsTypes)
                if (const auto It = Sets.find(T); It != Sets.end() && !ViewHands.empty())
                    for (const auto& [N, Anims] : It->second)
                    {
                        const std::string Id = std::string(T) + ":" + N;
                        std::vector<std::string> All;
                        for (const auto& A : *Anims)
                            if (Starts(A, "suitanimpackage "))
                            {
                                if (const auto* S = Set("suitanimpackage", A.substr(16))) All.insert(All.end(), S->begin(), S->end());
                            }
                            else All.push_back(A);
                        for (const auto& A : All)
                            if (Exported.count(A))
                            {
                                if (!Out.Owners.count(Id))
                                {
                                    ::AnimOwners::Owner O{ {}, std::string(T) + " " + N + " -> the view hands most weapons name, " + ViewHands };
                                    Split(ViewHands, O.Models);
                                    Out.Owners[Id] = std::move(O);
                                }
                                Add(Ids, A, Id);
                            }
                    }

            // a tree's owners: the player's trees the characters first, then the layout's models
            const auto TreeOwners = [&](const std::string& Tree) {
                std::vector<std::string> Got;
                const bool Player = std::find(L.PlayerTrees.begin(), L.PlayerTrees.end(), Tree) != L.PlayerTrees.end();
                if (Player)
                    for (const Facts* F : Sources)
                    {
                        for (const auto& [Script, Models] : F->Characters)
                        {
                            const std::string Id = "character:" + Script;
                            if (!Out.Owners.count(Id)) Out.Owners[Id] = { Models, "scriptfile " + Script + " (compiled; its strings)" };
                            Got.push_back(Id);
                        }
                        if (!F->Characters.empty()) break;
                    }
                // a level's AI: the level's own soldier (its own zone's)
                if (std::find(L.LevelTrees.begin(), L.LevelTrees.end(), Tree) != L.LevelTrees.end() && !Sources.front()->Bodies.empty())
                {
                    const Facts& F = *Sources.front();
                    const auto Body = std::find_if(F.Bodies.begin(), F.Bodies.end(), [](const std::string& B) { return B.find("_civ_") == std::string::npos; });
                    const std::string& B = Body != F.Bodies.end() ? *Body : F.Bodies.front();
                    const std::string Id = "character:" + B;
                    if (!Out.Owners.count(Id))
                    {
                        ::AnimOwners::Owner O{ { B }, "the level's soldier (" + B };
                        if (!F.Heads.empty()) O.Models.push_back(F.Heads.front()), O.Source += ", " + F.Heads.front();
                        O.Source += "), a stand-in for its AI types (scripts)";
                        Out.Owners[Id] = std::move(O);
                    }
                    Got.push_back(Id);
                }
                if (const auto It = L.Trees.find(Tree); It != L.Trees.end() && !It->second.Models.empty())
                {
                    const std::string Id = "animtree:" + Tree;
                    if (!Out.Owners.count(Id)) Out.Owners[Id] = { It->second.Models, "animtrees/" + Tree + ".atr -> " + It->second.Source };
                    Got.push_back(Id);
                }
                return Got;
            };

            // 3. animclasses: the anims their states name, on the model beside them named after them
            // (zombie_ghost_animclass: zombie_ghost_green), else on their tree's (the .atr a state
            // names, else the tree their name starts with: dog_animclass -> dog_mp), else on a body
            // beside them holding their name (mp_fullbody_dog_a)
            if (const auto It = Sets.find("animclass"); It != Sets.end())
                for (const auto& [N, Strings] : It->second)
                {
                    std::string Stem = N;
                    if (const size_t P = Stem.find("_animclass"); P != std::string::npos) Stem = Stem.substr(0, P);
                    ::AnimOwners::Owner O;
                    for (const Facts* F : Sources)
                        if (const auto C = F->ClassModels.find(N); O.Models.empty() && C != F->ClassModels.end())
                            O = { { C->second.front() }, "animclass " + N + " -> the model beside it named after it, " + C->second.front() };
                    for (const auto& S : *Strings)
                        if (Ends(S, ".atr"))
                            if (const auto T = L.Trees.find(S.substr(0, S.size() - 4)); O.Models.empty() && T != L.Trees.end() && !T->second.Models.empty())
                            {
                                O = { T->second.Models, "animclass " + N + " -> " + S + " -> " + T->second.Source };
                                break;
                            }
                    for (const auto& [T, M] : L.Trees)
                        if (O.Models.empty() && (T == Stem || Starts(T, Stem + "_")) && !M.Models.empty())
                            O = { M.Models, "animclass " + N + " -> animtrees/" + T + ".atr -> " + M.Source };
                    for (const Facts* F : Sources)
                        if (const auto C = F->ClassNear.find(N); O.Models.empty() && C != F->ClassNear.end())
                            O = { { C->second.front() }, "animclass " + N + " -> the body beside it holding its name, " + C->second.front() };
                    if (O.Models.empty()) continue;
                    const std::string Id = "animclass:" + N;
                    for (const auto& A : *Strings)
                        if (Exported.count(A))
                        {
                            if (!Out.Owners.count(Id)) Out.Owners[Id] = O;
                            Add(Ids, A, Id);
                        }
                }

            // 4. player anim sets (Infinite Warfare): the layout's models for the set, else the human
            // sets on the player's body (the robot's, C8's and the Reaper's are other bodies)
            if (const auto It = Sets.find("playeranim"); It != Sets.end() && !L.PlayerTrees.empty())
                for (const auto& [N, Anims] : It->second)
                {
                    auto Body = L.Trees.find(N);
                    if (Body == L.Trees.end() && Starts(N, "human")) Body = L.Trees.find(L.PlayerTrees.front());
                    if (Body == L.Trees.end() || Body->second.Models.empty()) continue;
                    {
                        const std::string Id = "playeranim:" + N;
                        for (const auto& A : *Anims)
                            if (Exported.count(A))
                            {
                                if (!Out.Owners.count(Id)) Out.Owners[Id] = { Body->second.Models, "playeranim " + N + " -> " + Body->second.Source };
                                Add(Ids, A, Id);
                            }
                    }
                }

            // 5. the trees holding the anim (a tree as all the sources give it: a patch's adds anims)
            std::map<std::string, std::set<std::string>> Trees;
            for (const Facts* F : Sources)
                for (const auto& [T, Anims] : F->Trees) Trees[T].insert(Anims.begin(), Anims.end());
            for (const auto& [T, Anims] : Trees)
            {
                const auto Owners = TreeOwners(T);
                if (Owners.empty()) continue;
                for (const auto& A : Anims)
                    if (Exported.count(A))
                        for (const auto& Id : Owners) Add(Ids, A, Id);
            }

            // owners with the same models given once, MaxOwners at most
            for (const auto& [A, List] : Ids)
            {
                std::set<std::string> Seen;
                size_t N = 0;
                for (const auto& Id : List)
                {
                    const auto O = Out.Owners.find(Id);
                    if (O == Out.Owners.end() || O->second.Models.empty() || !Seen.insert(ModelsKey(O->second)).second) continue;
                    if (N++ >= MaxOwners) break;
                    Out.Own(Exported.at(A), Id);
                }
            }
        }
    }
}
