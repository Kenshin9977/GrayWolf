#include "stdafx.h"

#include "Bo3AnimOwners.h"

#include "AnimOwners.h"
#include "ExportNaming.h"
#include "Gdt.h"
#include "Games/T7/Gdt/AnimMappingTable.h"
#include "ZonekitZone.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>

namespace fs = std::filesystem;

namespace Bo3AnimOwners
{
    namespace
    {
        using ExportNaming::Lower;
        using Json = nlohmann::ordered_json;
        constexpr const char* Folder = "animowners/";

        std::string Strip(std::string S)
        {
            if (!S.empty() && S[0] == ',') S.erase(0, 1);   // a reference: the asset's name without it
            return S;
        }

        // a data type's keys as the gdt kind writes them (its layouts' fields)
        std::map<std::string, std::string> Keys(const Gdt::Type& Ty, const Gdt::Memory& M, int64_t Pos)
        {
            Gdt::Stats S;
            std::map<std::string, std::string> Out;
            for (auto& [K, V] : Ty.Fields(M, Pos, S)) Out.emplace(K, Strip(V));
            return Out;
        }

        // the gdt kind's entries of an asset: its type's converter's blocks, else its fields
        std::vector<Gdt::Block> Blocks(const Gdt::Type& Ty, const Gdt::Memory& M, int64_t Pos, const std::string& Name)
        {
            std::vector<Gdt::Block> Out;
            Gdt::Stats S;
            if (const auto Fn = Gdt::Find(Ty.Settings().value("converter", std::string())))
                Fn(Ty, M, Pos, Name, Out, S);
            else
                Out.push_back({ Name, "", "", Ty.Fields(M, Pos, S) });
            return Out;
        }

        std::string Get(const std::map<std::string, std::string>& K, const std::string& Key)
        {
            const auto It = K.find(Key);
            return It == K.end() ? std::string() : It->second;
        }

        // a zone-level asset's name, "" for a reference another zone defines
        std::string Own(const Gdt::Type& Ty, const Gdt::Memory& M, int64_t Pos)
        {
            std::string N = Ty.Name(M, Pos);
            return N.empty() || N[0] == ',' ? std::string() : N;
        }

        bool IsNumber(const std::string& S)
        {
            return !S.empty() && std::all_of(S.begin(), S.end(), [](char C) { return std::isdigit((unsigned char)C) || C == '.' || C == '-'; });
        }

        void ReadAiTypes(const ZonekitZone& Z, Json& F, std::string& Error)
        {
            Json List = Json::array();
            Z.ForEachGdtAsset("aitype", [&](const Gdt::Type& Ty, const Gdt::Memory& M, int64_t Pos) {
                const std::string Name = Own(Ty, M, Pos);
                if (Name.empty()) return;
                const auto K = Keys(Ty, M, Pos);
                Json Chars = Json::array(), Tables = Json::array();
                for (int I = 1; I <= 8; I++)
                    if (auto C = Get(K, "character" + std::to_string(I)); !C.empty()) Chars.push_back(C);
                for (const char* T : { "animMappingTableName", "animMappingTableName2", "animMappingTableName3" })
                    if (auto A = Get(K, T); !A.empty()) Tables.push_back(A);
                if (Chars.empty() && Tables.empty()) return;
                List.push_back({ { "name", Name }, { "archetype", Get(K, "archetype") }, { "characters", Chars }, { "am", Tables } });
            }, Error);
            F["aitypes"] = std::move(List);
        }

        void ReadCharacters(const ZonekitZone& Z, Json& F, std::string& Error)
        {
            Json Out = Json::object();
            Z.ForEachGdtAsset("character", [&](const Gdt::Type& Ty, const Gdt::Memory& M, int64_t Pos) {
                const std::string Name = Own(Ty, M, Pos);
                if (Name.empty()) return;
                const auto K = Keys(Ty, M, Pos);
                Json C = Json::object();
                for (const char* Key : { "body", "bodyAlias", "head", "headAlias", "hat", "hatAlias", "gear", "gearAlias" })
                    if (auto V = Get(K, Key); !V.empty()) C[Key] = V;
                if (!C.empty()) Out[Name] = std::move(C);
            }, Error);
            F["characters"] = std::move(Out);
        }

