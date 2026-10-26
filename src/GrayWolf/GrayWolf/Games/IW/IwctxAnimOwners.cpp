#include "stdafx.h"

#include "IwctxAnimOwners.h"

#include "AnimOwners.h"
#include "ExportNaming.h"
#include "Games/IW/IwctxReaders.h"
#include "Strings.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>

#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace IwctxAnimOwners
{
    namespace
    {
        constexpr uint64_t Mask = 0x7FFFFFFFFFFFFFFFull;
        using ExportNaming::Lower;

        std::string Hex(uint64_t H) { return std::format("{:016x}", H); }
        uint64_t FromHex(const std::string& S) { return S.empty() ? 0 : std::stoull(S, nullptr, 16); }
        // a model as the model kind names it
        std::string ModelFile(const std::string& N) { return Strings::Replace(N, "::", "_"); }
        uint64_t NameHash(const std::string& N) { return IwctxReaders::AssetHash(N) & Mask; }
    }

    struct Zones::Index
    {
        std::vector<Served> Z;
        Layout L;
        std::vector<std::unordered_map<int64_t, size_t>> Top;              // per zone: position -> instance
        std::unordered_map<uint64_t, std::pair<size_t, size_t>> ByHash;    // a set, node, anim, model... by its hash
        std::vector<size_t> ByBase;                                         // zones by Base

        const std::string& TypeOf(size_t Zi, size_t Inst) const
        {
            static const std::string None;
            const auto& R = *Z[Zi].R;
            const uint16_t O = R.Instances[Inst].Owner;
            return O < R.OwnerNames.size() ? R.OwnerNames[O] : None;
        }
        uint64_t U64(size_t Zi, int64_t Pos) const
        {
            uint64_t V = 0;
            const auto& B = *Z[Zi].Bytes;
            if (Pos >= 0 && Pos + 8 <= (int64_t)B.size()) std::memcpy(&V, B.data() + Pos, 8);
            return V;
        }
        uint32_t U32(size_t Zi, int64_t Pos) const
        {
            uint32_t V = 0;
            const auto& B = *Z[Zi].Bytes;
            if (Pos >= 0 && Pos + 4 <= (int64_t)B.size()) std::memcpy(&V, B.data() + Pos, 4);
            return V;
        }
        // the zone and position of an address, false when no zone holds it
        bool Locate(uint64_t A, size_t& Zi, int64_t& Pos) const
        {
            auto It = std::upper_bound(ByBase.begin(), ByBase.end(), A, [this](uint64_t V, size_t K) { return V < Z[K].Base; });
            if (It == ByBase.begin()) return false;
            const size_t K = *--It;
            if (A - Z[K].Base >= Z[K].Bytes->size()) return false;
            Zi = K;
            Pos = (int64_t)(A - Z[K].Base);
            return true;
        }
        // the zone-level instance an 8-byte value names (a pointer to one, or its hash), not Self
        bool Names(uint64_t V, size_t Self, size_t SelfZone, size_t& Zi, size_t& Inst) const
        {
            int64_t Pos;
            if (Locate(V, Zi, Pos))
            {
                const auto It = Top[Zi].find(Pos);
                if (It == Top[Zi].end() || (Zi == SelfZone && It->second == Self)) return false;
                Inst = It->second;
                return true;
            }
            const auto It = ByHash.find(V & Mask);
            if (It == ByHash.end()) return false;
            Zi = It->second.first;
            Inst = It->second.second;
            return !(Zi == SelfZone && Inst == Self);
        }
        // each 8-byte value of an instance's reads (its children's included)
        template <class F>
        void Values(size_t Zi, size_t Inst, F&& Visit) const
        {
            const auto& R = *Z[Zi].R;
            const auto& I = R.Instances[Inst];
            const auto& B = *Z[Zi].Bytes;
            for (uint32_t K = I.FirstRead; K < I.EndRead && K < R.Reads.size() && K < R.ReadPos.size(); K++)
            {
                const int64_t Pos = R.ReadPos[K], Size = R.Reads[K];
                if (Pos < 0 || Size <= 0) continue;
                for (int64_t O = 0; O + 8 <= Size && Pos + O + 8 <= (int64_t)B.size(); O += 8)
                {
                    uint64_t V;
                    std::memcpy(&V, B.data() + Pos + O, 8);
                    if (V != 0 && V != ~0ull) Visit(V);
                }
            }
        }
        static bool Is(const std::string& T, const std::vector<std::string>& Types) { return std::find(Types.begin(), Types.end(), T) != Types.end(); }
        // a zone-level xmodel's name as the model kind writes it
        std::string ModelNameOf(size_t Zi, size_t Inst) const
        {
            const int64_t Pos = Z[Zi].R->Instances[Inst].Pos;
            size_t Sz;
            int64_t At;
            std::string N;
            if (Locate(U64(Zi, Pos + L.ModelName), Sz, At))
                for (const auto& B = *Z[Sz].Bytes; At < (int64_t)B.size() && B[(size_t)At] != 0 && N.size() < 512; At++) N.push_back((char)B[(size_t)At]);
            return N.empty() ? L.AssetName(L.Model.c_str(), U64(Zi, Pos) & Mask) : ModelFile(N);
        }
    };

    Zones::Zones(std::vector<Served> All, const Layout& L) : I(std::make_unique<Index>())
    {
        I->Z = std::move(All);
        I->L = L;
        std::vector<std::string> Hashed = L.Sets;
        Hashed.insert(Hashed.end(), L.Nodes.begin(), L.Nodes.end());
        for (const auto& T : { L.Anim, L.Character, L.Composite, L.Model }) Hashed.push_back(T);
        I->Top.resize(I->Z.size());
        for (size_t Zi = 0; Zi < I->Z.size(); Zi++)
        {
            const auto& R = *I->Z[Zi].R;
            for (size_t K = 0; K < R.Instances.size(); K++)
            {
                if (R.Instances[K].Parent >= 0) continue;
                I->Top[Zi].emplace(R.Instances[K].Pos, K);
                if (Index::Is(I->TypeOf(Zi, K), Hashed))
                    if (const uint64_t H = I->U64(Zi, R.Instances[K].Pos) & Mask; H != 0) I->ByHash.try_emplace(H, Zi, K);
            }
            I->ByBase.push_back(Zi);
        }
        std::sort(I->ByBase.begin(), I->ByBase.end(), [this](size_t A, size_t B) { return I->Z[A].Base < I->Z[B].Base; });
    }

    Zones::~Zones() = default;

    Json Zones::Facts() const
    {
        const Index& X = *I;
        Json Sets = Json::array(), Chars = Json::array();
        for (size_t Zi = 0; Zi < X.Z.size(); Zi++)
        {
            const auto& R = *X.Z[Zi].R;
            for (size_t K = 0; K < R.Instances.size(); K++)
            {
                if (R.Instances[K].Parent >= 0) continue;
                const std::string& T = X.TypeOf(Zi, K);
                const int64_t Pos = R.Instances[K].Pos;
                const uint64_t Id = X.U64(Zi, Pos) & Mask;
                if (Index::Is(T, X.L.Sets))
                {
                    // its xanims through the states and aliases it names, the sets it names (a scene
                    // set's base); a hash no zone here defines: an xanim of another zone, or a set
                    std::set<std::string> Anims;
                    std::set<uint64_t> Links;
                    std::unordered_set<uint64_t> Seen{ (uint64_t)Zi << 40 | K };
                    std::vector<std::pair<size_t, size_t>> Stack{ { Zi, K } };
                    while (!Stack.empty())
                    {
                        const auto [Sz, Si] = Stack.back();
                        Stack.pop_back();
                        X.Values(Sz, Si, [&, Sz = Sz, Si = Si](uint64_t V) {
                            size_t Nz, Ni;
                            if (X.Names(V, Si, Sz, Nz, Ni))
                            {
                                const std::string& Nt = X.TypeOf(Nz, Ni);
                                const uint64_t H = X.U64(Nz, X.Z[Nz].R->Instances[Ni].Pos) & Mask;
                                if (Nt == X.L.Anim) Anims.insert(X.L.AnimName(H));
                                else if (Index::Is(Nt, X.L.Sets)) Links.insert(H);
                                else if (Index::Is(Nt, X.L.Nodes) && Seen.size() < 100000 && Seen.insert((uint64_t)Nz << 40 | Ni).second) Stack.push_back({ Nz, Ni });
                                return;
                            }
                            size_t Lz;
                            int64_t Lp;
                            if (X.Locate(V, Lz, Lp) || ((V >> 63) == 0 && V < (1ull << 40)) || (V & Mask) == Id) return;
                            Links.insert(V & Mask);
                            Anims.insert(X.L.AnimName(V & Mask));
                        });
                    }
                    Json S;
                    S["id"] = Hex(Id);
                    S["name"] = X.L.AssetName(T.c_str(), Id);
                    S["zone"] = X.Z[Zi].Name;
                    S["anims"] = Anims;
                    Json Ls = Json::array();
                    for (const uint64_t H : Links) Ls.push_back(Hex(H));
                    S["links"] = std::move(Ls);
                    Sets.push_back(std::move(S));
                }
                else if (T == X.L.Character && X.L.CharBody >= 0)
                {
                    // a script string by its hash's low u32, else (Black Ops 6) by its slot in the zone's list
                    const auto Str = [&](int64_t At) {
                        if (At < 0) return std::string();
                        const uint32_t V = X.U32(Zi, Pos + At);
                        std::string S = IwctxReaders::ScriptString32(V);
                        if (S.empty() && V != 0 && V < (1u << 20))
                            if (S = IwctxReaders::SlotScriptString(V); S.rfind("string_", 0) == 0) S.clear();
                        return S;
                    };
                    Json C;
                    C["id"] = Hex(Id);
                    C["name"] = X.L.AssetName("character", Id);
                    C["zone"] = X.Z[Zi].Name;
                    C["body"] = ModelFile(Str(X.L.CharBody));
                    C["head"] = ModelFile(Str(X.L.CharHead));
                    C["archetype"] = Str(X.L.CharArchetype);
                    // the assets it names (its aianimsettings: named as the anim set it plays)
                    std::set<uint64_t> Links;
                    X.Values(Zi, K, [&](uint64_t V) {
                        size_t Nz, Ni, Lz;
                        int64_t Lp;
                        if (X.Names(V, K, Zi, Nz, Ni)) Links.insert(X.U64(Nz, X.Z[Nz].R->Instances[Ni].Pos) & Mask);
                        else if (!X.Locate(V, Lz, Lp) && ((V >> 63) != 0 || V >= (1ull << 40)) && (V & Mask) != Id) Links.insert(V & Mask);
                    });
                    Json Ls = Json::array();
                    for (const uint64_t H : Links) Ls.push_back(Hex(H));
                    C["links"] = std::move(Ls);
                    Chars.push_back(std::move(C));
                }
            }
        }
        Json Out;
        Json Zs = Json::array();
        for (const auto& S : X.Z) Zs.push_back(S.Name);
        Out["zones"] = std::move(Zs);
        Out["sets"] = std::move(Sets);
        Out["characters"] = std::move(Chars);
        Out["scenes"] = Json::array();
        Out["mapents"] = Json::array();
        Out["names"] = Json::object();
        return Out;
    }

    std::vector<std::string> Zones::CompositeModels(const std::string& Name) const
    {
        const Index& X = *I;
        std::vector<std::string> Out;
        const auto It = X.ByHash.find(NameHash(Name));
        if (It == X.ByHash.end() || X.TypeOf(It->second.first, It->second.second) != X.L.Composite) return Out;
        X.Values(It->second.first, It->second.second, [&](uint64_t V) {
            size_t Nz, Ni;
            if (X.Names(V, It->second.second, It->second.first, Nz, Ni) && X.TypeOf(Nz, Ni) == X.L.Model)
                if (std::string M = X.ModelNameOf(Nz, Ni); std::find(Out.begin(), Out.end(), M) == Out.end()) Out.push_back(std::move(M));
        });
        return Out;
    }

    void AddScenes(Json& Facts, const Json& Bundle, const std::string& BundleName,
                   const std::function<std::vector<std::string>(const std::string&)>& Composite)
    {
        struct Object
        {
            std::string Name, Ai, Subclass, AnimSet;
            std::vector<std::string> Models, Anims;
        };
        std::vector<Object> Objects;
        // a variant_object starts an object; its fields (and its shots') fill it
        // Depth: 0 for the object's own fields (its name), more in its shots
        std::function<void(const Json&, long long, int)> Walk = [&](const Json& F, long long Cur, int Depth) {
            if (F.is_array())
            {
                for (const auto& E : F) Walk(E, Cur, Depth);
                return;
            }
            if (!F.is_object()) return;
            const std::string Key = F.value("key", std::string());
            const auto V = F.find("value");
            if (V == F.end()) return;
            if (Key == "variant_object" && V->is_array())
            {
                Objects.emplace_back();
                Walk(*V, (long long)Objects.size() - 1, 0);
                return;
            }
            if (V->is_array())
            {
                Walk(*V, Cur, Depth + 1);
                return;
            }
            if (Cur < 0 || !V->is_string()) return;
            const std::string S = V->get<std::string>();
            if (S.empty()) return;
            Object& O = Objects[(size_t)Cur];
            const std::string Type = F.value("type", std::string());
            if (Key == "xmodelname" || Key == "10f740e41e697024" || Key == "model") O.Models.push_back(S);
            else if (Key == "aitypename") O.Ai = S;
            else if (Key == "aisubclass") O.Subclass = S;
            else if (Key == "a6e0cdc5ae95755e") O.AnimSet = S;   // the object's anim set
            else if (Key == "name" && Depth == 0) O.Name = S;
            else if (Type == "anim" || Key == "animation" || Key == "deltaanimation") O.Anims.push_back(S.substr(0, S.find('@')));
        };
        Walk(Bundle.contains("fields") ? Bundle["fields"] : Json::array(), -1, 0);
        for (size_t K = 0; K < Objects.size(); K++)
        {
            auto& O = Objects[K];
            // its name under a key no index names: its model's, else its rank
            if (O.Name.empty()) O.Name = O.Models.empty() ? std::format("object{}", K) : O.Models.front();
            if (O.Anims.empty() || (O.Models.empty() && O.Subclass.empty() && O.AnimSet.empty())) continue;
            std::vector<std::string> Models;
            for (const auto& M : O.Models)
            {
                std::vector<std::string> Parts = Lower(M).rfind("xcomposite_", 0) == 0 ? Composite(M) : std::vector<std::string>{ ModelFile(M) };
                for (auto& P : Parts)
                    if (std::find(Models.begin(), Models.end(), P) == Models.end()) Models.push_back(std::move(P));
            }
            Json S;
            S["bundle"] = BundleName;
            S["object"] = O.Name;
            S["models"] = Models;
            S["given"] = O.Models;
            S["aitype"] = O.Ai;
            S["subclass"] = O.Subclass;
            S["animset"] = O.AnimSet;
            S["anims"] = O.Anims;
            Facts["scenes"].push_back(std::move(S));
        }
    }

    void AddCharacterNames(Json& Facts, const Json& AiType)
    {
        std::function<void(const Json&)> Walk = [&](const Json& F) {
            if (F.is_array())
            {
                for (const auto& E : F) Walk(E);
                return;
            }
            if (!F.is_object() || !F.contains("value")) return;
            const auto& V = F["value"];
            if (V.is_array()) Walk(V);
            else if (V.is_string() && F.value("key", std::string()) == "character" && !V.get<std::string>().empty())
                Facts["names"][Hex(NameHash(V.get<std::string>()))] = V.get<std::string>();
        };
        if (AiType.contains("fields")) Walk(AiType["fields"]);
    }

    void AddMapEntities(Json& Facts, const std::vector<std::vector<std::pair<std::string, std::string>>>& Entities)
    {
        std::set<std::pair<std::string, std::string>> Done;
        for (const auto& E : Entities)
        {
            std::string Model, Anim;
            for (const auto& [K, V] : E)
                if (K == "model") Model = V;
                else if (K == "animation") Anim = V;
            if (Model.empty() || Anim.empty() || Model[0] == '*' || !Done.insert({ Model, Anim }).second) continue;
            Facts["mapents"].push_back({ { "model", ModelFile(Model) }, { "anim", Anim } });
        }
    }

    void ReadExported(const DiskGame::IDiskZone::Exporter& Export, DiskGame::Kind K, std::vector<std::pair<std::string, Json>>& Out)
    {
        // the trees handed over in memory (Context::OnTree): nothing written
        (void)K;
        DiskGame::Context C;
        C.OnTree = [&Out](const std::string& Stem, Json& Tree) { Out.emplace_back(Stem, std::move(Tree)); };
        DiskGame::Totals T;
        Export(C, T);
    }

    namespace
    {
        thread_local bool Shared = false;
    }
    bool ReadingShared() { return Shared; }

    std::vector<std::string> SharedZones(const std::string& Zone, const std::string& Install, const char* Folder,
                                         const std::vector<std::string>& Zm, const std::vector<std::string>& Mp)
    {
        const fs::path Dir = fs::path(Zone).parent_path();
        const std::string Stem = Lower(fs::path(Zone).stem().string());
        const bool IsZm = Stem.rfind("zm_", 0) == 0 || Stem.find("_zm_") != std::string::npos || (Stem.size() > 3 && Stem.compare(Stem.size() - 3, 3, "_zm") == 0);
        const std::vector<std::string>& Names = IsZm ? Zm : Mp;
        std::vector<std::string> Out;
        std::error_code Ec;
        for (const auto& N : Names)
            for (const fs::path& From : { Dir, fs::path(Install), Install.empty() ? fs::path() : fs::path(Install) / Folder })
                if (N != Stem && !From.empty() && fs::exists(From / (N + ".ff"), Ec))
                {
                    Out.push_back((From / (N + ".ff")).string());
                    break;
                }
        return Out;
    }

    namespace
    {
        const Json* DocumentOf(const std::vector<std::pair<std::string, std::string>>& Docs, Json& Keep)
        {
            for (const auto& [N, Text] : Docs)
                if (N == DocumentName)
                {
                    Keep = Json::parse(Text, nullptr, false);
                    if (!Keep.is_discarded()) return &Keep;
                }
            return nullptr;
        }

        // where a shared zone's facts are kept between runs (opening one takes 20-30 s): %TEMP% or
        // <--cache>, keyed by the zone file (path, size, time), the reader's tables (Key: the asking
        // zone's IndexKey) and this executable (its time: the code that read them)
        fs::path KeptFacts(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& Options, const fs::path& Zone, uintmax_t Size,
                           fs::file_time_type When, const std::string& Key)
        {
            std::error_code Ec;
            wchar_t Exe[MAX_PATH * 4] = {};
            GetModuleFileNameW(nullptr, Exe, (DWORD)std::size(Exe));
            const auto Built = fs::last_write_time(fs::path(Exe), Ec).time_since_epoch().count();
            const uint64_t H = ExportNaming::Fnv1a(std::format("{}|{}|{}|{}|{}|facts1", Strings::ToNormalString(Zone.wstring()), Size,
                                                               (long long)When.time_since_epoch().count(), Key, (long long)Built));
            const fs::path Root = !Options.Cache.empty() ? fs::path(Options.Cache) / "anim_owner_facts" : fs::temp_directory_path(Ec) / "greyhound_anim_owner_facts";
            return Root / Game.Id() / std::format("{}_{:016x}.json", ExportNaming::SafeFileName(Zone.stem().string()), H);
        }

        // the facts of a zone loaded with others, read once per file (its size and time unchanged),
        // kept on disk between runs (KeptFacts)
        std::shared_ptr<const Json> SharedFacts(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& Options, const fs::path& Zone,
                                                const std::string& ReaderKey)
        {
            struct Kept
            {
                uintmax_t Size = 0;
                fs::file_time_type When;
                std::shared_ptr<const Json> F;
            };
            static std::mutex Lock;
            static std::map<std::wstring, Kept> Cache;
            std::error_code Ec, Ec2;
            const uintmax_t Size = fs::file_size(Zone, Ec);
            const auto When = fs::last_write_time(Zone, Ec2);
            if (Ec || Ec2) return nullptr;
            const std::wstring Key = Zone.wstring();
            {
                std::lock_guard<std::mutex> G(Lock);
                if (const auto It = Cache.find(Key); It != Cache.end() && It->second.Size == Size && It->second.When == When) return It->second.F;
            }
            const fs::path File = KeptFacts(Game, Options, Zone, Size, When, ReaderKey);
            {
                std::ifstream In(File, std::ios::binary);
                if (In)
                {
                    Json J = Json::parse(In, nullptr, false);
                    if (!J.is_discarded())
                    {
                        auto F = J.is_null() ? nullptr : std::make_shared<const Json>(std::move(J));
                        std::lock_guard<std::mutex> G(Lock);
                        Cache[Key] = { Size, When, F };
                        return F;
                    }
                }
            }
            std::string Error;
            std::vector<std::pair<std::string, std::string>> Docs;
            DiskGame::ExportOptions Alone = Options;
            Alone.ZoneAlone = true;
            Shared = true;
            if (auto Z = Game.Open(Zone.string(), Alone, Error)) Z->Documents(Docs);
            Shared = false;
            Json J;
            auto F = DocumentOf(Docs, J) ? std::make_shared<const Json>(std::move(J)) : nullptr;
            {
                // written whole, then moved over (two runs side by side)
                fs::create_directories(File.parent_path(), Ec);
                const fs::path Temp = File.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
                const std::string Text = F ? F->dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) : std::string("null");
                bool Ok;
                {
                    std::ofstream O(Temp, std::ios::binary);
                    O.write(Text.data(), (std::streamsize)Text.size());
                    Ok = (bool)O;
                }
                if (!Ok || !MoveFileExW(Temp.c_str(), File.c_str(), MOVEFILE_REPLACE_EXISTING)) fs::remove(Temp, Ec);
            }
            std::lock_guard<std::mutex> G(Lock);
            Cache[Key] = { Size, When, F };
            return F;
        }

        struct Character
        {
            std::string Id, Name, Zone, Body, Head, Archetype;
            std::vector<uint64_t> Links;
        };
    }

    void Fill(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& Options, const std::vector<DiskGame::IDiskGame::AnimZone>& Zones,
              AnimOwners::Book& Out, const std::function<std::vector<std::string>(const std::string& Zone, const Json& Facts)>& Shared)
    {
        for (const auto& Z : Zones)
        {
            Json Own;
            const Json* Mine = DocumentOf(Z.Documents, Own);
            if (Mine == nullptr) continue;
            std::vector<std::shared_ptr<const Json>> Kept;
            std::vector<const Json*> Sources{ Mine };
            for (const auto& P : Shared(Z.Zone, *Mine))
                if (auto F = SharedFacts(Game, Options, fs::path(P), Mine->value("reader_key", std::string())))
                {
                    Sources.push_back(F.get());
                    Kept.push_back(std::move(F));
                }
            // the run's anims by their lower-case name
            std::unordered_map<std::string, std::string> Wanted;
            for (const auto& A : Z.Anims) Wanted.emplace(Lower(A), A);
            // the characters (the first of an id), named by an aitype when the index does not, by archetype
            std::map<std::string, std::string> Names;
            for (const Json* S : Sources)
                if (S->contains("names"))
                    for (const auto& [H, N] : (*S)["names"].items()) Names.try_emplace(H, N.get<std::string>());
            // a character's links that are anim sets' (its aianimsettings, named as its set)
            std::unordered_set<uint64_t> AllSets;
            for (const Json* S : Sources)
                for (const auto& Js : S->value("sets", Json::array())) AllSets.insert(FromHex(Js.value("id", "")));
            std::vector<Character> Chars;
            std::set<std::string> CharIds;
            std::unordered_map<uint64_t, std::vector<size_t>> ByArchetype;
            for (const Json* S : Sources)
                for (const auto& C : S->value("characters", Json::array()))
                {
                    Character Ch{ C.value("id", ""), C.value("name", ""), C.value("zone", ""), C.value("body", ""), C.value("head", ""), C.value("archetype", "") };
                    if (Ch.Body.empty() || Ch.Archetype.empty() || !CharIds.insert(Ch.Id).second) continue;
                    if (const auto It = Names.find(Ch.Id); It != Names.end()) Ch.Name = It->second;
                    std::set<uint64_t> Keys{ NameHash(Ch.Archetype) };
                    for (const auto& L : C.value("links", Json::array()))
                        if (const uint64_t H = FromHex(L.get<std::string>()); AllSets.count(H)) Keys.insert(H);
                    for (const uint64_t H : Keys) ByArchetype[H].push_back(Chars.size());
                    Chars.push_back(std::move(Ch));
                }
            // an anim set's name: an archetype's text, else its index name
            std::unordered_map<uint64_t, std::string> SetNames;
            for (const Json* S : Sources)
                for (const auto& Js : S->value("sets", Json::array())) SetNames.try_emplace(FromHex(Js.value("id", "")), Js.value("name", ""));
            for (const auto& C : Chars) SetNames[NameHash(C.Archetype)] = C.Archetype;
            const auto SetName = [&](uint64_t H) {
                const auto It = SetNames.find(H);
                return It != SetNames.end() ? It->second : Hex(H);
            };
            // per anim: its owners with their rank, given in rank order
            std::unordered_map<std::string, std::vector<std::pair<std::pair<int, size_t>, std::string>>> Ranked;
            const auto Give = [&](const std::string& Anim, std::pair<int, size_t> Rank, const std::string& Id) {
                if (const auto It = Wanted.find(Lower(Anim)); It != Wanted.end()) Ranked[It->second].push_back({ Rank, Id });
            };
            const auto CharacterOwner = [&](const Character& C, const std::string& Archetype, const std::string& Why) {
                const std::string Id = "archetype:" + Archetype + "/" + (C.Name.empty() ? C.Id : C.Name);
                if (!Out.Owners.count(Id))
                {
                    AnimOwners::Owner O;
                    O.Models.push_back(C.Body);
                    if (!C.Head.empty() && C.Head != C.Body) O.Models.push_back(C.Head);
                    O.Source = "character " + (C.Name.empty() ? C.Id : C.Name) + " (" + C.Zone + "): archetype " + C.Archetype + Why;
                    Out.Owners[Id] = std::move(O);
                }
                return Id;
            };
            // scenes and map entities: the models they play an anim on (an AI: its archetype's characters)
            size_t Seq = 0;
            for (const Json* S : Sources)
            {
                for (const auto& Sc : S->value("scenes", Json::array()))
                {
                    const std::string Obj = Sc.value("object", ""), Bundle = Sc.value("bundle", "");
                    const Json Models = Sc.value("models", Json::array());
                    if (!Models.empty())
                    {
                        const std::string Id = "scene:" + Bundle + "/" + Obj;
                        AnimOwners::Owner O;
                        for (const auto& M : Models) O.Models.push_back(M.get<std::string>());
                        O.Source = "scriptbundle " + Bundle + " object " + Obj + " (" + Sc.value("given", Json::array()).dump() + ")";
                        Out.Owners.try_emplace(Id, std::move(O));
                        for (const auto& A : Sc.value("anims", Json::array())) Give(A.get<std::string>(), { 0, Seq }, Id);
                        Seq++;
                        continue;
                    }
                    for (const std::string Arch : { Sc.value("subclass", ""), Sc.value("animset", "") })
                        if (const auto It = ByArchetype.find(NameHash(Arch)); !Arch.empty() && It != ByArchetype.end())
                            for (const size_t C : It->second)
                            {
                                const std::string Id = CharacterOwner(Chars[C], Arch, "; scriptbundle " + Bundle + " object " + Obj);
                                for (const auto& A : Sc.value("anims", Json::array())) Give(A.get<std::string>(), { 2, Seq }, Id);
                                Seq++;
                            }
                }
                for (const auto& E : S->value("mapents", Json::array()))
                {
                    const std::string Model = E.value("model", ""), Id = "mapent:" + Model;
                    Out.Owners.try_emplace(Id, AnimOwners::Owner{ { Model }, "map entity model " + Model });
                    Give(E.value("anim", ""), { 0, Seq++ }, Id);
                }
            }
            // the sets: the characters of their archetype, then of the sets they name; the smallest set first
            std::set<std::string> SetIds;
            for (const Json* S : Sources)
                for (const auto& Js : S->value("sets", Json::array()))
                {
                    if (!SetIds.insert(Js.value("id", "")).second) continue;
                    const Json& Anims = Js["anims"];
                    std::vector<std::string> Here;
                    for (const auto& A : Anims)
                        if (Wanted.count(Lower(A.get<std::string>()))) Here.push_back(A.get<std::string>());
                    if (Here.empty()) continue;
                    const std::string Set = "; set " + Js.value("name", "") + " (" + Js.value("zone", "") + ")";
                    std::vector<std::tuple<size_t, std::string, std::string>> Who;   // character, archetype, why
                    const uint64_t SetId = FromHex(Js.value("id", ""));
                    if (const auto It = ByArchetype.find(SetId); It != ByArchetype.end())
                        for (const size_t C : It->second) Who.push_back({ C, SetName(SetId), Set });
                    for (const auto& L : Js["links"])
                        if (const auto It = ByArchetype.find(FromHex(L.get<std::string>())); It != ByArchetype.end())
                            for (const size_t C : It->second) Who.push_back({ C, SetName(FromHex(L.get<std::string>())), Set + " naming " + SetName(FromHex(L.get<std::string>())) });
                    size_t K = 0;
                    for (const auto& [C, Arch, Why] : Who)
                    {
                        const std::string Id = CharacterOwner(Chars[C], Arch, Why);
                        for (const auto& A : Here) Give(A, { 1, Anims.size() * 1000 + K }, Id);
                        K++;
                    }
                }
            for (auto& [Anim, List] : Ranked)
            {
                std::stable_sort(List.begin(), List.end(), [](const auto& A, const auto& B) { return A.first < B.first; });
                for (const auto& [Rank, Id] : List) Out.Own(Anim, Id);
            }
        }
    }
}
