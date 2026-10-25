#include "stdafx.h"

#include "AnimRigs.h"

#include "CastReader.h"       // CastFile::Parse, ReadAnimClip
#include "SeAnimReader.h"     // SeAnimClip
#include "DiskGame.h"         // ZoneIndex, ResolveZones: where a reference model is
#include "GameInstalls.h"     // the installs a reference model is exported from
#include "AppLog.h"
#include "Strings.h"
#include "WraithX.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>

#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace AnimRigs
{
    namespace
    {
        std::string Lower(std::string S)
        {
            for (auto& c : S) c = (char)tolower((unsigned char)c);
            return S;
        }

        // "name_LOD2" -> ("name", 2); a name with no LOD suffix is LOD 0
        std::string WithoutLod(const std::string& Stem, int* Lod = nullptr)
        {
            const auto P = Stem.rfind("_LOD");
            if (P != std::string::npos && P + 4 < Stem.size() && Stem.find_first_not_of("0123456789", P + 4) == std::string::npos)
            {
                if (Lod) *Lod = std::atoi(Stem.c_str() + P + 4);
                return Stem.substr(0, P);
            }
            if (Lod) *Lod = 0;
            return Stem;
        }

        enum class Role { Other, View, Body };

        bool Has(const std::string& S, const char* Part) { return S.find(Part) != std::string::npos; }
        bool Starts(const std::string& S, const char* Part) { return S.rfind(Part, 0) == 0; }
        bool Ends(const std::string& S, const char* Part)
        {
            const size_t N = std::strlen(Part);
            return S.size() >= N && S.compare(S.size() - N, N, Part) == 0;
        }

        Role AnimRole(const std::string& Name)
        {
            const std::string N = Lower(Name);
            if (Starts(N, "vm_") || Has(N, "viewmodel") || Has(N, "_vm_") || Ends(N, "_vm")) return Role::View;
            if (Starts(N, "pb_") || Starts(N, "mp_") || Starts(N, "pt_") || Starts(N, "ai_") || Starts(N, "sdr_") || Has(N, "body")) return Role::Body;
            return Role::Other;
        }

        Role ModelRole(const std::string& Name)
        {
            const std::string N = Lower(Name);
            if (Has(N, "viewhands") || Has(N, "viewarms") || Has(N, "view_arms") || Has(N, "view_hands") || Has(N, "vm_arms") ||
                Has(N, "viewmodel") || Has(N, "_vm_") || Ends(N, "_vm") || Has(N, "_view"))
                return Role::View;
            if (Has(N, "body") || Has(N, "_fb")) return Role::Body;
            return Role::Other;
        }

        // A name's words worth comparing: 3 letters or more, not an anim's state ("idle", "fire")
        std::set<std::string> Words(const std::string& Name)
        {
            static const std::set<std::string> Common = {
                "viewmodel", "view", "anim", "idle", "fire", "reload", "empty", "sprint", "loop", "ads", "raise", "first",
                "pullout", "putaway", "lod0", "lod1", "lod2", "lod3", "mod", "body", "viewhands", "world", "stow", "out",
                "the", "and", "left", "right", "fast", "slow", "walk", "run", "stand", "crouch", "prone",
            };
            std::set<std::string> Out;
            std::stringstream Ss(Lower(Name));
            std::string W;
            while (std::getline(Ss, W, '_'))
                if (W.size() >= 3 && !Common.count(W)) Out.insert(W);
            return Out;
        }

        fs::path RigsRoot()
        {
            std::error_code Ec;
            const fs::path Temp = fs::temp_directory_path(Ec);
            return Ec ? fs::path() : Temp / "greyhound_rigs";
        }

        fs::path ExeDir()
        {
            wchar_t Buf[MAX_PATH * 4] = {};
            GetModuleFileNameW(nullptr, Buf, (DWORD)std::size(Buf));
            return fs::path(Buf).parent_path();
        }

        // preview_rigs.json beside the exe (the build copies it), else in a checkout above it
        fs::path TableFile()
        {
            std::error_code Ec;
            for (fs::path P = ExeDir(); !P.empty() && P != P.parent_path(); P = P.parent_path())
            {
                const fs::path F = P / "zonekit" / "profiles" / "preview_rigs.json";
                if (fs::exists(F, Ec)) return F;
            }
            return {};
        }

        bool ReadAll(const std::string& Path, std::vector<uint8_t>& Out)
        {
            std::ifstream In(fs::path(Strings::ToUnicodeString(Path)), std::ios::binary | std::ios::ate);
            if (!In) return false;
            const std::streamsize Size = In.tellg();
            if (Size < 0) return false;
            In.seekg(0);
            Out.resize((size_t)Size);
            return Size == 0 || (bool)In.read((char*)Out.data(), Size);
        }

        // Count null-terminated names from Bytes at Offset; false when the bytes end first
        bool CStrings(const std::vector<uint8_t>& Bytes, size_t Offset, uint32_t Count, std::vector<std::string>& Out)
        {
            Out.clear();
            for (uint32_t i = 0; i < Count; i++)
            {
                size_t E = Offset;
                while (E < Bytes.size() && Bytes[E] != 0) E++;
                if (E >= Bytes.size()) return false;
                Out.emplace_back((const char*)Bytes.data() + Offset, E - Offset);
                Offset = E + 1;
            }
            return true;
        }

        // A binary header's name list (SEModel, SEAnim): the u32 count at CountAt, the names from
        // NamesFrom; only as much of the file as they take is read. FlagBit unset at FlagAt: no names.
        bool ReadNames(const std::string& Path, size_t HeaderSize, size_t CountAt, size_t NamesFrom, size_t FlagAt, uint8_t FlagBit,
                       const char* Magic, std::vector<std::string>& Out)
        {
            std::ifstream In(fs::path(Strings::ToUnicodeString(Path)), std::ios::binary);
            if (!In) return false;
            std::vector<uint8_t> Bytes(HeaderSize);
            if (!In.read((char*)Bytes.data(), (std::streamsize)HeaderSize) || std::memcmp(Bytes.data(), Magic, std::strlen(Magic)) != 0) return false;
            if (FlagBit && !(Bytes[FlagAt] & FlagBit)) { Out.clear(); return true; }
            uint32_t Count = 0;
            std::memcpy(&Count, Bytes.data() + CountAt, 4);
            if (Count > 100000) return false;
            for (size_t Want = NamesFrom + (size_t)Count * 48 + 64;; Want *= 4)
            {
                const size_t Had = Bytes.size();
                Bytes.resize(Want);
                In.read((char*)Bytes.data() + Had, (std::streamsize)(Want - Had));
                Bytes.resize(Had + (size_t)In.gcount());
                if (CStrings(Bytes, NamesFrom, Count, Out)) return true;
                if (!In) return false;
            }
        }

        const CastFile::Node* FindNode(const std::vector<CastFile::Node>& Nodes, uint32_t Id)
        {
            for (const auto& N : Nodes)
            {
                if (N.Id == Id) return &N;
                if (const CastFile::Node* F = FindNode(N.Children, Id)) return F;
            }
            return nullptr;
        }
    }

    bool ModelBoneNames(const std::string& Path, std::vector<std::string>& Out)
    {
        Out.clear();
        const std::string Ext = Lower(fs::path(Path).extension().string());
        if (Ext == ".semodel")   // "SEModel" ver hdrSize dataFlags@11 boneFlags meshFlags boneCount@14 ... names@29
            return ReadNames(Path, 29, 14, 29, 11, 1, "SEModel", Out);
        if (Ext == ".xmodel_export")
        {
            std::ifstream In(fs::path(Strings::ToUnicodeString(Path)));
            std::string Line;
            int Count = -1;
            while (std::getline(In, Line))
            {
                if (Count < 0 && Line.rfind("NUMBONES ", 0) == 0)
                {
                    Count = std::atoi(Line.c_str() + 9);
                    if (Count == 0) return true;
                }
                else if (Count > 0 && Line.rfind("BONE ", 0) == 0)
                {
                    const auto A = Line.find('"'), B = Line.rfind('"');
                    if (A != std::string::npos && B > A) Out.push_back(Line.substr(A + 1, B - A - 1));
                    if ((int)Out.size() == Count) return true;
                }
            }
            return Count >= 0;
        }
        if (Ext == ".cast")
        {
            std::vector<uint8_t> Bytes;
            std::vector<CastFile::Node> Roots;
            if (!ReadAll(Path, Bytes) || !CastFile::Parse(Bytes, Roots)) return false;
            const CastFile::Node* Model = FindNode(Roots, CastFile::IdModel);
            if (!Model) return false;
            for (const auto& C : Model->Children)
                if (C.Id == CastFile::IdSkeleton)
                    for (const auto& B : C.Children)
                        if (B.Id == CastFile::IdBone) Out.push_back(B.String("n"));
            return true;
        }
        return false;
    }

    bool AnimBoneNames(const std::string& Path, std::vector<std::string>& Out)
    {
        Out.clear();
        const std::string Ext = Lower(fs::path(Path).extension().string());
        if (Ext == ".seanim")   // "SEAnim" ver hdrSize type flags dataFlags dataProp reserved rate frames boneCount@24 ... names@36
            return ReadNames(Path, 36, 24, 36, 0, 0, "SEAnim", Out);
        SeAnimClip Clip;
        if (!ReadAnimClip(Path, Clip)) return false;
        for (const auto& B : Clip.Bones) Out.push_back(B.Name);
        return true;
    }

    std::string GameOf(const std::string& Path)
    {
        static const std::set<std::string> Kinds = [] {
            std::set<std::string> K;
            for (const auto& Info : DiskGame::Kinds) K.insert(Info.Folder);
            K.insert("xanims");
            K.insert("xmodels");
            return K;
        }();
        std::vector<std::string> Parts;
        for (const auto& P : fs::path(Path).parent_path()) Parts.push_back(Lower(P.string()));
        for (size_t i = Parts.size(); i-- > 1;)
            if (Kinds.count(Parts[i])) return Parts[i - 1];
        return {};
    }

    struct Index::Impl
    {
        // a model as the picks see it
        struct Model
        {
            std::string Path, Name, Game, Base;   // Base: its name without _LODn
            Role R = Role::Other;
            std::set<std::string> Words;
            std::vector<int> Bones;               // sorted ids
            bool Reference = false;
            bool RefBody = false, RefHead = false;   // a game's reference body, head (the table's lists)
        };
        struct Result
        {
            Pick P;
            std::vector<Candidate> Ranked;
            uint64_t Key = 0;
        };
        // a game's reference models
        struct Refs
        {
            enum class State { None, Queued, Done } S = State::None;
            std::vector<ModelRef> Models;
            std::string Note;
        };

        mutable std::mutex M;
        std::condition_variable Cv;
        // under M
        std::vector<ModelRef> Listed;
        uint64_t ListedVersion = 0;
        bool HaveModels = false;
        uint64_t RefVersion = 0;
        std::deque<std::string> Queue;
        std::unordered_set<std::string> Queued;
        std::unordered_map<std::string, Result> Results;
        std::unordered_map<std::string, std::string> Manual;
        std::map<std::string, Refs> GameRefs;
        std::deque<std::string> RefQueue;
        bool Fetch = true, Stop = false, Busy = false, RefBusy = false;
        std::atomic<bool> Stopping{ false };   // Stop, read without M: an export under way is ended
        std::atomic<uint64_t> Gen{ 1 };
        size_t PoolSize = 0;
        // the table, read once (under M): per game its lists of names (hands, body, head), the first found of each used
        using Lists = std::vector<std::vector<std::string>>;
        std::map<std::string, Lists> Table;
        std::map<std::string, std::set<std::string>> Bodies, Heads;
        std::set<std::string> FromInstall;   // games whose references come from the install folder whole ("from": "install")   // per game, the table's body and head names (lower case)
        bool TableRead = false;

        // the worker's own
        std::unordered_map<std::string, int> BoneIds;
        std::unordered_map<std::string, std::vector<int>> BonesOf;   // by path
        std::set<std::string> FoldersLoaded;
        std::map<std::string, std::vector<std::string>> FolderDirty;  // folder -> paths read since its cache was written
        std::vector<Model> Pool;
        uint64_t PoolKey = ~0ull;
        // the bones some model of the pool has, per game and of any game (by id): what a score counts
        std::map<std::string, std::vector<uint8_t>> Known;
        std::vector<uint8_t> KnownAll;

        std::thread Worker, RefWorker;

        uint64_t KeyNow() const { return (ListedVersion << 20) ^ RefVersion; }

        int BoneId(const std::string& Name)
        {
            const auto [It, New] = BoneIds.emplace(Lower(Name), (int)BoneIds.size());
            return It->second;
        }

        // the bone-name cache a model's names are kept in: its game folder's (above models\), else its folder's
        static std::string CacheFolder(const std::string& Path)
        {
            std::vector<fs::path> Up;
            for (fs::path D = fs::path(Path).parent_path(); !D.empty() && D != D.parent_path(); D = D.parent_path()) Up.push_back(D);
            for (size_t i = 0; i + 1 < Up.size(); i++)
                if (Lower(Up[i].filename().string()) == "models") return Up[i + 1].string();
            return fs::path(Path).parent_path().string();
        }
        static fs::path CacheFile(const std::string& Folder)
        {
            uint64_t H = 1469598103934665603ull;
            for (const char c : Lower(Folder)) { H ^= (uint8_t)c; H *= 1099511628211ull; }
            char Name[40];
            snprintf(Name, sizeof(Name), "%016llx.tsv", (unsigned long long)H);
            return RigsRoot() / "bones" / Name;
        }
        static std::string Stamp(const std::string& Path)
        {
            std::error_code Ec, Ec2;
            const fs::path P(Strings::ToUnicodeString(Path));
            const auto Size = fs::file_size(P, Ec);
            const auto When = fs::last_write_time(P, Ec2).time_since_epoch().count();
            return std::to_string(Ec ? 0 : Size) + ":" + std::to_string(Ec2 ? 0 : When);
        }

        void LoadFolderCache(const std::string& Folder)
        {
            if (!FoldersLoaded.insert(Folder).second) return;
            std::ifstream In(CacheFile(Folder));
            std::string Line;
            while (std::getline(In, Line))
            {
                std::stringstream Ss(Line);
                std::string Path, St, Names;
                if (!std::getline(Ss, Path, '\t') || !std::getline(Ss, St, '\t')) continue;
                std::getline(Ss, Names);
                if (Stamp(Path) != St) continue;
                std::vector<int> Ids;
                std::stringstream Ns(Names);
                std::string B;
                while (std::getline(Ns, B, ',')) if (!B.empty()) Ids.push_back(BoneId(B));
                std::sort(Ids.begin(), Ids.end());
                Ids.erase(std::unique(Ids.begin(), Ids.end()), Ids.end());
                BonesOf[Path] = std::move(Ids);
            }
        }

        void SaveFolderCaches()
        {
            std::vector<std::string> Names(BoneIds.size());
            for (const auto& [N, Id] : BoneIds) Names[Id] = N;
            for (auto& [Folder, Paths] : FolderDirty)
            {
                if (Paths.empty()) continue;
                std::error_code Ec;
                const fs::path F = CacheFile(Folder);
                fs::create_directories(F.parent_path(), Ec);
                std::ofstream Out(F, std::ios::app);
                for (const auto& Path : Paths)
                {
                    Out << Path << '\t' << Stamp(Path) << '\t';
                    const auto It = BonesOf.find(Path);
                    if (It != BonesOf.end())
                        for (size_t i = 0; i < It->second.size(); i++) Out << (i ? "," : "") << Names[It->second[i]];
                    Out << '\n';
                }
                Paths.clear();
            }
        }

        const std::vector<int>& BonesFor(const std::string& Path)
        {
            if (const auto It = BonesOf.find(Path); It != BonesOf.end()) return It->second;
            const std::string Folder = CacheFolder(Path);
            LoadFolderCache(Folder);
            if (const auto It = BonesOf.find(Path); It != BonesOf.end()) return It->second;
            std::vector<std::string> Names;
            std::vector<int> Ids;
            if (ModelBoneNames(Path, Names))
                for (const auto& N : Names) Ids.push_back(BoneId(N));
            std::sort(Ids.begin(), Ids.end());
            Ids.erase(std::unique(Ids.begin(), Ids.end()), Ids.end());
            FolderDirty[Folder].push_back(Path);
            return BonesOf[Path] = std::move(Ids);
        }

        // the models picks choose from: one per name and folder (its lowest LOD number), the games' references
        void BuildPool(const std::vector<ModelRef>& Models, const std::map<std::string, Refs>& References)
        {
            std::map<std::string, std::pair<int, const ModelRef*>> ByName;   // folder|base -> (lod, model)
            for (const auto& L : Models)
            {
                int Lod = 0;
                const std::string Base = WithoutLod(L.Name, &Lod);
                const std::string Key = Lower(fs::path(L.Path).parent_path().string() + "|" + Base);
                const auto It = ByName.find(Key);
                if (It == ByName.end() || Lod < It->second.first) ByName[Key] = { Lod, &L };
            }
            Pool.clear();
            const auto Add = [&](const ModelRef& L, bool Reference) {
                Model Md;
                Md.Path = L.Path; Md.Name = L.Name; Md.Base = WithoutLod(L.Name); Md.Game = GameOf(L.Path);
                Md.R = ModelRole(Md.Base); Md.Words = Words(Md.Base); Md.Reference = Reference;
                Md.RefBody = Reference && Bodies[Md.Game].count(Lower(Md.Base));
                Md.RefHead = Reference && Heads[Md.Game].count(Lower(Md.Base));
                Md.Bones = BonesFor(L.Path);
                if (!Md.Bones.empty()) Pool.push_back(std::move(Md));
            };
            for (const auto& [Key, V] : ByName) Add(*V.second, false);
            for (const auto& [Game, R] : References)
                for (const auto& L : R.Models) Add(L, true);
            Known.clear();
            KnownAll.assign(BoneIds.size(), 0);
            for (const Model& Md : Pool)
            {
                auto& K = Known[Md.Game];
                K.resize(BoneIds.size(), 0);
                for (const int B : Md.Bones) K[B] = KnownAll[B] = 1;
            }
            SaveFolderCaches();
        }

        struct Scored
        {
            const Model* M = nullptr;
            int Hits = 0, Extra = 0, Words = 0;
            bool SameGame = false, RoleFits = false;
            float Score = 0;
        };

        // Better first: the score (in steps of 2%), then the game, the kind, the words, fewer bones left still
        static bool Before(const Scored& A, const Scored& B)
        {
            const int Sa = (int)(A.Score * 50.0f + 1e-4f), Sb = (int)(B.Score * 50.0f + 1e-4f);
            if (Sa != Sb) return Sa > Sb;
            if (A.SameGame != B.SameGame) return A.SameGame;
            if (A.RoleFits != B.RoleFits) return A.RoleFits;
            if (A.Words != B.Words) return A.Words > B.Words;
            if (A.Extra != B.Extra) return A.Extra < B.Extra;
            return A.M->Path < B.M->Path;
        }

        Result Choose(const std::string& AnimPath, bool& WantRefs)
        {
            Result R;
            WantRefs = false;
            std::vector<std::string> Names;
            if (!AnimBoneNames(AnimPath, Names)) return R;
            const std::string AnimName = fs::path(AnimPath).stem().string();
            const std::string Game = GameOf(AnimPath);
            // the anim's bones some model of its game has (of any game when the pool holds none of
            // its game): a bone no model has (a sleeve's helper joints) costs no rig its score
            const auto KnownIt = Known.find(Game);
            const bool GameKnown = !Game.empty() && KnownIt != Known.end();
            const std::vector<uint8_t>& Counted = GameKnown ? KnownIt->second : KnownAll;
            std::vector<uint8_t> InAnim;
            int N = 0;
            for (const auto& Nm : Names)
            {
                const int Id = BoneId(Nm);
                if ((size_t)Id >= Counted.size() || !Counted[Id]) continue;
                if ((size_t)Id >= InAnim.size()) InAnim.resize(Id + 1, 0);
                if (!InAnim[Id]) { InAnim[Id] = 1; N++; }
            }
            if (N == 0)
            {
                WantRefs = true;
                return R;
            }
            const Role Want = AnimRole(AnimName);
            const std::set<std::string> AnimWords = Words(AnimName);

            std::vector<Scored> All;
            for (const Model& Md : Pool)
            {
                Scored S;
                S.M = &Md;
                for (const int B : Md.Bones) S.Hits += (size_t)B < InAnim.size() && InAnim[B];
                if (S.Hits == 0) continue;
                S.Extra = (int)Md.Bones.size() - S.Hits;
                S.Score = (float)S.Hits / (float)N;
                S.SameGame = !Game.empty() && Md.Game == Game;
                S.RoleFits = Want != Role::Other && Md.R == Want;
                for (const auto& W : Md.Words) S.Words += (int)AnimWords.count(W);
                All.push_back(S);
            }
            std::sort(All.begin(), All.end(), Before);
            // the anim's own game when one of its models fits half as well as the best of any game's
            // (a body without its head beside another game's whole one), else any game's
            bool OwnGame = false;
            for (const auto& S : All) OwnGame |= S.SameGame && S.Score >= 0.2f && S.Score >= 0.5f * All.front().Score;
            // the user's list: the anim's own game first
            for (const int Pass : { 0, 1 })
                for (const auto& S : All)
                    if (R.Ranked.size() < 400 && (Pass == 0) == (S.SameGame || !OwnGame))
                        R.Ranked.push_back({ S.M->Path, S.M->Base, S.Score, S.M->Reference });
            const Scored* Best = nullptr;
            for (const auto& S : All)
                if (!OwnGame || S.SameGame) { Best = &S; break; }
            if (Best == nullptr || Best->Score < 0.2f)
            {
                WantRefs = true;
                return R;
            }

            // short of bones: the model holding most of the rest, of the same game
            const Scored* Second = nullptr;
            int Gain = 0;
            if (Best->Score < 0.97f)
            {
                std::vector<uint8_t> Missing = InAnim;
                for (const int B : Best->M->Bones) if ((size_t)B < Missing.size()) Missing[B] = 0;
                Scored Top;
                for (const auto& S : All)
                {
                    if (&S == Best || S.M->Base == Best->M->Base || (OwnGame && !S.SameGame)) continue;
                    int G = 0;
                    for (const int B : S.M->Bones) G += (size_t)B < Missing.size() && Missing[B];
                    if (G == 0) continue;
                    Scored C = S;
                    C.Hits = G;
                    C.Score = (float)G / (float)N;
                    if (Second == nullptr || Before(C, Top)) { Second = &S; Top = C; Gain = G; }
                }
                if (Second && (float)Gain / (float)N < 0.04f) { Second = nullptr; Gain = 0; }
            }
            // the skeleton the other joins: a body (a head is bound in its own space), else hands, else
            // the one with more bones (hands before their weapon)
            const Model* First = Best->M;
            const Model* Other = Second ? Second->M : nullptr;
            const auto Rank = [](const Model* Md) { return std::make_pair(Md->R == Role::Body ? 2 : Md->R == Role::View ? 1 : 0, Md->Bones.size()); };
            if (Other && Rank(Other) > Rank(First)) std::swap(First, Other);
            R.P.Paths.push_back(First->Path);
            R.P.Label = First->Base;
            R.P.Reference = First->Reference;
            if (Other)
            {
                R.P.Paths.push_back(Other->Path);
                R.P.Label += " + " + Other->Base;
                R.P.Reference |= Other->Reference;
            }
            // a reference body wears its game's reference head, whatever bones the anim moves (a body
            // the table gives a head for has none of its own)
            const Model* Body = First->RefBody ? First : Other && Other->RefBody ? Other : nullptr;
            int HeadGain = 0;
            if (Body && !First->RefHead && !(Other && Other->RefHead))
                for (const Model& Md : Pool)
                    if (Md.RefHead && Md.Game == Body->Game)
                    {
                        std::vector<uint8_t> Missing = InAnim;
                        for (const Model* Had : { First, Other })
                            if (Had) for (const int B : Had->Bones) if ((size_t)B < Missing.size()) Missing[B] = 0;
                        for (const int B : Md.Bones) HeadGain += (size_t)B < Missing.size() && Missing[B];
                        R.P.Paths.push_back(Md.Path);
                        R.P.Label += " + " + Md.Base;
                        break;
                    }
            R.P.Score = (float)(Best->Hits + Gain + HeadGain) / (float)N;
            const bool HasView = First->R == Role::View || (Other && Other->R == Role::View);
            WantRefs = R.P.Score < 0.9f || (Want == Role::View && !HasView) || !Best->SameGame;
            return R;
        }

        void Loop()
        {
            std::unique_lock<std::mutex> Lk(M);
            for (;;)
            {
                Cv.wait(Lk, [&] { return Stop || (HaveModels && !Queue.empty()); });
                if (Stop) return;
                Busy = true;
                const std::string Anim = Queue.back();
                Queue.pop_back();
                Queued.erase(Anim);
                const uint64_t Key = KeyNow();
                if (Key != PoolKey)
                {
                    const std::vector<ModelRef> Models = Listed;
                    const std::map<std::string, Refs> References = GameRefs;
                    Lk.unlock();
                    BuildPool(Models, References);
                    Lk.lock();
                    PoolKey = Key;
                    PoolSize = Pool.size();
                }
                Lk.unlock();
                bool WantRefs = false;
                Result R = Choose(Anim, WantRefs);
                R.Key = Key;
                const std::string Game = GameOf(Anim);
                Lk.lock();
                Results[Anim] = std::move(R);
                if (WantRefs && Fetch && !Game.empty())
                {
                    Refs& G = GameRefs[Game];
                    if (G.S == Refs::State::None && Table.count(Game))
                    {
                        G.S = Refs::State::Queued;
                        RefQueue.push_back(Game);
                    }
                }
                Busy = false;
                Gen++;
                Cv.notify_all();
            }
        }

        void ReadTable()
        {
            if (TableRead) return;
            TableRead = true;
            const fs::path F = TableFile();
            try
            {
                std::ifstream In(F);
                const nlohmann::json J = nlohmann::json::parse(In);
                for (const auto& [Game, V] : J.at("games").items())
                {
                    if (V.value("from", std::string()) == "install") FromInstall.insert(Game);
                    for (const char* Part : { "hands", "body", "head" })
                        if (V.contains(Part))
                        {
                            Table[Game].push_back(V[Part].get<std::vector<std::string>>());
                            if (Part[1] != 'a')
                                for (const auto& N : Table[Game].back()) (Part[1] == 'o' ? Bodies : Heads)[Game].insert(Lower(N));
                        }
                }
            }
            catch (const std::exception& E)
            {
                AppLog::Log().warn("anim rigs: no reference table ({}): {}", F.string(), E.what());
            }
        }

        // what a game's reference folder holds: the first of each list exported there
        std::vector<ModelRef> KeptRefs(const std::string& Game) const
        {
            std::vector<ModelRef> Out;
            const auto It = Table.find(Game);
            if (It == Table.end()) return Out;
            std::error_code Ec;
            for (const auto& List : It->second)
                for (const auto& Name : List)
                {
                    const fs::path F = RigsRoot() / Game / "models" / Name / (Name + "_LOD0.semodel");
                    if (fs::exists(F, Ec)) { Out.push_back({ F.string(), Name + "_LOD0" }); break; }
                }
            return Out;
        }

        // the names an export was last tried with (tried.txt): a name the install lacks is not asked again each session
        static std::string Signature(const Lists& Names)
        {
            std::string S;
            for (const auto& L : Names)
                for (const auto& N : L) S += N + ",";
            return S;
        }
        static bool Tried(const std::string& Game, const Lists& Names)
        {
            std::ifstream In(RigsRoot() / Game / "tried.txt");
            std::string Line;
            return std::getline(In, Line) && Line == Signature(Names);
        }
        static void MarkTried(const std::string& Game, const Lists& Names)
        {
            std::error_code Ec;
            fs::create_directories(RigsRoot() / Game, Ec);
            std::ofstream(RigsRoot() / Game / "tried.txt") << Signature(Names) << '\n';
        }

        static std::wstring Quote(const std::string& S) { return L"\"" + Strings::ToUnicodeString(S) + L"\""; }

        // disk-export of Names from Zone, as its own process (a reader failing on a zone leaves the browser up)
        bool ExportFrom(const std::string& Game, const std::string& Install, const std::string& Zone,
                               const std::vector<std::string>& Names, std::string& Note)
        {
            std::string Filter;
            for (const auto& N : Names) Filter += (Filter.empty() ? "=" : ",=") + N;   // each name whole
            const fs::path Dir = RigsRoot() / Game;
            std::error_code Ec;
            fs::create_directories(Dir, Ec);
            wchar_t Exe[MAX_PATH * 4] = {};
            GetModuleFileNameW(nullptr, Exe, (DWORD)std::size(Exe));
            std::wstring Cmd = L"\"" + std::wstring(Exe) + L"\" disk-export " + Quote(Zone) + L" --game " + Strings::ToUnicodeString(Game) +
                               L" --install " + Quote(Install) + L" --type model --name-filter " + Quote(Filter) +
                               L" --model-format semodel --image-format png --out " + Quote(RigsRoot().string()) +
                               L" --report " + Quote((Dir / "export_report.txt").string());
            SetEnvironmentVariableW(L"GRAYWOLF_NO_UPDATE", L"1");
            STARTUPINFOW Si{};
            Si.cb = sizeof(Si);
            PROCESS_INFORMATION Pi{};
            std::vector<wchar_t> Mutable(Cmd.begin(), Cmd.end());
            Mutable.push_back(0);
            const std::wstring WorkDir = ExeDir().wstring();
            if (!CreateProcessW(nullptr, Mutable.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, WorkDir.c_str(), &Si, &Pi))
            {
                Note = "could not start disk-export";
                return false;
            }
            bool Done = false;
            for (int Waited = 0; !Done && Waited < 15 * 60 * 5 && !Stopping.load(); Waited++)
                Done = WaitForSingleObject(Pi.hProcess, 200) == WAIT_OBJECT_0;
            if (!Done)
            {
                TerminateProcess(Pi.hProcess, 1);
                WaitForSingleObject(Pi.hProcess, INFINITE);
            }
            DWORD Code = 1;
            GetExitCodeProcess(Pi.hProcess, &Code);
            CloseHandle(Pi.hProcess);
            CloseHandle(Pi.hThread);
            if (!Done || Code != 0) Note = "disk-export of " + Zone + (Done ? " failed (" + std::to_string(Code) + ")" : " timed out");
            return Done && Code == 0;
        }

        // The smallest zone the install index file has each name in (the file read as it is: an index
        // kept before a game update still names zones that are there, and listing the install to
        // check it takes long). False when there is no index file.
        static bool ZonesOf(const std::string& IndexFile, const Lists& Names,
                            std::map<std::string, std::vector<std::string>>& ByZone)
        {
            // binary: text mode ends the file at a Ctrl+Z byte (a hashed image name may hold one)
            std::ifstream In(IndexFile, std::ios::binary);
            if (!In) return false;
            std::map<std::string, std::pair<uint64_t, std::string>> Best;   // lower-case name -> (zone size, zone)
            for (const auto& List : Names)
                for (const auto& N : List) Best[Lower(N)] = { ~0ull, std::string() };
            std::string Line, Zone;
            uint64_t Size = 0;
            while (std::getline(In, Line))
            {
                if (!Line.empty() && Line.back() == '\r') Line.pop_back();
                if (Line.rfind("zone\t", 0) == 0)
                {
                    const auto A = Line.find('\t', 5);
                    Size = std::strtoull(Line.c_str() + 5, nullptr, 10);
                    Zone = A == std::string::npos ? std::string() : Line.substr(A + 1);
                }
                else if (Line.rfind("model\t", 0) == 0)
                {
                    const auto It = Best.find(Lower(Line.substr(6)));
                    if (It != Best.end() && Size < It->second.first) It->second = { Size, Zone };
                }
            }
            for (const auto& List : Names)
                for (const auto& N : List)
                    if (const auto& B = Best[Lower(N)]; !B.second.empty()) { ByZone[B.second].push_back(N); break; }
            return true;
        }

        // Game's reference models exported from its install, each from the smallest zone the install index has it in
        void Export(const std::string& Game, const Lists& Names, std::string& Note)
        {
            const DiskGame::IDiskGame* G = DiskGame::Find(Game);
            if (G == nullptr) { Note = "no disk reader"; return; }
            static const std::vector<GameInstalls::Install> Detected = GameInstalls::Detect();
            DiskGame::ExportOptions Defaults;
            for (const DiskGame::Setting* S : G->AllSettings()) Defaults.Settings[S->Key] = S->Default;
            // the installs detected, then those an index was kept for (an install no detection finds)
            std::vector<GameInstalls::Install> Installs;
            for (const auto& I : Detected) if (I.DiskId == Game) Installs.push_back(I);
            for (const auto& Path : DiskGame::KeptInstalls(*G, Defaults))
            {
                bool Known = false;
                for (const auto& I : Installs) Known |= Lower(fs::absolute(I.Path).string()) == Lower(Path);
                if (!Known) { GameInstalls::Install I; I.Path = Path; I.DiskId = Game; Installs.push_back(I); }
            }
            if (FromInstall.count(Game))
            {
                // an install read whole (archives, no install index): every name asked of it at once, the
                // first of each list it holds kept
                std::vector<std::string> All;
                for (const auto& List : Names) All.insert(All.end(), List.begin(), List.end());
                Note = "no install found";
                for (const auto& I : Installs)
                {
                    Note.clear();
                    const bool Ok = ExportFrom(Game, I.Path, I.Path, All, Note);
                    AppLog::Log().info("anim rigs: {} references from the install {}: {}", Game, I.Path, Ok ? "exported" : Note);
                    return;
                }
                return;
            }
            Note = "no install index (Load Game: index the install)";
            for (const auto& I : Installs)
            {
                DiskGame::ExportOptions O = Defaults;
                O.Install = I.Path;
                std::map<std::string, std::vector<std::string>> ByZone;
                const std::string IndexFile = DiskGame::KeptFile(*G, O, I.Path, ".idx");
                if (!ZonesOf(IndexFile, Names, ByZone)) continue;
                AppLog::Log().info("anim rigs: {} references from the index {}: {} zones", Game, IndexFile, ByZone.size());
                if (ByZone.empty()) { Note = "the install index of " + I.Path + " holds none of the names"; continue; }
                Note.clear();
                for (const auto& [Zone, Names] : ByZone)
                {
                    std::vector<std::string> Paths;
                    std::string Error;
                    if (!G->ResolveZones(I.Path, { Zone }, O, Paths, Error) || Paths.empty())
                        Note = Zone + ": " + Error;
                    else
                    {
                        // the zone's file, or its name in the install when it is no file of its own (in an archive)
                        std::error_code Ec;
                        const std::string& Input = fs::exists(fs::path(Strings::ToUnicodeString(Paths[0])), Ec) ? Paths[0] : Zone;
                        const bool Ok = ExportFrom(Game, I.Path, Input, Names, Note);
                        AppLog::Log().info("anim rigs: {} reference {} from {}: {}", Game, Names.front(), Input, Ok ? "exported" : Note);
                    }
                }
                return;
            }
        }

        void RefLoop()
        {
            WraithX::InitializeAPI();
            std::unique_lock<std::mutex> Lk(M);
            for (;;)
            {
                Cv.wait(Lk, [&] { return Stop || !RefQueue.empty(); });
                if (Stop) return;
                RefBusy = true;
                const std::string Game = RefQueue.front();
                RefQueue.pop_front();
                const Lists Names = Table[Game];
                std::vector<ModelRef> Kept = KeptRefs(Game);
                Lk.unlock();
                std::string Note;
                if (Kept.size() < Names.size() && !Tried(Game, Names))
                {
                    AppLog::Timer T("anim rigs: reference models of " + Game, spdlog::level::info, 0);
                    Export(Game, Names, Note);
                    MarkTried(Game, Names);
                    Lk.lock();
                    Kept = KeptRefs(Game);
                    Lk.unlock();
                    T.Detail(": " + std::to_string(Kept.size()) + " kept" + (Note.empty() ? "" : ", " + Note));
                }
                Lk.lock();
                Refs& R = GameRefs[Game];
                R.S = Refs::State::Done;
                R.Models = std::move(Kept);
                R.Note = Note;
                if (!R.Models.empty())
                {
                    RefVersion++;
                    // the game's anims are picked again with them
                    for (auto& [Anim, Res] : Results)
                        if (GameOf(Anim) == Game && Queued.insert(Anim).second) Queue.push_front(Anim);
                }
                RefBusy = false;
                Gen++;
                Cv.notify_all();
            }
        }

        void Enqueue(const std::string& Anim)
        {
            if (Queued.insert(Anim).second) Queue.push_back(Anim);
            Cv.notify_all();
        }
    };

    Index::Index() : P(std::make_shared<Impl>())
    {
        // references kept by an earlier session are used without asking
        {
            std::lock_guard<std::mutex> Lk(P->M);
            P->ReadTable();
            for (const auto& [Game, Names] : P->Table)
            {
                auto Kept = P->KeptRefs(Game);
                if (Kept.size() >= Names.size() || (!Kept.empty() && Impl::Tried(Game, Names)))
                {
                    P->GameRefs[Game].S = Impl::Refs::State::Done;
                    P->GameRefs[Game].Models = std::move(Kept);
                }
            }
        }
        P->Worker = std::thread([Q = P.get()] { Q->Loop(); });
        P->RefWorker = std::thread([Q = P.get()] { Q->RefLoop(); });
    }

    Index::~Index()
    {
        {
            std::lock_guard<std::mutex> Lk(P->M);
            P->Stop = true;
            P->Stopping = true;
        }
        P->Cv.notify_all();
        if (P->Worker.joinable()) P->Worker.join();
        // a reference export running is waited for (its process, at most its timeout)
        if (P->RefWorker.joinable()) P->RefWorker.join();
    }

    void Index::SetModels(std::vector<ModelRef> Models, uint64_t Version)
    {
        std::lock_guard<std::mutex> Lk(P->M);
        if (P->HaveModels && Version == P->ListedVersion) return;
        P->Listed = std::move(Models);
        P->ListedVersion = Version;
        P->HaveModels = true;
        P->Gen++;
        P->Cv.notify_all();
    }

    bool Index::PickFor(const std::string& AnimPath, Pick& Out)
    {
        std::lock_guard<std::mutex> Lk(P->M);
        const auto It = P->Results.find(AnimPath);
        if (It == P->Results.end() || It->second.Key != P->KeyNow()) P->Enqueue(AnimPath);
        if (It == P->Results.end()) return false;
        Out = It->second.P;
        if (const auto Mn = P->Manual.find(AnimPath); Mn != P->Manual.end())
        {
            Out = Pick{};
            Out.Paths = { Mn->second };
            Out.Manual = true;
            Out.Label = WithoutLod(fs::path(Mn->second).stem().string());
            for (const auto& C : It->second.Ranked)
                if (C.Path == Mn->second) { Out.Score = C.Score; Out.Reference = C.Reference; }
        }
        return true;
    }

    bool Index::Ranked(const std::string& AnimPath, std::vector<Candidate>& Out)
    {
        std::lock_guard<std::mutex> Lk(P->M);
        const auto It = P->Results.find(AnimPath);
        if (It == P->Results.end()) { P->Enqueue(AnimPath); return false; }
        Out = It->second.Ranked;
        return true;
    }

    void Index::SetManual(const std::string& AnimPath, const std::string& ModelPath)
    {
        std::lock_guard<std::mutex> Lk(P->M);
        if (ModelPath.empty()) P->Manual.erase(AnimPath); else P->Manual[AnimPath] = ModelPath;
        P->Gen++;
    }

    uint64_t Index::Generation() const { return P->Gen.load(); }

    void Index::SetFetchReferences(bool On)
    {
        std::lock_guard<std::mutex> Lk(P->M);
        P->Fetch = On;
    }

    void Index::WaitIdle()
    {
        std::unique_lock<std::mutex> Lk(P->M);
        P->Cv.wait(Lk, [&] { return P->Stop || (P->Queue.empty() && !P->Busy && P->RefQueue.empty() && !P->RefBusy); });
    }

    std::string Index::Stats() const
    {
        std::lock_guard<std::mutex> Lk(P->M);
        size_t Picks = 0, Rigged = 0, Good = 0;
        for (const auto& [Anim, R] : P->Results)
        {
            Picks++;
            Rigged += !R.P.Paths.empty();
            Good += !R.P.Paths.empty() && R.P.Score >= 0.8f;
        }
        std::string S = "anim rigs: " + std::to_string(Picks) + " anims picked, " + std::to_string(Rigged) + " with a rig, " +
                        std::to_string(Good) + " scoring 0.8 or more, " + std::to_string(P->PoolSize) + " models";
        for (const auto& [Game, R] : P->GameRefs)
            S += ", " + Game + " references " + (R.S == Impl::Refs::State::Done ? std::to_string(R.Models.size()) : std::string("queued")) +
                 (R.Note.empty() ? "" : " (" + R.Note + ")");
        return S;
    }
}