        void ReadAliases(const ZonekitZone& Z, Json& F, std::string& Error)
        {
            Json Out = Json::object();
            Z.ForEachGdtAsset("xmodelalias", [&](const Gdt::Type& Ty, const Gdt::Memory& M, int64_t Pos) {
                const std::string Name = Own(Ty, M, Pos);
                if (Name.empty()) return;
                const auto K = Keys(Ty, M, Pos);
                Json Models = Json::array();
                for (int I = 1; I <= 50; I++)
                    if (auto V = Get(K, "model" + std::to_string(I)); !V.empty()) Models.push_back(V);
                if (!Models.empty()) Out[Name] = std::move(Models);
            }, Error);
            F["aliases"] = std::move(Out);
        }

        void ReadAnimTables(const ZonekitZone& Z, Json& F, std::string& Error)
        {
            Json Out = Json::object();
            Z.ForEachGdtAsset("animmappingtable", [&](const Gdt::Type& Ty, const Gdt::Memory& M, int64_t Pos) {
                const std::string Name = Own(Ty, M, Pos);
                if (Name.empty()) return;
                Json Anims = Json::array();
                std::set<std::string> Seen;
                for (const auto& [Alias, List] : T7Gdt::ReadAnimMappingTable(M, Pos))
                    for (const auto& A : List)
                        if (!A.empty() && Seen.insert(Lower(A)).second) Anims.push_back(A);
                if (!Anims.empty()) Out[Name] = std::move(Anims);
            }, Error);
            F["am"] = std::move(Out);
        }

        // board n: boardModel<n>, its boardAnim<n> and tearAnim<n>
        void ReadBarriers(const ZonekitZone& Z, Json& F, std::string& Error)
        {
            Json List = Json::array();
            Z.ForEachGdtAsset("zbarrier", [&](const Gdt::Type& Ty, const Gdt::Memory& M, int64_t Pos) {
                const std::string Name = Own(Ty, M, Pos);
                if (Name.empty()) return;
                const auto K = Keys(Ty, M, Pos);
                Json Boards = Json::array();
                for (int I = 1; I <= 6; I++)
                {
                    const std::string N = std::to_string(I), Model = Get(K, "boardModel" + N);
                    Json Anims = Json::array();
                    for (const std::string Key : { "boardAnim" + N, "tearAnim" + N })
                        if (auto A = Get(K, Key); !A.empty()) Anims.push_back(A);
                    if (!Model.empty() && !Anims.empty()) Boards.push_back({ { "board", I }, { "model", Model }, { "anims", Anims } });
                }
                if (!Boards.empty()) List.push_back({ { "name", Name }, { "boards", Boards } });
            }, Error);
            F["zbarriers"] = std::move(List);
        }

        // a scene bundle's objects: object<n>_Model, object<n>_Name and its object<n>_*Anim* keys
        void ReadScenes(const ZonekitZone& Z, Json& F, std::string& Error)
        {
            static const std::regex Key(R"(object(\d+)_(\w+))", std::regex::icase);
            Json List = Json::array();
            Z.ForEachGdtAsset("scriptbundle", [&](const Gdt::Type& Ty, const Gdt::Memory& M, int64_t Pos) {
                const std::string Name = Own(Ty, M, Pos);
                if (Name.empty()) return;
                for (const auto& B : Blocks(Ty, M, Pos, Name))
                {
                    struct Object
                    {
                        std::string Name, Model;
                        std::vector<std::string> Anims;
                    };
                    std::map<int, Object> Objects;
                    std::smatch Mt;
                    for (const auto& [K, V] : B.Keys)
                    {
                        if (V.empty() || !std::regex_match(K, Mt, Key)) continue;
                        Object& O = Objects[std::stoi(Mt[1].str())];
                        const std::string Field = Lower(Mt[2].str());
                        if (Field == "model") O.Model = Strip(V);
                        else if (Field == "name") O.Name = V;
                        else if (Field.find("anim") != std::string::npos && !IsNumber(V)) O.Anims.push_back(Strip(V));
                    }
                    Json Objs = Json::array();
                    for (const auto& [N, O] : Objects)
                        if (!O.Model.empty() && !O.Anims.empty())
                            Objs.push_back({ { "object", O.Name.empty() ? "object" + std::to_string(N) : O.Name }, { "model", O.Model }, { "anims", O.Anims } });
                    if (!Objs.empty()) List.push_back({ { "name", B.Name }, { "objects", Objs } });
                }
            }, Error);
            F["scenes"] = std::move(List);
        }

