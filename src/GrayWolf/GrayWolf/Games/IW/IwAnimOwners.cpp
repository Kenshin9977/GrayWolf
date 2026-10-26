#include "stdafx.h"

#include "IwAnimOwners.h"

#include "AnimOwners.h"
#include "ExportNaming.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace IwAnimOwners
{
    namespace
    {
        // a field of the document: no tab, no line break
        std::string Field(const std::string& S)
        {
            std::string Out = S;
            for (char& Ch : Out)
                if (Ch == '\t' || Ch == '\n' || Ch == '\r') Ch = ' ';
            return Out;
        }

        std::vector<std::string> Split(const std::string& Line)
        {
            std::vector<std::string> Out;
            size_t Start = 0;
            for (size_t Tab; (Tab = Line.find('\t', Start)) != std::string::npos; Start = Tab + 1) Out.push_back(Line.substr(Start, Tab - Start));
            Out.push_back(Line.substr(Start));
            return Out;
        }

        bool PlainByte(char Ch) { return (Ch >= 'a' && Ch <= 'z') || (Ch >= '0' && Ch <= '9') || Ch == '_'; }

        // a name's words ('_' between them, lower case)
        std::set<std::string> Tokens(const std::string& Name)
        {
            std::set<std::string> Out;
            std::string W;
            for (const char Ch : ExportNaming::Lower(Name) + "_")
                if (Ch == '_')
                {
                    if (!W.empty()) Out.insert(W);
                    W.clear();
                }
                else W += Ch;
            return Out;
        }
    }

    // set <key> <source> <anim>...; char <archetype key> <archetype> <source> <model>...;
    // direct <id> <source> <model count> <model>... <anim>...
    std::string Write(const Facts& F)
    {
        std::string Out;
        for (const auto& S : F.Sets)
        {
            Out += "set\t" + Field(S.Key) + "\t" + Field(S.Source);
            for (const auto& A : S.Anims) Out += "\t" + Field(A);
            Out += "\n";
        }
        for (const auto& C : F.Characters)
        {
            Out += "char\t" + Field(C.Archetype) + "\t" + Field(C.Name) + "\t" + Field(C.Source);
            for (const auto& M : C.Models) Out += "\t" + Field(M);
            Out += "\n";
        }
        for (const auto& D : F.Directs)
        {
            Out += "direct\t" + Field(D.Id) + "\t" + Field(D.Source) + "\t" + std::to_string(D.Models.size());
            for (const auto& M : D.Models) Out += "\t" + Field(M);
            for (const auto& A : D.Anims) Out += "\t" + Field(A);
            Out += "\n";
        }
        return Out;
    }

    Facts Read(const std::string& Text)
    {
        Facts F;
        std::istringstream In(Text);
        for (std::string Line; std::getline(In, Line);)
        {
            if (!Line.empty() && Line.back() == '\r') Line.pop_back();
            const auto P = Split(Line);
            if (P[0] == "set" && P.size() >= 3)
                F.Sets.push_back({ P[1], P[2], std::vector<std::string>(P.begin() + 3, P.end()) });
            else if (P[0] == "char" && P.size() >= 4)
                F.Characters.push_back({ P[1], P[2], P[3], std::vector<std::string>(P.begin() + 4, P.end()) });
            else if (P[0] == "direct" && P.size() >= 4)
            {
                const size_t N = std::min<size_t>(std::strtoul(P[3].c_str(), nullptr, 10), P.size() - 4);
                F.Directs.push_back({ P[1], P[2], std::vector<std::string>(P.begin() + 4, P.begin() + 4 + (std::ptrdiff_t)N),
                                      std::vector<std::string>(P.begin() + 4 + (std::ptrdiff_t)N, P.end()) });
            }
        }
        return F;
    }

    Facts FromDocuments(const std::vector<std::pair<std::string, std::string>>& Docs)
    {
        Facts F;
        for (const auto& [Name, Text] : Docs)
        {
            if (ExportNaming::Lower(Name) != DocumentName) continue;
            Facts One = Read(Text);
            for (auto& S : One.Sets) F.Sets.push_back(std::move(S));
            for (auto& C : One.Characters) F.Characters.push_back(std::move(C));
            for (auto& D : One.Directs) F.Directs.push_back(std::move(D));
        }
        return F;
    }

    void Own(const std::vector<const Facts*>& Sources, const std::vector<std::string>& Anims,
             const std::function<std::string(const std::string&)>& Key, AnimOwners::Book& Out)
    {
        // an archetype's character: the first with models, of the first source giving one
        std::unordered_map<std::string, const Character*> Playing;
        for (const Facts* F : Sources)
            for (const auto& C : F->Characters)
                if (!C.Models.empty() && !C.Name.empty()) Playing.try_emplace(C.Archetype, &C);
        // the owners: a set with a character (its size its largest), a direct owner (one per id)
        struct Candidate
        {
            std::string Id;
            std::set<std::string> Words;   // its name's
            size_t Size = 0;
            AnimOwners::Owner Owner;
        };
        std::map<std::string, Candidate> Candidates;
        std::unordered_map<std::string, std::vector<std::string>> Of;   // anim key -> candidate ids
        const auto Add = [&](const std::string& Id, const std::string& Name, const std::vector<std::string>& Keys, AnimOwners::Owner Owner) {
            const std::set<std::string> Unique(Keys.begin(), Keys.end());
            auto [It, New] = Candidates.try_emplace(Id);
            if (New) It->second = { Id, Tokens(Name), 0, std::move(Owner) };
            It->second.Size = std::max(It->second.Size, Unique.size());
            for (const auto& A : Unique)
            {
                auto& L = Of[A];
                if (std::find(L.begin(), L.end(), Id) == L.end()) L.push_back(Id);
            }
        };
        for (const Facts* F : Sources)
        {
            for (const auto& S : F->Sets)
                if (const auto It = Playing.find(S.Key); It != Playing.end())
                    Add("archetype:" + It->second->Name, It->second->Name, S.Anims, { It->second->Models, S.Source + " -> " + It->second->Source });
            for (const auto& D : F->Directs)
                if (!D.Models.empty()) Add(D.Id, D.Id.substr(D.Id.find(':') + 1), D.Anims, { D.Models, D.Source });
        }
        // an anim's owners: the one whose name the anim's names the most of first (zombie_base before
        // zombie_base_abom_crawler for ai_t9_zm_zombie_base_walk, iw8_ar_mike4 for vm_ar_mike4_idle),
        // then the one with the fewest anims (the most specific)
        for (const auto& Anim : Anims)
        {
            const auto It = Of.find(Key(Anim));
            if (It == Of.end()) continue;
            const std::set<std::string> Words = Tokens(Anim);
            std::vector<std::pair<double, const Candidate*>> Ranked;
            for (const auto& Id : It->second)
            {
                const Candidate& C = Candidates.at(Id);
                size_t In = 0;
                for (const auto& W : C.Words) In += Words.count(W);
                Ranked.emplace_back(C.Words.empty() ? 0.0 : (double)In / (double)C.Words.size(), &C);
            }
            std::stable_sort(Ranked.begin(), Ranked.end(), [](const auto& A, const auto& B) {
                return A.first != B.first ? A.first > B.first : A.second->Size < B.second->Size;
            });
            for (const auto& [Affinity, C] : Ranked)
            {
                if (!Out.Owners.count(C->Id)) Out.Owners[C->Id] = C->Owner;
                Out.Own(Anim, C->Id);
            }
        }
    }

    namespace
    {
        // a shared zone's facts, read once per file (its size and time unchanged)
        std::shared_ptr<const Facts> SharedFacts(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& Options, const fs::path& Zone)
        {
            struct Kept
            {
                uintmax_t Size = 0;
                fs::file_time_type When;
                std::shared_ptr<const Facts> F;
            };
            static std::mutex Lock;
            static std::map<std::wstring, Kept> Cache;
            std::error_code Ec, Ec2;
            const uintmax_t Size = fs::file_size(Zone, Ec);
            const auto When = fs::last_write_time(Zone, Ec2);
            if (Ec || Ec2) return nullptr;
            const std::wstring Name = Zone.wstring();
            {
                std::lock_guard<std::mutex> G(Lock);
                if (const auto It = Cache.find(Name); It != Cache.end() && It->second.Size == Size && It->second.When == When) return It->second.F;
            }
            std::string Error;
            std::vector<std::pair<std::string, std::string>> Docs;
            if (auto Z = Game.Open(Zone.string(), Options, Error)) Z->Documents(Docs);
            auto F = std::make_shared<const Facts>(FromDocuments(Docs));
            std::lock_guard<std::mutex> G(Lock);
            Cache[Name] = { Size, When, F };
            return F;
        }
    }

    void Fill(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& Options,
              const std::vector<DiskGame::IDiskGame::AnimZone>& Zones, const std::function<std::vector<std::string>()>& Shared,
              const std::function<std::string(const std::string&)>& Key, AnimOwners::Book& Out)
    {
        std::vector<Facts> RunFacts;
        std::set<std::wstring> Run;
        for (const auto& Z : Zones)
        {
            RunFacts.push_back(FromDocuments(Z.Documents));
            std::error_code Ec;
            Run.insert(fs::weakly_canonical(Z.Zone, Ec).wstring());
        }
        std::vector<const Facts*> Sources;
        for (const auto& F : RunFacts) Sources.push_back(&F);
        AnimOwners::Book Book;
        for (const auto& Z : Zones) Own(Sources, Z.Anims, Key, Book);
        // the shared zones' facts only when an anim is left without an owner
        const auto Left = [&] {
            for (const auto& Z : Zones)
                for (const auto& A : Z.Anims)
                    if (!Book.Anims.count(A)) return true;
            return false;
        };
        std::vector<std::shared_ptr<const Facts>> Kept;
        if (Shared && Left())
            for (const auto& P : Shared())
            {
                std::error_code Ec;
                if (Run.count(fs::weakly_canonical(P, Ec).wstring())) continue;
                if (auto F = SharedFacts(Game, Options, P)) Kept.push_back(std::move(F));
            }
        if (!Kept.empty())
        {
            for (const auto& F : Kept) Sources.push_back(F.get());
            Book = {};
            for (const auto& Z : Zones) Own(Sources, Z.Anims, Key, Book);
        }
        for (auto& [Id, O] : Book.Owners) Out.Owners.try_emplace(Id, std::move(O));
        for (const auto& [Anim, Ids] : Book.Anims)
            for (const auto& Id : Ids) Out.Own(Anim, Id);
    }

    bool Plain(const std::string& Text)
    {
        return !Text.empty() && std::all_of(Text.begin(), Text.end(), PlainByte);
    }

    std::string Repaired(const std::string& Text, const std::function<bool(const std::string&)>& Known)
    {
        if (Text.empty() || Known(Text)) return Text;
        static const std::string Alphabet = "abcdefghijklmnopqrstuvwxyz0123456789_";
        std::vector<std::string> Found;
        std::string Try = Text;
        for (size_t I = 0; I < Text.size() && Found.size() < 2; I++)
        {
            for (const char Ch : Alphabet)
                if (Ch != Text[I] && (Try[I] = Ch, Known(Try))) Found.push_back(Try);
            Try[I] = Text[I];
        }
        if (Found.empty())
        {
            std::vector<size_t> Bad;
            for (size_t I = 0; I < Text.size(); I++)
                if (!PlainByte(Text[I])) Bad.push_back(I);
            for (size_t A = 0; Bad.size() <= 4 && A < Bad.size(); A++)
                for (size_t B = A + 1; B < Bad.size(); B++)
                    for (const char X : Alphabet)
                        for (const char Y : Alphabet)
                        {
                            Try[Bad[A]] = X, Try[Bad[B]] = Y;
                            if (Known(Try)) Found.push_back(Try);
                            Try[Bad[A]] = Text[Bad[A]], Try[Bad[B]] = Text[Bad[B]];
                        }
        }
        return Found.size() == 1 ? Found[0] : Text;
    }
}
