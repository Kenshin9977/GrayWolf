#include "stdafx.h"

#include "IwClassicAnimOwners.h"

#include "ExportNaming.h"
#include "Games/Common/WeaponFiles.h"
#include "Games/OAT/OatWeapons.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace IwClassicAnimOwners
{
    namespace
    {
        using ExportNaming::Lower;

        // a model as the model kind names it: lower case, without CoD1's and CoD2's "xmodel/"
        std::string Model(std::string Name)
        {
            Name = Lower(Name);
            if (Name.rfind("xmodel/", 0) == 0 || Name.rfind("xmodel\\", 0) == 0) Name.erase(0, 7);
            return Name;
        }

        // "character/mp_x.gsc" -> ("character", "mp_x"), lower case, '/' or '\'
        std::pair<std::string, std::string> FolderStem(const std::string& Name)
        {
            std::string N = Lower(Name);
            for (auto& C : N) if (C == '\\') C = '/';
            const size_t Slash = N.rfind('/'), Dot = N.rfind('.');
            const std::string Folder = Slash == std::string::npos ? std::string() : N.substr(0, Slash);
            const size_t From = Slash == std::string::npos ? 0 : Slash + 1;
            return { Folder, N.substr(From, Dot == std::string::npos || Dot < From ? std::string::npos : Dot - From) };
        }

        bool EndsWith(const std::string& S, const char* End)
        {
            const size_t N = std::strlen(End);
            return S.size() >= N && S.compare(S.size() - N, N, End) == 0;
        }

        std::string Trim(const std::string& S)
        {
            size_t A = 0, B = S.size();
            while (A < B && std::isspace((unsigned char)S[A])) A++;
            while (B > A && std::isspace((unsigned char)S[B - 1])) B--;
            return S.substr(A, B - A);
        }

        // a weapon file: weapon:<file name> on its hands then its gun (joined to the hands); the anims its *Anim, *AnimR,
        // *AnimL fields name, and MW3's animOverrides lines' (attachment, attachment, type, anim,
        // alt-mode anim, times)
        void Weapon(const std::string& Name, const std::string& Text, Facts& F)
        {
            const auto Fields = WeaponFiles::Parse(Text);
            const auto Field = [&](const char* K) {
                const auto It = Fields.find(K);
                return It == Fields.end() ? std::string() : Model(Trim(It->second));
            };
            ::AnimOwners::Owner O;
            for (const auto& M : { Field("handModel"), Field("gunModel") })
                if (!M.empty()) O.Models.push_back(M);
            if (O.Models.empty()) return;
            const std::string Id = "weapon:" + Lower(fs::path(Name).filename().string());
            O.Source = "weapon file " + Name;
            const auto Add = [&](const std::string& Anim) {
                const std::string A = Lower(Trim(Anim));
                if (A.empty() || A == "none" || A.find_first_of(" \r\n") != std::string::npos) return;
                if (std::all_of(A.begin(), A.end(), [](char C) { return std::isdigit((unsigned char)C) || C == '.'; })) return;
                auto& Ids = F.WeaponAnims[A];
                if (std::find(Ids.begin(), Ids.end(), Id) == Ids.end()) Ids.push_back(Id);
            };
            for (const auto& [K, V] : Fields)
                if (EndsWith(K, "Anim") || EndsWith(K, "AnimR") || EndsWith(K, "AnimL")) Add(V);
            if (const auto It = Fields.find("animOverrides"); It != Fields.end())
            {
                std::istringstream Lines(It->second);
                for (std::string Line; std::getline(Lines, Line);)
                {
                    std::istringstream Words(Line);
                    std::vector<std::string> W;
                    for (std::string S; Words >> S;) W.push_back(S);
                    for (size_t I = 3; I < 5 && I < W.size(); I++) Add(W[I]);
                }
            }
            F.Weapons[Id] = std::move(O);
        }

        // MW3's compiled scripts: a small one whose strings hold a multiplayer body (mp_body_*,
        // mp_fullbody_*) is a character; its first head_* string its head (an xmodelalias of heads,
        // a script of its own known by number, is left out)
        void CompiledCharacter(const std::string& Name, const std::string& Text, Facts& F)
        {
            if (Text.size() > 2048) return;
            std::vector<std::string> Strings;
            for (size_t I = 0; I < Text.size();)
            {
                size_t E = I;
                while (E < Text.size() && Text[E] >= 0x20 && Text[E] < 0x7f) E++;
                if (E - I >= 3) Strings.push_back(Lower(Text.substr(I, E - I)));
                I = E + 1;
            }
            std::string Body, Head;
            for (const auto& S : Strings)
            {
                if (Body.empty() && (S.rfind("mp_body_", 0) == 0 || S.rfind("mp_fullbody_", 0) == 0)) Body = S;
                if (Head.empty() && S.rfind("head_", 0) == 0) Head = S;
            }
            if (Body.empty()) return;
            const std::string Stem = FolderStem(Name).second;
            std::string Id = Body;
            if (const auto It = F.Gsc.Characters.find(Id); It != F.Gsc.Characters.end() && It->second.Head != Head) Id += "_" + Stem;
            if (F.Gsc.Characters.count(Id)) return;
            F.Gsc.Characters[Id] = { Body, {}, Head, {} };
            F.Gsc.Types.push_back({ "character", Id, "multiplayer", { Id } });
            F.Sources["character:" + Id] = "scriptfile " + Stem + " (compiled; its strings: " + Body + (Head.empty() ? "" : ", " + Head) + ")";
        }

        // what IW's character files say apart from the GSC helper: a head attached
        // (attachFromArray / attachHead of an xmodelalias, attach("...head...")), models without "xmodel/"
        void CharacterHeads(const std::vector<std::pair<std::string, std::string>>& Docs, GscAnimOwners::Facts& G)
        {
            static const std::regex AliasCall(R"re(xmodelalias[\\/](\w+)::main)re", std::regex::icase);
            static const std::regex Attach(R"re(attach\s*\(\s*"([^"]+)")re", std::regex::icase);
            for (const auto& [Name, Text] : Docs)
            {
                const auto [Folder, Stem] = FolderStem(Name);
                if (Folder != "character" || !EndsWith(Lower(Name), ".gsc")) continue;
                const auto It = G.Characters.find(Stem);
                if (It == G.Characters.end()) continue;
                auto& Ch = It->second;
                if (!Ch.Head.empty() || !Ch.HeadAlias.empty()) continue;
                const size_t P = Text.find("precache()");
                const std::string Main = P == std::string::npos ? Text : Text.substr(0, P);
                for (std::sregex_iterator M(Main.begin(), Main.end(), AliasCall), End; M != End; ++M)
                    if (const std::string A = Lower((*M)[1].str()); A != Ch.BodyAlias)
                    {
                        Ch.HeadAlias = A;
                        break;
                    }
                if (!Ch.HeadAlias.empty()) continue;
                for (std::sregex_iterator M(Main.begin(), Main.end(), Attach), End; M != End; ++M)
                    if (const std::string A = Model((*M)[1].str()); A.find("head") != std::string::npos)
                    {
                        Ch.Head = A;
                        break;
                    }
            }
            for (auto& [N, Ch] : G.Characters)
            {
                if (!Ch.Body.empty()) Ch.Body = Model(Ch.Body);
                if (!Ch.Head.empty()) Ch.Head = Model(Ch.Head);
            }
            for (auto& [N, List] : G.Aliases)
                for (auto& M : List) M = Model(M);
        }

        // how much of the anim's name the gun's starts with (viewmodel_ak47_ for viewmodel_ak47_reload)
        size_t Likeness(const std::string& Anim, const std::string& Gun)
        {
            size_t N = 0;
            while (N < Anim.size() && N < Gun.size() && Anim[N] == Gun[N]) N++;
            return N;
        }

        std::string ModelsKey(const ::AnimOwners::Owner& O)
        {
            std::string K;
            for (const auto& M : O.Models) K += Lower(M) + "|";
            return K;
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

        // the zone's weapon files, through OpenAssetTools
        void WeaponFilesOf(const std::string& Zone, std::vector<std::pair<std::string, std::string>>& Docs)
        {
            OatWeapons::Source S(Zone);
            const std::vector<std::pair<std::string, std::string>>* Files = nullptr;
            size_t Assets = 0;
            std::string Error;
            if (!S.Files(Files, Assets, Error) || Files == nullptr) return;
            for (const auto& [Path, Text] : *Files)
                if (WeaponFiles::IsWeaponFile(Text)) Docs.emplace_back(Path, Text);
        }

        // Zone's facts: its documents (Known when the run kept them, else read from the zone opened
        // here) and its weapon files
        std::shared_ptr<const Facts> FactsOf(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& Options, const fs::path& Zone,
                                             const std::vector<std::pair<std::string, std::string>>* Known)
        {
            std::error_code Ec, Ec2;
            const uintmax_t Size = fs::file_size(Zone, Ec);
            const auto When = fs::last_write_time(Zone, Ec2);
            if (Ec || Ec2) return nullptr;
            const std::wstring Key = Zone.wstring();
            {
                std::lock_guard<std::mutex> G(CacheLock);
                if (const auto It = Cache.find(Key); It != Cache.end() && It->second.Size == Size && It->second.When == When) return It->second.F;
            }
            std::vector<std::pair<std::string, std::string>> Docs;
            if (Known) Docs = *Known;
            else
            {
                std::string Error;
                if (auto Z = Game.Open(Zone.string(), Options, Error)) Z->Documents(Docs);
            }
            WeaponFilesOf(Zone.string(), Docs);
            auto F = std::make_shared<const Facts>(Parse(Docs));
            std::lock_guard<std::mutex> G(CacheLock);
            Cache[Key] = { Size, When, F };
            return F;
        }

        // the zones the game loads with Zone, the one overriding the others first: its patch
        // (MW3's patch_<zone>), then multiplayer's patch_mp, common_mp, else patch, common
        std::vector<fs::path> SharedZones(const fs::path& Zone)
        {
            const std::string Stem = Lower(Zone.stem().string());
            const bool Mp = Stem.rfind("mp_", 0) == 0 || EndsWith(Stem, "_mp") || Stem.find("_mp_") != std::string::npos;
            std::vector<std::string> Names{ "patch_" + Stem };
            for (const char* N : Mp ? std::vector<const char*>{ "patch_mp", "common_mp" } : std::vector<const char*>{ "patch", "common" }) Names.push_back(N);
            std::vector<fs::path> Out;
            std::error_code Ec;
            for (const auto& N : Names)
                if (N != Stem)
                    if (const fs::path P = Zone.parent_path() / (N + ".ff"); fs::exists(P, Ec)) Out.push_back(P);
            return Out;
        }
    }

    std::vector<std::string> Documents()
    {
        auto D = GscAnimOwners::Documents();
        D.push_back("*.gscsource");
        return D;
    }

    Facts Parse(const std::vector<std::pair<std::string, std::string>>& Docs)
    {
        Facts F;
        F.Gsc = GscAnimOwners::Parse(Docs);
        CharacterHeads(Docs, F.Gsc);
        std::vector<const std::pair<std::string, std::string>*> Compiled;
        for (const auto& D : Docs)
        {
            if (WeaponFiles::IsWeaponFile(D.second)) Weapon(D.first, D.second, F);
            else if (EndsWith(Lower(D.first), ".gscsource")) Compiled.push_back(&D);
        }
        // MW3's compiled scripts by number (the order its tools wrote them in), not the zone's
        const auto Number = [](const std::string& Name) {
            const std::string Stem = FolderStem(Name).second;
            const bool Digits = !Stem.empty() && Stem.size() < 19 &&
                                std::all_of(Stem.begin(), Stem.end(), [](char C) { return std::isdigit((unsigned char)C) != 0; });
            return Digits ? std::stoull(Stem) : ~0ull;
        };
        std::stable_sort(Compiled.begin(), Compiled.end(), [&](const auto* A, const auto* B) { return Number(A->first) < Number(B->first); });
        for (const auto* D : Compiled) CompiledCharacter(D->first, D->second, F);
        return F;
    }

    void Own(const std::vector<const Facts*>& Sources, const std::vector<std::string>& Anims, ::AnimOwners::Book& Out)
    {
        // the weapons naming the anim, of the first source holding any
        for (const auto& Anim : Anims)
        {
            const std::string L = Lower(Anim);
            for (const Facts* F : Sources)
            {
                const auto It = F->WeaponAnims.find(L);
                if (It == F->WeaponAnims.end() || It->second.empty()) continue;
                std::vector<std::pair<const std::string*, size_t>> Ids;
                for (const auto& Id : It->second) Ids.push_back({ &Id, Likeness(L, F->Weapons.at(Id).Models.back()) });
                std::stable_sort(Ids.begin(), Ids.end(), [](const auto& A, const auto& B) {
                    if (A.second != B.second) return A.second > B.second;
                    return A.first->size() < B.first->size();
                });
                for (const auto& [Id, Score] : Ids)
                {
                    Out.Owners[*Id] = F->Weapons.at(*Id);
                    Out.Own(Anim, *Id);
                }
                break;
            }
        }
        // its tree's AI and player types
        std::vector<const GscAnimOwners::Facts*> Gsc;
        for (const Facts* F : Sources) Gsc.push_back(&F->Gsc);
        GscAnimOwners::Own(Gsc, Anims, Out);
        for (auto F = Sources.rbegin(); F != Sources.rend(); ++F)
            for (const auto& [Id, Source] : (*F)->Sources)
                if (const auto It = Out.Owners.find(Id); It != Out.Owners.end()) It->second.Source = Source;
        // owners with the same models given once (alternatives no rig tells apart), MaxOwners at most
        for (const auto& Anim : Anims)
        {
            const auto It = Out.Anims.find(Anim);
            if (It == Out.Anims.end()) continue;
            std::set<std::string> Seen;
            std::vector<std::string> Keep;
            for (const auto& Id : It->second)
            {
                const auto O = Out.Owners.find(Id);
                if (O == Out.Owners.end() || !Seen.insert(ModelsKey(O->second)).second) continue;
                if (Keep.size() < MaxOwners) Keep.push_back(Id);
            }
            It->second = std::move(Keep);
        }
    }

    void Fill(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& Options,
              const std::vector<DiskGame::IDiskGame::AnimZone>& Zones, ::AnimOwners::Book& Out)
    {
        // the run's zones first: their documents are known, and one may be another's shared zone
        std::vector<std::shared_ptr<const Facts>> Own;
        for (const auto& Z : Zones) Own.push_back(FactsOf(Game, Options, fs::path(Z.Zone), &Z.Documents));
        for (size_t I = 0; I < Zones.size(); I++)
        {
            std::vector<std::shared_ptr<const Facts>> Held{ Own[I] };
            for (const auto& P : SharedZones(fs::path(Zones[I].Zone))) Held.push_back(FactsOf(Game, Options, P, nullptr));
            std::vector<const Facts*> Sources;
            for (const auto& F : Held)
                if (F) Sources.push_back(F.get());
            IwClassicAnimOwners::Own(Sources, Zones[I].Anims, Out);
        }
    }
}