        // a weapon's gunModel and its *Anim* fields (its view anims)
        void ReadWeapons(const ZonekitZone& Z, Json& F, std::string& Error)
        {
            Json List = Json::array();
            Z.ForEachGdtAsset("weapon", [&](const Gdt::Type& Ty, const Gdt::Memory& M, int64_t Pos) {
                const std::string Name = Own(Ty, M, Pos);
                if (Name.empty()) return;
                const auto K = Keys(Ty, M, Pos);
                const std::string Gun = Get(K, "gunModel");
                if (Gun.empty()) return;
                Json Anims = Json::array();
                std::set<std::string> Seen;
                for (const auto& [Key, V] : K)
                    if (Key.find("Anim") != std::string::npos && Key != "playerAnimType" && !V.empty() && !IsNumber(V) && Seen.insert(Lower(V)).second)
                        Anims.push_back(V);
                if (!Anims.empty()) List.push_back({ { "name", Name }, { "model", Gun }, { "anims", Anims } });
            }, Error);
            F["weapons"] = std::move(List);
        }

        // the player body types of a customization table: their first body and helmet styles'
        // models, their playeranim_* poses
        void ReadPlayers(const ZonekitZone& Z, Json& F, std::string& Error)
        {
            Json List = Json::array();
            Z.ForEachGdtAsset("customizationtable", [&](const Gdt::Type& Ty, const Gdt::Memory& M, int64_t Pos) {
                const std::string Name = Own(Ty, M, Pos);
                if (Name.empty()) return;
                const auto All = Blocks(Ty, M, Pos, Name);
                std::map<std::string, std::string> Styles;   // body or helmet style -> its xmodel
                for (const auto& B : All)
                    if (B.Gdf == "playerbodystyle.gdf" || B.Gdf == "playerhelmetstyle.gdf")
                        for (const auto& [K, V] : B.Keys)
                            if (K == "xmodel" && !V.empty()) Styles[B.Name] = Strip(V);
                for (const auto& B : All)
                {
                    if (B.Gdf != "playerbodytype.gdf") continue;
                    std::map<std::string, std::string> K(B.Keys.begin(), B.Keys.end());
                    const auto First = [&](const char* Prefix) {
                        for (int I = 1; I < 100; I++)
                            if (const auto It = Styles.find(Get(K, Prefix + std::string(I < 10 ? "0" : "") + std::to_string(I))); It != Styles.end())
                                return It->second;
                        return std::string();
                    };
                    const std::string Body = First("bodyStyle"), Head = First("helmetStyle");
                    Json Anims = Json::array();
                    for (const auto& [Key, V] : B.Keys)
                        if (Key.rfind("playeranim_", 0) == 0 && !V.empty()) Anims.push_back(Strip(V));
                    if (!Body.empty() && !Anims.empty()) List.push_back({ { "name", B.Name }, { "body", Body }, { "head", Head }, { "anims", Anims } });
                }
            }, Error);
            F["players"] = std::move(List);
        }

        // ---- the facts of the zones an exported zone is loaded with

        struct Facts
        {
            Json J;
            std::string Zone;
        };
        using FactsPtr = std::shared_ptr<const Facts>;

        FactsPtr Parse(const std::string& Zone, const std::string& Text)
        {
            auto F = std::make_shared<Facts>();
            F->Zone = Zone;
            try
            {
                F->J = Json::parse(Text);
            }
            catch (const std::exception&)
            {
                return nullptr;
            }
            return F;
        }

        // the zones the game loads with Zone, the one overriding the others first
        std::vector<fs::path> SharedZones(const fs::path& Zone)
        {
            const std::string Stem = Lower(Zone.stem().string());
            const std::string Mode = Stem.substr(0, Stem.find('_'));
            std::vector<std::string> Names{ Stem + "_patch" };
            if (Mode == "zm" || Mode == "mp" || Mode == "cp")
            {
                Names.push_back(Mode + "_patch");
                Names.push_back(Mode + "_common");
            }
            Names.push_back("core_patch");
            Names.push_back("core_common");
            std::vector<fs::path> Out;
            std::set<std::string> Seen{ Stem };
            std::error_code Ec;
            for (const auto& N : Names)
                if (Seen.insert(N).second)
                    if (const fs::path P = Zone.parent_path() / (N + ".ff"); fs::exists(P, Ec)) Out.push_back(P);
            return Out;
        }

        // a shared zone's facts: kept in this process, then in a file beside the install index (its
        // size and time unchanged), else read now (the zone opened alone)
        FactsPtr SharedFacts(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& Options, const fs::path& Zone)
        {
            static std::mutex Lock;
            static std::map<std::wstring, FactsPtr> Cache;
            std::error_code Ec, Ec2;
            const uintmax_t Size = fs::file_size(Zone, Ec);
            const auto When = fs::last_write_time(Zone, Ec2);
            if (Ec || Ec2) return nullptr;
            const long long Time = (long long)When.time_since_epoch().count();
            const std::wstring Key = Zone.wstring() + L"|" + std::to_wstring(Size) + L"|" + std::to_wstring(Time);
            {
                std::lock_guard<std::mutex> G(Lock);
                if (const auto It = Cache.find(Key); It != Cache.end()) return It->second;
            }
            const std::string Stem = Lower(Zone.stem().string());
            const std::string File = DiskGame::KeptFile(Game, Options, Zone.parent_path().string(), (".owners-" + Stem + ".json").c_str());
            FactsPtr F;
            try
            {
                std::ifstream In(File, std::ios::binary);
                if (In)
                {
                    const Json K = Json::parse(In);
                    if (K.value("format", std::string()) == "graywolf-bo3-owner-facts" && K.value("version", 0) == 1 &&
                        K.value("size", (uintmax_t)0) == Size && K.value("time", 0ll) == Time)
                    {
                        auto P = std::make_shared<Facts>();
                        P->Zone = Stem;
                        P->J = K["facts"];
                        F = std::move(P);
                    }
                }
            }
            catch (const std::exception&)
            {
                F = nullptr;
            }
            if (!F)
            {
                std::string Error;
                std::vector<std::pair<std::string, std::string>> Docs;
                if (auto Z = Game.Open(Zone.string(), Options, Error)) Z->Documents(Docs);
                else return nullptr;
                Json Empty = Json::object();
                for (const auto& [Name, Text] : Docs)
                    if (Name.rfind(Folder, 0) == 0) F = Parse(Stem, Text);
                if (!F) F = Parse(Stem, Empty.dump());
                if (F)
                {
                    const Json K{ { "format", "graywolf-bo3-owner-facts" }, { "version", 1 }, { "size", Size }, { "time", Time }, { "facts", F->J } };
                    const std::string Temp = File + ".tmp";
                    {
                        std::ofstream Out(Temp, std::ios::binary | std::ios::trunc);
                        Out << K.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
                    }
                    fs::rename(Temp, File, Ec);
                }
            }
            std::lock_guard<std::mutex> G(Lock);
            Cache[Key] = F;
            return F;
        }

        // what the sources say, the first holding a name winning
        struct Sources
        {
            std::vector<FactsPtr> All;

            const Json* Find(const char* Section, const std::string& Name) const
            {
                const std::string L = Lower(Name);
                for (const auto& F : All)
                    if (const auto S = F->J.find(Section); S != F->J.end() && S->is_object())
                        for (const auto& [K, V] : S->items())
                            if (Lower(K) == L) return &V;
                return nullptr;
            }
            std::string FirstOfAlias(const std::string& Alias) const
            {
                const Json* A = Alias.empty() ? nullptr : Find("aliases", Alias);
                return A && A->is_array() && !A->empty() ? (*A)[0].get<std::string>() : std::string();
            }
            // an AI type's models: its first character with a body
            bool Models(const Json& Ai, AnimOwners::Owner& Out) const
            {
                for (const auto& C : Ai.value("characters", Json::array()))
                {
                    const Json* Ch = Find("characters", C.get<std::string>());
                    if (!Ch) continue;
                    const auto Part = [&](const char* Model, const char* Alias) {
                        const std::string M = Ch->value(Model, std::string());
                        return M.empty() ? FirstOfAlias(Ch->value(Alias, std::string())) : M;
                    };
                    const std::string Body = Part("body", "bodyAlias");
                    if (Body.empty()) continue;
                    Out.Models.clear();
                    for (const std::string& M : { Body, Part("head", "headAlias"), Part("hat", "hatAlias"), Part("gear", "gearAlias") })
                        if (!M.empty() && std::find(Out.Models.begin(), Out.Models.end(), M) == Out.Models.end()) Out.Models.push_back(M);
                    Out.Source = "aitype " + Ai.value("name", std::string()) + " (archetype " + Ai.value("archetype", std::string()) + ") -> character " +
                                 C.get<std::string>();
                    return true;
                }
                return false;
            }
        };

        std::vector<std::string> Strings(const Json& A)
        {
            std::vector<std::string> Out;
            if (A.is_array())
                for (const auto& S : A)
                    if (S.is_string()) Out.push_back(S.get<std::string>());
            return Out;
        }
    }

    std::vector<std::string> Documents()
    {
        return { std::string(Folder) + "*" };
    }

    std::pair<std::string, std::string> ZoneFacts(const ZonekitZone& Z)
    {
        Json F = Json::object();
        std::string Error;   // a type the profile lacks: that part is empty
        ReadAiTypes(Z, F, Error);
        ReadCharacters(Z, F, Error);
        ReadAliases(Z, F, Error);
        ReadAnimTables(Z, F, Error);
        ReadBarriers(Z, F, Error);
        ReadScenes(Z, F, Error);
        ReadWeapons(Z, F, Error);
        ReadPlayers(Z, F, Error);
        return { Folder + Z.Name() + ".json", F.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) };
    }

    void Fill(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& Options,
              const std::vector<DiskGame::IDiskGame::AnimZone>& Zones, AnimOwners::Book& Out)
    {
        for (const auto& Z : Zones)
        {
            Sources S;
            for (const auto& [Name, Text] : Z.Documents)
                if (Name.rfind(Folder, 0) == 0)
                    if (auto F = Parse(Name, Text)) S.All.push_back(std::move(F));
            for (const auto& P : SharedZones(fs::path(Z.Zone)))
                if (auto F = SharedFacts(Game, Options, P)) S.All.push_back(std::move(F));

            // each anim's owners, in order: the AI types (the exported zone's first), scenes,
            // barriers, weapons, player body types
            std::map<std::string, AnimOwners::Owner> Owners;
            std::map<std::string, std::vector<std::string>> Ids;   // anim, lower case -> owner ids
            const auto Give = [&](const std::string& Id, AnimOwners::Owner O, const std::vector<std::string>& Anims) {
                // tag_origin: a script model with no mesh of its own (the magic box's weapon slot)
                O.Models.erase(std::remove_if(O.Models.begin(), O.Models.end(), [](const std::string& M) { return M.empty() || Lower(M) == "tag_origin"; }),
                               O.Models.end());
                if (O.Models.empty() || !Owners.emplace(Id, std::move(O)).second) return;
                for (const auto& A : Anims)
                {
                    auto& V = Ids[Lower(A)];
                    if (std::find(V.begin(), V.end(), Id) == V.end()) V.push_back(Id);
                }
            };
            for (const auto& F : S.All)
                for (const auto& Ai : F->J.value("aitypes", Json::array()))
                {
                    AnimOwners::Owner O;
                    if (!S.Models(Ai, O)) continue;
                    std::vector<std::string> Anims;
                    for (const auto& T : Strings(Ai.value("am", Json::array())))
                    {
                        O.Source += ", " + T;
                        if (const Json* Am = S.Find("am", T)) for (auto& A : Strings(*Am)) Anims.push_back(std::move(A));
                        else O.Source += " (not found)";
                    }
                    Give("aitype:" + Ai.value("name", std::string()), std::move(O), Anims);
                }
            for (const auto& F : S.All)
                for (const auto& Sc : F->J.value("scenes", Json::array()))
                    for (const auto& Ob : Sc.value("objects", Json::array()))
                    {
                        const std::string Scene = Sc.value("name", std::string()), Obj = Ob.value("object", std::string());
                        Give("scene:" + Scene + "/" + Obj, { { Ob.value("model", std::string()) }, "scriptbundle " + Scene + " " + Obj },
                             Strings(Ob.value("anims", Json::array())));
                    }
            for (const auto& F : S.All)
                for (const auto& Zb : F->J.value("zbarriers", Json::array()))
                    for (const auto& B : Zb.value("boards", Json::array()))
                    {
                        const std::string Name = Zb.value("name", std::string()), N = std::to_string(B.value("board", 0));
                        Give("zbarrier:" + Name + "/" + N, { { B.value("model", std::string()) }, "zbarrier " + Name + " boardModel" + N },
                             Strings(B.value("anims", Json::array())));
                    }
            for (const auto& F : S.All)
                for (const auto& W : F->J.value("weapons", Json::array()))
                    Give("weapon:" + W.value("name", std::string()), { { W.value("model", std::string()) }, "weapon " + W.value("name", std::string()) + " gunModel" },
                         Strings(W.value("anims", Json::array())));
            for (const auto& F : S.All)
                for (const auto& P : F->J.value("players", Json::array()))
                {
                    AnimOwners::Owner O{ { P.value("body", std::string()) }, "customizationtable playerbodytype " + P.value("name", std::string()) };
                    if (const std::string Head = P.value("head", std::string()); !Head.empty()) O.Models.push_back(Head);
                    Give("character:" + P.value("name", std::string()), std::move(O), Strings(P.value("anims", Json::array())));
                }

            for (const auto& Anim : Z.Anims)
            {
                const auto It = Ids.find(Lower(Anim));
                if (It == Ids.end()) continue;
                for (const auto& Id : It->second)
                {
                    Out.Owners.emplace(Id, Owners[Id]);
                    Out.Own(Anim, Id);
                }
            }
        }
    }
}
