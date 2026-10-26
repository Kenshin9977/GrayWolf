#include "stdafx.h"

#include "ExportWriters.h"

#include "DiskGame.h"

#include "AnimOwners.h"
#include "DiskGame.Materials.h"
#include "DiskGame.Monitor.h"
#include "ExportNaming.h"
#include "FastFile.h"
#include "Games/GameModule.h"
#include "MaterialGdt.h"
#include "MemoryBudget.h"
#include "SettingsManager.h"
#include "Siren.h"
#include "WmaSounds.h"
#include "XboxPackages.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <tuple>

namespace fs = std::filesystem;

namespace DiskGame
{
namespace
{
    // A zone file: a .ff that holds something (a Game Pass package keeps an empty .ff for each zone
    // of the game it does not ship, those of DLC not installed among them)
    bool IsZoneFile(const fs::directory_entry& E)
    {
        std::error_code Ec;
        return ExportNaming::Lower(E.path().extension().string()) == ".ff" && E.is_regular_file(Ec) && E.file_size(Ec) > 0;
    }

    std::vector<const IDiskGame*>& Registry()
    {
        static std::vector<const IDiskGame*> List;
        return List;
    }
}

const std::vector<Kind>& AllKinds()
{
    static const std::vector<Kind> All = [] {
        std::vector<Kind> V;
        for (const KindInfo& I : Kinds) V.push_back(I.Id);
        return V;
    }();
    return All;
}

const KindInfo& Info(Kind K)
{
    return Kinds[static_cast<size_t>(K)];
}

bool ParseKinds(const std::string& Text, std::set<Kind>& Out, std::string& Error)
{
    std::stringstream Ss(ExportNaming::Lower(Text));
    std::string Item;
    while (std::getline(Ss, Item, ','))
    {
        Item.erase(0, Item.find_first_not_of(" 	"));
        Item.erase(Item.find_last_not_of(" 	") + 1);
        if (Item.empty()) continue;
        const KindInfo* Found = nullptr;
        for (const KindInfo& I : Kinds)
            if (Item == I.Name || Item == I.Folder) Found = &I;
        if (Found == nullptr)
        {
            std::string Names;
            for (const KindInfo& I : Kinds) Names += std::string(Names.empty() ? "" : ", ") + I.Name;
            Error = "unknown --type " + Item + " (" + Names + ")";
            return false;
        }
        Out.insert(Found->Id);
    }
    if (Out.empty())
        for (const KindInfo& I : Kinds)
            if (I.ByDefault) Out.insert(I.Id);
    return true;
}

std::string Setting::Of(const ExportOptions& O) const
{
    return Of(O.Settings);
}

std::string Setting::Of(const std::map<std::string, std::string>& Values) const
{
    const auto It = Values.find(Key);
    return It == Values.end() || It->second.empty() ? Default : It->second;
}

const Setting MapPng{ "map_png", "Map textures", { { "off", "Blender (DDS)" }, { "on", "+ PNG for glTF tools" } },
                      "off", { Kind::Map } };

const Setting MapTextures{ "map_textures", "Map texture size", { { "full", "Full" }, { "2k", "2K" }, { "1k", "1K" } },
                           "full", { Kind::Map } };

const Setting MapCast{ "map_cast", "Map scene", { { "off", ".glb (Blender)" }, { "on", "+ Cast (Maya, Cast plugins)" } },
                       "off", { Kind::Map } };

const Setting MapRadiant{ "map_radiant", "Map source", { { "off", "None" }, { "on", "+ Radiant .map (mod tools)" } },
                          "off", { Kind::Map } };

const Setting GdtMaterials{ "material_gdt", "Mod tools materials", { { "off", "None" }, { "on", "+ BO3 material GDT" } },
                            "off", { Kind::Model, Kind::Map } };
const Setting SoundAliases{ "sound_aliases", "Sound aliases", { { "off", "None" }, { "on", "+ alias CSV (BO3 mod tools)" } },
                            "off", { Kind::Sound } };
const Setting MapCollision{ "map_collision", "Map collision", { { "off", "None" }, { "on", "+ collision .json (clip map)" } },
                            "off", { Kind::Map } };

const Setting MapContent{ "map_content", "Map content", { { "all", "Everything" }, { "terrain", "Terrain only" } },
                          "all", { Kind::Map } };

const Setting ScriptDecompile{ "decompile", "Compiled scripts and Lua", { { "off", "Bytecode" }, { "on", "Decompiled" } },
                               "off", { Kind::Script, Kind::Rawfile } };

const std::vector<Setting>& CommonSettings()
{
    static const std::vector<Setting> All = { MapPng, MapTextures, MapCast, MapRadiant, GdtMaterials, SoundAliases, MapCollision, MapContent, ScriptDecompile };
    return All;
}

bool Setting::Parse(const std::string& Text, std::string& Value) const
{
    const std::string Want = ExportNaming::Lower(Text);
    for (const auto& [V, Label] : Values)
        if (Want == ExportNaming::Lower(V) || Want == ExportNaming::Lower(Label))
        {
            Value = V;
            return true;
        }
    return false;
}

bool IDiskZone::Export(Kind K, const Context& C, Totals& T) const
{
    const Exporter* E = ExporterOf(K);
    if (E == nullptr) return false;
    (*E)(C, T);
    return true;
}

const IDiskZone::Exporter* IDiskZone::ExporterOf(Kind K) const
{
    const auto It = Exporters.find(K);
    return It == Exporters.end() ? nullptr : &It->second;
}

std::set<Kind> IDiskZone::Supported() const
{
    std::set<Kind> Out;
    for (const auto& [K, E] : Exporters) Out.insert(K);
    return Out;
}

const std::vector<Setting>& IDiskGame::Settings() const
{
    static const std::vector<Setting> None;
    return None;
}

std::vector<const Setting*> IDiskGame::AllSettings() const
{
    std::vector<const Setting*> Out;
    for (const Setting& S : Settings()) Out.push_back(&S);
    const std::set<Kind> Exported = Kinds();
    for (const Setting& S : CommonSettings())
        for (Kind K : S.Concerns)
            if (Exported.count(K))
            {
                Out.push_back(&S);
                break;
            }
    return Out;
}

std::set<Kind> IDiskGame::Kinds() const
{
    static std::mutex Lock;
    static std::map<const IDiskGame*, std::set<Kind>> Known;
    std::lock_guard<std::mutex> Guard(Lock);
    const auto It = Known.find(this);
    if (It != Known.end()) return It->second;
    const auto Zone = Unopened();
    return Known[this] = Zone ? Zone->Supported() : std::set<Kind>();
}

bool AssetId::operator<(const AssetId& O) const
{
    if (K != O.K) return K < O.K;
    return _stricmp(Name.c_str(), O.Name.c_str()) < 0;
}

struct Context::Limited
{
    std::mutex Lock;
    std::map<Kind, Selection> Names;
};

bool Context::Wants(Kind K, const std::string& Name) const
{
    bool Yes = false;
    if (Only) Yes = Only->count({ K, Name }) != 0;
    else if (NameFilter.empty()) Yes = true;
    else
    {
        // parsed once per filter text, not per asset
        thread_local std::string Text;
        thread_local ExportNaming::NameFilter Filter;
        if (Text != NameFilter) Filter = ExportNaming::NameFilter(Text = NameFilter);
        Yes = Filter.Matches(Name);
    }
    if (Yes && !Only && Limit > 0 && LimitSeen)
    {
        // the same asset asked again (a reader's second pass) is still wanted
        std::lock_guard<std::mutex> G(LimitSeen->Lock);
        auto& Names = LimitSeen->Names[K];
        if (Names.size() >= Limit && !Names.count({ K, Name })) Yes = false;
        else Names.insert({ K, Name });
    }
    if (Yes && OnWanted) OnWanted({ K, Name });
    return Yes;
}

std::string Context::KindDir(Kind K) const
{
    const fs::path Dir = fs::path(OutRoot) / KindFolder(K);
    std::error_code Ec;
    fs::create_directories(Dir, Ec);
    return Dir.string();
}

void Totals::Fail(const std::string& Zone, Kind K, const std::string& Name, const std::string& Why)
{
    Failures.push_back(Zone + ": " + KindName(K) + " " + Name + (Why.empty() ? "" : " " + Why));
}

void Totals::Lacks(const std::string& Zone, Kind K, const std::string& Name, const std::string& Why)
{
    Absent.push_back(Zone + ": " + KindName(K) + " " + Name + (Why.empty() ? "" : " " + Why));
}

bool IDiskGame::Zones(const std::string& Input, const ExportOptions&, std::vector<std::string>& Out, std::string& Error) const
{
    const fs::path In(Input);
    std::error_code Ec;
    if (fs::is_directory(In, Ec))
    {
        for (const auto& E : fs::recursive_directory_iterator(In, Ec))
            if (IsZoneFile(E)) Out.push_back(E.path().string());
        std::sort(Out.begin(), Out.end());
    }
    else if (ExportNaming::Lower(In.extension().string()) == ".txt")
    {
        // a list: one zone path per line (a long run cut in parts, or several runs side by side)
        std::ifstream List(In);
        for (std::string Line; std::getline(List, Line);)
        {
            while (!Line.empty() && (Line.back() == '\r' || Line.back() == ' ')) Line.pop_back();
            if (!Line.empty() && fs::exists(fs::path(Line), Ec)) Out.push_back(Line);
        }
    }
    else if (fs::exists(In, Ec))
        Out.push_back(In.string());
    if (Out.empty()) Error = "no zone in " + Input;
    return !Out.empty();
}

bool IDiskGame::ListZones(const std::string& Install, const ExportOptions&, std::vector<std::string>& Names, std::string& Error) const
{
    std::error_code Ec;
    for (const auto& E : fs::recursive_directory_iterator(Install, fs::directory_options::skip_permission_denied, Ec))
        if (IsZoneFile(E))
            Names.push_back(fs::relative(E.path(), Install, Ec).string());
    std::sort(Names.begin(), Names.end());
    // an Xbox app game's DLC, each its own package beside the game's: its zones as
    // "<package name>\<path in it>" (ResolveZones maps them back)
    for (const auto& D : XboxPackages::Dlc(Install))
    {
        const size_t First = Names.size();
        for (const auto& E : fs::recursive_directory_iterator(D.Root, fs::directory_options::skip_permission_denied, Ec))
            if (IsZoneFile(E))
                Names.push_back((fs::path(D.Name) / fs::relative(E.path(), D.Root, Ec)).string());
        std::sort(Names.begin() + (std::ptrdiff_t)First, Names.end());
    }
    if (Names.empty()) Error = "no zone in " + Install;
    return !Names.empty();
}

bool IDiskGame::ResolveZones(const std::string& Install, const std::vector<std::string>& Names, const ExportOptions&,
                             std::vector<std::string>& Out, std::string& Error) const
{
    const auto Dlc = XboxPackages::Dlc(Install);
    for (const auto& N : Names)
    {
        fs::path Path = fs::path(Install) / N;
        for (const auto& D : Dlc)
        {
            const std::string Prefix = ExportNaming::Lower(D.Name) + "\\";
            if (ExportNaming::Lower(N).rfind(Prefix, 0) == 0) Path = fs::path(D.Root) / N.substr(Prefix.size());
        }
        Out.push_back(Path.string());
    }
    if (Out.empty()) Error = "no zone picked";
    return !Out.empty();
}

std::string IDiskGame::InstallOfZone(const std::string& Zone) const
{
    return fs::path(Zone).parent_path().string();
}

Registration::Registration(const IDiskGame& Game)
{
    Registry().push_back(&Game);
}

const std::vector<const IDiskGame*>& Games()
{
    return Registry();
}

const IDiskGame* Find(const std::string& IdOrAlias)
{
    const std::string Want = ExportNaming::Lower(IdOrAlias);
    for (const IDiskGame* G : Registry())
    {
        if (ExportNaming::Lower(G->Id()) == Want) return G;
        for (const auto& A : G->Aliases())
            if (ExportNaming::Lower(A) == Want) return G;
    }
    return nullptr;
}

const IDiskGame* Detect(const std::string& Input)
{
    std::string First = Input;
    std::error_code Ec;
    if (fs::is_directory(Input, Ec))
    {
        First.clear();
        for (const auto& E : fs::recursive_directory_iterator(Input, Ec))
            if (IsZoneFile(E)) { First = E.path().string(); break; }
    }
    if (First.empty()) return nullptr;
    const auto Info = FastFile::Identify(First);
    const GameModule* Module = FindGameModule(Info.Magic, Info.Version, First);
    if (Module == nullptr) return nullptr;
    for (const IDiskGame* G : Registry())
        if (std::string(G->Engine()) == Module->Engine) return G;
    return nullptr;
}

std::string Report(const Totals& T)
{
    std::ostringstream Os;
    Os << "zones=" << T.Zones << "\nzones_walked_to_end=" << T.ZonesWalked << "\nzones_failed=" << T.ZonesFailed << "\n";
    for (const auto& [K, V] : T.Kinds)
    {
        const std::string N = KindFolder(K);
        Os << N << "=" << V.Seen << "\n" << N << "_written=" << V.Written << "\n";
        if (V.Known) Os << N << "_known=" << V.Known << "\n";
        if (V.Linked) Os << N << "_linked=" << V.Linked << "\n";
        if (V.References) Os << N << "_references=" << V.References << "\n";
    }
    for (const auto& [K, N] : T.Pulled)
    {
        const auto From = T.PulledFrom.find(K);
        Os << "deps_" << KindFolder(K) << "=" << N << " from " << (From == T.PulledFrom.end() ? 0 : From->second.size())
           << " zones\n";
    }
    for (Kind K : T.Unsupported) Os << "unsupported=" << KindFolder(K) << "\n";
    for (const auto& [Name, N] : T.Counters) Os << Name << "=" << N << "\n";
    if (const auto It = T.Counters.find(WmaSounds::NoFfmpegCounter); It != T.Counters.end()) Os << "warning " << WmaSounds::NoFfmpegMessage(It->second) << "\n";
    Os << "failures=" << T.Failures.size() << "\n";
    for (const auto& F : T.Failures) Os << "failure " << F << "\n";
    if (!T.Absent.empty()) Os << "absent_from_install=" << T.Absent.size() << "\n";
    for (const auto& A : T.Absent) Os << "absent " << A << "\n";
    if (!T.Unresolved.empty()) Os << "deps_unresolved=" << T.Unresolved.size() << "\n";
    for (const auto& U : T.Unresolved) Os << "unresolved " << U << "\n";
    if (!T.IndexRenamed.empty()) Os << "index_renamed=" << T.IndexRenamed.size() << "\n";
    for (const auto& R : T.IndexRenamed) Os << "index_renamed " << R << "\n";
    if (T.Cancelled) Os << "cancelled=1\n";
    for (const auto& E : T.ZoneErrors) Os << "zone_error " << E << "\n";
    for (const auto& [K, V] : T.Timing) Os << K << "=" << V << "\n";
    return Os.str();
}

namespace
{
    std::string Stem(const std::string& Zone)
    {
        return ExportNaming::Lower(fs::path(Zone).stem().string());
    }

    // What a zone's exports met and needed, settled once the zone is done: an asset met and not
    // needed was exported; one needed and not exported yet is a dependency.
    struct Ledger
    {
        Selection Exported;
        Selection Met, Asked;
        void Wire(Context& C)
        {
            C.OnWanted = [this](const AssetId& A) { Met.insert(A); };
            C.OnNeeds = [this](const AssetId& A) { Asked.insert(A); };
        }
        Selection Settle()
        {
            for (const auto& A : Met)
                if (!Asked.count(A)) Exported.insert(A);
            Selection Needs;
            for (const auto& A : Asked)
                if (!Exported.count(A)) Needs.insert(A);
            Met.clear();
            Asked.clear();
            return Needs;
        }
    };

    double Seconds(std::chrono::steady_clock::time_point Since)
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - Since).count();
    }

    std::string Ms(double Seconds)
    {
        return std::to_string((long long)(Seconds * 1000)) + " ms";
    }

    // "opened in N ms (load a, walk b...)", each of the reader's steps added to the run's phases
    std::string Opened(const IDiskZone& Zone, double Took, RunMonitor& Mon, const std::string& Phase)
    {
        std::vector<std::pair<std::string, double>> Steps;
        Zone.OpenTimes(Steps);
        std::string Text = "opened in " + Ms(Took) + (Zone.WalkedToEnd() ? "" : " (not walked to its end)");
        for (size_t I = 0; I < Steps.size(); I++)
        {
            Text += (I ? ", " : " (") + Steps[I].first + " " + Ms(Steps[I].second) + (I + 1 == Steps.size() ? ")" : "");
            Mon.AddTime(Phase + Steps[I].first, Steps[I].second);
        }
        return Text;
    }

    // a dependency -> the zones it may not come from (those that needed it, or failed to give it)
    using Wanted = std::map<AssetId, std::set<std::string>>;

    std::string Describe(const AssetId& A, const std::string& Why)
    {
        return std::string(KindName(A.K)) + " " + A.Name + ": " + Why;
    }

    // Each round groups the pending dependencies by a zone that defines them (the one giving the
    // most first) and opens each such zone once, alone; what those exports need in turn is the
    // next round. An asset is asked of a zone at most once: no cycle, nothing written twice.
    void PullDependencies(const IDiskGame& Game, const ExportOptions& Options, const std::string& Install,
                          const std::string& ReaderKey, Wanted Pending, Context& Run, Ledger& L, Totals& T,
                          Progress& P, const ProgressFn& OnProgress, const std::function<bool()>& Cancelled, RunMonitor& Mon)
    {
        P.ZoneName = "indexing the install";
        P.Current.reset();
        P.Dependencies = true;
        Mon.Where("the install", "indexing");
        Mon.Log(std::to_string(Pending.size()) + " dependencies to pull: reading the install index");
        std::string Error;
        const auto Index = ZoneIndex::For(Game, Options, Install, ReaderKey, [&](size_t Done, size_t Count) {
            Mon.Touch();
            P.Done = Done;
            P.Count = Count;
            if (OnProgress) OnProgress(P);
        }, Run.Cancel, Error);
        if (!Index)
        {
            Mon.Log("no index of " + Install + ": " + Error);
            for (const auto& [A, Not] : Pending) T.Unresolved.push_back(Describe(A, "no index of " + Install + ": " + Error));
            return;
        }
        Mon.AddTime(Index->Built ? "index_build" : "index_read", Index->Seconds);
        Mon.Log(std::string(Index->Built ? "index built" : "index read") + " in " + Ms(Index->Seconds) + ": " +
                std::to_string(Index->Zones().size()) + " zones, " + std::to_string(Index->Entries) + " assets (" + Index->File + ")");
        T.Counters[Index->Built ? "index_built_ms" : "index_read_ms"] += (size_t)(Index->Seconds * 1000);
        T.Counters["index_zones"] = Index->Zones().size();
        T.Counters["index_assets"] = Index->Entries;
        for (const auto& R : Index->Renamed) Mon.Log("index: a name that is not text: " + R);
        T.IndexRenamed.insert(T.IndexRenamed.end(), Index->Renamed.begin(), Index->Renamed.end());

        Totals Deps;                               // the dependency exports: Written is what was pulled
        std::map<AssetId, std::string> LastWhy;    // why the last zone asked did not give it
        Selection GivenUp;                         // reported unresolved, once
        // A zone read in one round and kept, parked, for the next, which may ask it again (what a
        // zone of the same round needed from it): read once, not twice.
        std::map<size_t, std::unique_ptr<IDiskZone>> Parked;
        for (int Round = 1; !Pending.empty() && !Cancelled(); Round++)
        {
            std::map<AssetId, std::vector<size_t>> Candidates;
            for (const auto& [A, Not] : Pending)
            {
                if (GivenUp.count(A)) continue;
                std::vector<size_t> Zs;
                for (size_t Z : Index->DefinedIn(A))
                    if (!Not.count(Stem(Index->Zones()[Z]))) Zs.push_back(Z);
                if (!Zs.empty())
                {
                    Candidates[A] = std::move(Zs);
                    continue;
                }
                GivenUp.insert(A);
                T.Unresolved.push_back(Describe(A, LastWhy.count(A) ? LastWhy[A]
                                                   : Index->DefinedIn(A).empty() ? "no zone of the install holds it whole (or its reader cannot write it)"
                                                                                 : "defined only in the zone that needs it"));
            }
            std::vector<std::pair<size_t, Selection>> Plan;
            while (!Candidates.empty())
            {
                std::map<size_t, size_t> Gives;
                for (const auto& [A, Zs] : Candidates)
                    for (size_t Z : Zs) Gives[Z]++;
                const size_t Best = std::max_element(Gives.begin(), Gives.end(),
                    [](const auto& X, const auto& Y) { return X.second < Y.second; })->first;
                Selection Take;
                for (auto It = Candidates.begin(); It != Candidates.end();)
                {
                    const bool Here = std::find(It->second.begin(), It->second.end(), Best) != It->second.end();
                    if (Here) Take.insert(It->first);
                    It = Here ? Candidates.erase(It) : std::next(It);
                }
                Plan.emplace_back(Best, std::move(Take));
            }
            Mon.Log("dependencies round " + std::to_string(Round) + ": " + std::to_string(Pending.size()) + " pending, from " +
                    std::to_string(Plan.size()) + " zones");
            for (auto It = Parked.begin(); It != Parked.end();)
            {
                const size_t Kept = It->first;
                const bool Asked = std::any_of(Plan.begin(), Plan.end(), [&](const auto& P) { return P.first == Kept; });
                It = Asked ? std::next(It) : Parked.erase(It);
            }

            Wanted Next;
            for (const auto& [Z, Take] : Plan)
            {
                const std::string& ZoneName = Index->Zones()[Z];
                const std::string From = Stem(ZoneName);
                // not given here: the next round asks another zone that defines it
                const auto Elsewhere = [&](const AssetId& A, const std::string& Why) {
                    auto& Not = Next[A];
                    Not.insert(Pending[A].begin(), Pending[A].end());
                    Not.insert(From);
                    LastWhy[A] = Why;
                };
                if (Cancelled()) break;
                std::string Why;
                std::unique_ptr<IDiskZone> Zone;
                if (const auto Kept = Parked.find(Z); Kept != Parked.end())
                {
                    Zone = std::move(Kept->second);
                    Parked.erase(Kept);
                }
                else
                {
                    Why = MemoryBudget::CheckFits(Index->Footprint(Z), ZoneName);
                    // the zones kept for a later round give their memory back before one is refused
                    if (!Why.empty() && !Parked.empty())
                    {
                        Parked.clear();
                        Why = MemoryBudget::CheckFits(Index->Footprint(Z), ZoneName);
                    }
                }
                if (Why.empty())
                {
                    P.ZoneName = From + " (dependencies)";
                    P.Current.reset();
                    P.Done = P.Count = 0;
                    if (OnProgress) OnProgress(P);
                    std::map<Kind, size_t> Asked;
                    for (const auto& A : Take) Asked[A.K]++;
                    std::string What;
                    for (const auto& [K, N] : Asked) What += (What.empty() ? "" : ", ") + std::to_string(N) + " " + KindFolder(K);
                    Mon.Log("dependencies zone " + From + ": " + What);
                    auto Start = std::chrono::steady_clock::now();
                    if (Zone)
                    {
                        Mon.Where(From, "dependencies: resuming");
                        Zone->Resume();
                        const double ResumeS = Seconds(Start);
                        Mon.AddTime("deps_resume", ResumeS);
                        Mon.Log("  " + From + ": kept from the last round, resumed in " + Ms(ResumeS));
                    }
                    else
                    {
                        Mon.Where(From, "dependencies: resolving");
                        std::vector<std::string> Paths;
                        const bool Resolved = Game.ResolveZones(Install, { ZoneName }, Options, Paths, Error) && !Paths.empty();
                        const double ResolveS = Seconds(Start);
                        Mon.AddTime("deps_resolve", ResolveS);
                        if (Resolved)
                        {
                            Mon.Where(From, "dependencies: opening");
                            Start = std::chrono::steady_clock::now();
                            Zone = Game.Open(Paths.front(), Options, Error);
                            const double OpenS = Seconds(Start);
                            Mon.AddTime("deps_open", OpenS);
                            if (Zone) Mon.Log("  " + From + ": resolved in " + Ms(ResolveS) + ", " + Opened(*Zone, OpenS, Mon, "deps_open_"));
                        }
                        if (!Zone) Why = ZoneName + " did not open: " + Error;
                    }
                }
                if (!Zone)
                {
                    Mon.Log("  dependencies zone " + From + " not read: " + Why);
                    for (const auto& A : Take) Elsewhere(A, Why);
                    continue;
                }
                for (Kind K : AllKinds())
                {
                    auto Only = std::make_shared<Selection>();
                    for (const auto& A : Take)
                        if (A.K == K) Only->insert(A);
                    if (Only->empty() || Cancelled()) continue;
                    Context Dep = Run;
                    Dep.Only = std::move(Only);
                    Dep.NameFilter.clear();
                    Dep.Kinds = { K };
                    P.Current = K;
                    P.Done = P.Count = 0;
                    if (OnProgress) OnProgress(P);
                    const size_t Before = Deps.Kinds[K].Written;
                    Mon.Where(From, std::string("dependencies: ") + KindName(K));
                    const KindMeter Meter(Deps, K);
                    if (Zone->Export(K, Dep, Deps) && Deps.Kinds[K].Written > Before) T.PulledFrom[K].insert(From);
                    Mon.EndAsset();
                    const KindSample S = Meter.Stop(Deps);
                    Mon.AddKind(std::string("deps ") + KindName(K), S);
                    Mon.Log("  " + From + " " + KindName(K) + ": " + S.Describe());
                }
                // kept while memory allows twice its size: the next round may ask it again
                if (MemoryBudget::Available() >= 2 * Zone->Footprint() && Zone->Park()) Parked[Z] = std::move(Zone);
                Zone.reset();
                for (const auto& A : L.Settle()) Next[A].insert(From);
                for (const auto& A : Take)
                    if (!L.Exported.count(A)) Elsewhere(A, "not exported from " + From);
            }
            for (auto It = Next.begin(); It != Next.end();)
                It = L.Exported.count(It->first) ? Next.erase(It) : std::next(It);
            if (Cancelled())
                for (const auto& [A, Not] : Pending)
                    if (!L.Exported.count(A)) Next[A];
            Pending.swap(Next);
        }
        Parked.clear();
        for (const auto& [A, Not] : Pending) T.Unresolved.push_back(Describe(A, "cancelled"));
        if (!T.Unresolved.empty()) Mon.Log(std::to_string(T.Unresolved.size()) + " dependencies unresolved (see the report)");
        for (const auto& [K, V] : Deps.Kinds)
            if (V.Written) T.Pulled[K] += V.Written;
        T.Failures.insert(T.Failures.end(), Deps.Failures.begin(), Deps.Failures.end());
        T.Absent.insert(T.Absent.end(), Deps.Absent.begin(), Deps.Absent.end());
    }

    // "~3 min left" - rounded, never claiming more precision than a rough estimate has.
    std::string FormatEtaDuration(double Sec)
    {
        if (Sec < 45) return "under a minute left";
        if (Sec < 3600)
        {
            long long Min = (long long)((Sec + 30) / 60);
            if (Min < 1) Min = 1;
            return "~" + std::to_string(Min) + " min left";
        }
        const long long Hr = (long long)((Sec + 1800) / 3600);
        return "~" + std::to_string(Hr) + " h left";
    }
}

void Eta::Observe(const Progress& P)
{
    if (P.FromWatchdog) return;
    TotalZones = P.Zones;
    if (P.Dependencies)
    {
        InDeps = true;
        DepZone = P.ZoneName;
        DepDone = P.Done;
        DepCount = P.Count;
        return;
    }
    InDeps = false;

    // A new zone or kind: bank the one that just ended (if its Count was ever known) before
    // starting the clock over for what's now running.
    const auto Now = std::chrono::steady_clock::now();
    if (P.Zone != CurZone || P.Current != CurKind)
    {
        if (CurKind && LastCount > 0)
        {
            KindSeconds[*CurKind] += std::chrono::duration<double>(Now - KindStarted).count();
            KindZonesSeen[*CurKind]++;
        }
        KindStarted = Now;
        CurZone = P.Zone;
        CurKind = P.Current;
        CurKindRate = 0.0;
        LastDone = 0;
    }
    if (P.Done > LastDone)
    {
        const double Dt = std::chrono::duration<double>(Now - LastTick).count();
        if (Dt > 0.0)
        {
            const double Rate = (double)(P.Done - LastDone) / Dt;
            CurKindRate = (CurKindRate <= 0.0) ? Rate : (CurKindRate * 0.7 + Rate * 0.3);
        }
        LastDone = P.Done;
    }
    LastTick = Now;
    LastCount = P.Count;
}

std::string Eta::Text() const
{
    if (InDeps)
    {
        std::string S = "pulling dependencies: " + DepZone;
        if (DepCount) S += ", " + std::to_string(DepDone) + "/" + std::to_string(DepCount);
        return S;
    }
    if (TotalZones == 0 || !CurKind) return "estimating...";

    // The current kind's remainder needs either a live rate (a couple of real seconds of it, not
    // the first jumpy tick) or an earlier zone's measured average for the same kind; with neither,
    // there is nothing yet to estimate from.
    const double KindElapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - KindStarted).count();
    const bool LiveRateReady = CurKindRate > 0.0 && KindElapsed >= 2.0;
    const bool HistoryReady = KindZonesSeen.count(*CurKind) != 0;
    if (!LiveRateReady && !HistoryReady) return "estimating...";

    // The rest of the kind in progress (its own live rate, falling back to its own average),
    // then a projected full pass of every wanted kind for the kinds still to come this zone and
    // for every zone after it - each kind's own average, not one blended zone-wide number.
    double Remaining = 0.0;
    if (LastCount > LastDone)
    {
        if (LiveRateReady) Remaining += (double)(LastCount - LastDone) / CurKindRate;
        else Remaining += KindSeconds.at(*CurKind) / (double)KindZonesSeen.at(*CurKind);
    }
    bool Past = false;
    for (Kind K : AllKinds())
    {
        if (K == *CurKind) { Past = true; continue; }
        if (!Past || !WantedKinds.count(K)) continue;
        if (KindZonesSeen.count(K)) Remaining += KindSeconds.at(K) / (double)KindZonesSeen.at(K);
    }
    const size_t ZonesLeft = (TotalZones > CurZone + 1) ? TotalZones - CurZone - 1 : 0;
    double PerZone = 0.0;
    for (Kind K : WantedKinds)
        if (KindZonesSeen.count(K)) PerZone += KindSeconds.at(K) / (double)KindZonesSeen.at(K);
    Remaining += PerZone * (double)ZonesLeft;

    if (Remaining <= 1.0) return "estimating...";
    return FormatEtaDuration(Remaining);
}

std::string RunLogPath(const Context& C)
{
    return (fs::path(C.OutRoot) / "export.log").string();
}

namespace
{
    // the run's header line: what it was asked to do
    std::string Header(const IDiskGame& Game, const ExportOptions& Options, const std::vector<std::string>& Zones, const Context& C)
    {
        std::string Kinds, Names;
        for (Kind K : C.Kinds) Kinds += (Kinds.empty() ? "" : ",") + std::string(KindName(K));
        for (size_t I = 0; I < Zones.size() && I < 8; I++) Names += (I ? ", " : "") + fs::path(Zones[I]).stem().string();
        if (Zones.size() > 8) Names += ", ...";
        std::string Text = std::string("=== export start: game ") + Game.Id() + ", " + std::to_string(Zones.size()) + " zone(s): " + Names +
                           "; kinds " + Kinds + "; out " + C.OutRoot;
        if (!C.NameFilter.empty()) Text += "; names " + C.NameFilter;
        if (C.Only) Text += "; " + std::to_string(C.Only->size()) + " picked assets";
        if (!Options.Install.empty()) Text += "; install " + Options.Install;
        if (!Options.Cache.empty()) Text += "; cache " + Options.Cache;
        if (!Options.Profile.empty()) Text += "; profile " + Options.Profile;
        for (const auto& [K, V] : Options.Settings) Text += "; " + K + "=" + V;
        return Text;
    }

    // the asset index (Totals::Index), <out>/asset_index.csv: kind,id,type,type_name,zone,name,file
    // (the file under the output root), sorted; a field holding a comma, a quote or a line break
    // quoted. The rows an earlier run wrote there stay, but for the kinds and zones this run wrote
    // (runs of one zone each build the index of them all)
    bool WriteIndex(const std::string& Root, const std::vector<Totals::IndexRow>& Rows)
    {
        using Fields = std::vector<std::string>;
        // a CSV line's fields (as ExportWriters::CsvField writes them)
        const auto Split = [](const std::string& Line) {
            Fields Out(1);
            bool InQuotes = false;
            for (size_t K = 0; K < Line.size(); K++)
            {
                const char Ch = Line[K];
                if (InQuotes && Ch == '"' && K + 1 < Line.size() && Line[K + 1] == '"') Out.back() += '"', K++;
                else if (Ch == '"') InQuotes = !InQuotes;
                else if (Ch == ',' && !InQuotes) Out.emplace_back();
                else Out.back() += Ch;
            }
            return Out;
        };
        const fs::path File = fs::path(Root) / "asset_index.csv";
        std::vector<Fields> All;
        std::set<std::pair<std::string, std::string>> Written;   // (kind, zone) of this run
        for (const auto& R : Rows)
        {
            const fs::path Rel = fs::path(R.File).lexically_relative(Root);
            All.push_back({ KindName(R.K), R.Id, R.Type, R.TypeName, R.Zone, R.Name, Rel.empty() ? R.File : Rel.generic_string() });
            Written.emplace(KindName(R.K), R.Zone);
        }
        if (std::ifstream In(File, std::ios::binary); In)
        {
            std::string Line;
            for (bool First = true; std::getline(In, Line); First = false)
            {
                if (!Line.empty() && Line.back() == '\r') Line.pop_back();
                if (First || Line.empty()) continue;
                Fields F = Split(Line);
                if (F.size() == 7 && !Written.count({ F[0], F[4] })) All.push_back(std::move(F));
            }
        }
        // by kind, zone, type name, type, name, file
        std::sort(All.begin(), All.end(), [](const Fields& A, const Fields& B) {
            return std::tie(A[0], A[4], A[3], A[2], A[5], A[6]) < std::tie(B[0], B[4], B[3], B[2], B[5], B[6]);
        });
        std::string Out = "kind,id,type,type_name,zone,name,file\n";
        for (const auto& F : All)
        {
            for (size_t K = 0; K < F.size(); K++) Out += (K ? "," : "") + ExportWriters::CsvField(F[K]);
            Out += "\n";
        }
        return ExportWriters::WriteBytes(File, (const uint8_t*)Out.data(), Out.size());
    }

    // Name fits Pattern, '*' any run of characters, without case
    bool Fits(const char* Name, const char* Pattern)
    {
        if (*Pattern == 0) return *Name == 0;
        if (*Pattern == '*') return Fits(Name, Pattern + 1) || (*Name && Fits(Name + 1, Pattern));
        return *Name && tolower((unsigned char)*Name) == tolower((unsigned char)*Pattern) && Fits(Name + 1, Pattern + 1);
    }

    // What a zone that wrote anims gives the game's AnimOwners hook (IDiskGame::AnimZone), taken
    // while it is open: the anims it exported (met, not asked of it as a dependency), then those it
    // asked for (another zone's, pulled later: BO3's mp_common names 4 000 of core_common's, its robots
    // and weapons own them), its documents the game reads
    void NoteAnimZone(const IDiskGame& Game, IDiskZone& Zone, const std::string& Path, const Selection& Met,
                      const Selection& Asked, std::vector<IDiskGame::AnimZone>& Out)
    {
        IDiskGame::AnimZone Az;
        Az.Zone = Path;
        for (const auto& A : Met)
            if (A.K == Kind::Anim && !Asked.count(A)) Az.Anims.push_back(A.Name);
        for (const auto& A : Asked)
            if (A.K == Kind::Anim) Az.Anims.push_back(A.Name);
        if (Az.Anims.empty()) return;
        if (const auto Patterns = Game.AnimOwnerDocuments(); !Patterns.empty())
        {
            std::vector<std::pair<std::string, std::string>> Docs;
            Zone.Documents(Docs);
            for (auto& D : Docs)
                for (const auto& P : Patterns)
                    if (Fits(D.first.c_str(), P.c_str()))
                    {
                        Az.Documents.push_back(std::move(D));
                        break;
                    }
        }
        Out.push_back(std::move(Az));
    }

    // The game's anim owners for the run's anims (its zones' and the dependencies it pulled), merged
    // into <out>/<game>/anims/anim_owners.json
    void WriteAnimOwners(const IDiskGame& Game, const ExportOptions& Options, const std::vector<IDiskGame::AnimZone>& Zones,
                         const Selection& Written, const Context& C, Totals& T, RunMonitor& Mon)
    {
        AnimOwners::Book Book;
        Game.AnimOwners(Options, Zones, Book);
        std::vector<std::string> Exported;
        for (const auto& A : Written)
            if (A.K == Kind::Anim) Exported.push_back(A.Name);
        Book.KeepOnly(Exported);
        if (Book.Empty()) return;
        std::string Why;
        if (!AnimOwners::Merge(C.KindDir(Kind::Anim), Game.Id(), Book, &Why))
        {
            T.Fail("", Kind::Anim, "anim_owners.json", Why);
            return;
        }
        T.Counters["anim_owners"] += Book.Anims.size();
        Mon.Log("anim owners: " + std::to_string(Book.Anims.size()) + " of " + std::to_string(Exported.size()) + " anims, " +
                std::to_string(Book.Owners.size()) + " owners (anim_owners.json)");
    }
}

Totals Export(const IDiskGame& Game, const ExportOptions& Options, const std::vector<std::string>& Zones,
              const Context& C, const ProgressFn& OnProgress, const std::atomic<bool>* Cancel)
{
    Totals T;
    Progress P;
    P.Zones = Zones.size();
    P.Running = &T;
    // the game's own Oodle, when none is beside the exe
    if (!Options.Install.empty()) Siren::AddSearchDir(fs::path(Options.Install).wstring());
    else if (!Zones.empty()) Siren::AddSearchDir(fs::path(Game.InstallOfZone(Zones.front())).wstring());
    const auto SoundsBefore = ExportWriters::SoundCounters();   // the sound writer's, this run's share added at the end
    std::error_code Ec;
    fs::create_directories(C.OutRoot, Ec);
    RunMonitor Mon(RunLogPath(C), [&](const std::string& Stalled) {
        if (!OnProgress) return;
        Progress W;
        W.FromWatchdog = true;
        W.Stalled = Stalled;
        OnProgress(W);
    });
    Mon.Log(Header(Game, Options, Zones, C));
    if (C.ResolveSeconds > 0)
    {
        Mon.Prior("resolve", C.ResolveSeconds);
        Mon.Log("zones named and pulled in " + Ms(C.ResolveSeconds));
    }
    const auto Notify = [&](std::optional<Kind> K) {
        P.Current = K;
        P.Done = P.Count = 0;
        P.Asset.clear();
        if (OnProgress) OnProgress(P);
    };
    // every asset timed; the progress callback within a kind at most every 100 ms, and its last asset
    Context Run = C;
    if (Run.Limit > 0) Run.LimitSeen = std::make_shared<Context::Limited>();
    if (Cancel != nullptr) Run.Cancel = Cancel;
    Run.Settings = Options.Settings;
    if (!Options.ActsExe.empty()) Run.Settings["acts_exe"] = Options.ActsExe;
    if (!Options.GscToolExe.empty()) Run.Settings["gsc_tool_exe"] = Options.GscToolExe;
    if (!Options.GameExe.empty()) Run.Settings["game_exe"] = Options.GameExe;
    // a decompiler not given: graywolf.json's, else on PATH
    for (const char* Tool : { "acts_exe", "gsc_tool_exe", "cod_luajit_decompiler_exe", "cod_lua_decompiler_exe", "ffmpeg_exe" })
        if (Run.Settings[Tool].empty()) Run.Settings[Tool] = SettingsManager::GetSetting(Tool, "");
    WmaSounds::UseFfmpeg(Run.Settings["ffmpeg_exe"]);
    const bool MaterialGdts = GdtMaterials.Of(Run.Settings) == "on";
    const MaterialGdt::AssetNamesInModelFiles ModToolsNames(MaterialGdts);
    auto Last = std::chrono::steady_clock::now();
    Run.Step = [&](size_t Done, size_t Count, const std::string* Asset) {
        Mon.Step(Done, Count, Asset);
        if (!OnProgress) return;
        const auto Now = std::chrono::steady_clock::now();
        if (Done < Count && Now - Last < std::chrono::milliseconds(100)) return;
        Last = Now;
        P.Done = Done;
        P.Count = Count;
        P.Asset = Mon.Asset();
        OnProgress(P);
    };
    Ledger L;
    L.Wire(Run);
    std::vector<std::function<void()>> Later;   // Context::After
    Run.OnAfter = [&Later](std::function<void()> Work) { Later.push_back(std::move(Work)); };
    Wanted Pending;
    std::string ReaderKey;
    // a model's name-only material headers, from the zones that define them (DiskGame.Materials.h)
    if (C.PullDependencies && !Zones.empty())
        Run.MaterialsElsewhere = MaterialLookup(Game, Options, !Options.Install.empty() ? Options.Install : Game.InstallOfZone(Zones.front()),
                                                ReaderKey, T);
    // what the game's readers do not export is reported unsupported; with nothing left, no zone opens
    const std::set<Kind> Declared = Game.Kinds();
    bool Any = false;
    for (Kind K : C.Kinds)
        if (Declared.count(K)) Any = true;
        else T.Unsupported.insert(K);
    const auto Cancelled = [&] { return T.Cancelled = T.Cancelled || Run.Cancelled(); };
    // the zones that wrote anims, for the game's anim owners (IDiskGame::AnimOwners)
    const bool OwnAnims = C.Kinds.count(Kind::Anim) && Declared.count(Kind::Anim);
    std::vector<IDiskGame::AnimZone> AnimZones;
    for (size_t I = 0; Any && I < Zones.size() && !Cancelled(); I++)
    {
        const auto& Path = Zones[I];
        P.Zone = I;
        P.ZoneName = fs::path(Path).stem().string();
        Notify(std::nullopt);
        T.Zones++;
        Mon.Where(P.ZoneName, "opening");
        std::string Error;
        const auto Start = std::chrono::steady_clock::now();
        auto Zone = Game.Open(Path, Options, Error);
        const double OpenS = Seconds(Start);
        Mon.AddTime("open", OpenS);
        if (!Zone)
        {
            Mon.Log("zone " + P.ZoneName + " (" + std::to_string(I + 1) + "/" + std::to_string(Zones.size()) + ") did not open after " +
                    Ms(OpenS) + ": " + Error);
            T.ZonesFailed++;
            T.ZoneErrors.push_back(fs::path(Path).filename().string() + ": " + Error);
            continue;
        }
        Mon.Log("zone " + P.ZoneName + " (" + std::to_string(I + 1) + "/" + std::to_string(Zones.size()) + ") " + Opened(*Zone, OpenS, Mon, "open_"));
        if (Zone->WalkedToEnd()) T.ZonesWalked++;
        if (ReaderKey.empty()) ReaderKey = Zone->IndexKey();
        for (Kind K : AllKinds())
        {
            if (!C.Kinds.count(K) || !Declared.count(K) || Cancelled()) continue;
            Notify(K);
            T.Kinds[K];
            Mon.Where(P.ZoneName, KindName(K));
            const KindMeter Meter(T, K);
            const bool Supported = Zone->Export(K, Run, T);
            Mon.EndAsset();
            if (!Supported)
            {
                // declared by the game, yet this zone's reader has no exporter for it
                T.Fail(P.ZoneName, K, "", "the game declares this kind but its reader has no exporter");
                continue;
            }
            const KindSample S = Meter.Stop(T);
            Mon.AddKind(KindName(K), S);
            Mon.Log("  " + P.ZoneName + " " + KindName(K) + ": " + S.Describe());
        }
        Zone->Measures(T);   // after the exports, which measure too
        if (OwnAnims && !T.Cancelled) NoteAnimZone(Game, *Zone, Path, L.Met, L.Asked, AnimZones);
        const auto Closing = std::chrono::steady_clock::now();
        Zone.reset();        // one zone open at a time
        Mon.AddTime("close", Seconds(Closing));
        for (const auto& A : L.Settle()) Pending[A].insert(Stem(Path));
    }
    // a kind no zone could export is reported as unsupported, not as zero
    for (Kind K : T.Unsupported)
        if (T.Kinds.count(K) && T.Kinds[K].Seen == 0) T.Kinds.erase(K);

    // what a later zone exported is no dependency; the rest comes from the install
    for (auto It = Pending.begin(); It != Pending.end();)
        It = L.Exported.count(It->first) ? Pending.erase(It) : std::next(It);
    if (C.PullDependencies && !Pending.empty() && !Cancelled())
    {
        const std::string Install = !Options.Install.empty() ? Options.Install : Game.InstallOfZone(Zones.front());
        P.Zone = Zones.size() - 1;
        const auto Start = std::chrono::steady_clock::now();
        PullDependencies(Game, Options, Install, ReaderKey, std::move(Pending), Run, L, T, P, OnProgress, Cancelled, Mon);
        Mon.AddTime("deps_total", Seconds(Start));
    }
    if (!Later.empty())
    {
        const auto Start = std::chrono::steady_clock::now();
        for (size_t K = 0; K < Later.size(); K++)
        {
            auto Work = std::move(Later[K]);   // Work may queue more
            // what it writes counts for its kind (the weapon stats table, built once every zone's
            // facts are in): metered like a zone's exporter, logged when it changed a kind's count
            std::vector<std::pair<Kind, KindMeter>> Meters;
            for (const auto& [Which, V] : T.Kinds) Meters.emplace_back(Which, KindMeter(T, Which));
            Work();
            for (const auto& [Which, Meter] : Meters)
            {
                const KindSample S = Meter.Stop(T);
                if (S.Seen == 0 && S.Written == 0) continue;
                Mon.AddKind(KindName(Which), S);
                Mon.Log("  after " + std::string(KindName(Which)) + ": " + S.Describe());
            }
        }
        Mon.AddTime("after", Seconds(Start));
    }
    // which models the anims play on, as the game says, once per run (no zone open)
    if (!AnimZones.empty() && !T.Cancelled)
    {
        const auto Start = std::chrono::steady_clock::now();
        WriteAnimOwners(Game, Options, AnimZones, L.Exported, Run, T, Mon);
        Mon.AddTime("anim_owners", Seconds(Start));
    }
    // the asset index, once per run, of what the data kinds wrote
    if (!T.Indexed.empty() && !WriteIndex(C.OutRoot, T.Indexed)) T.Fail("", T.Indexed.front().K, "asset_index.csv", "write failed");
    // the models' material GDT, once their sidecars and textures are all written
    if (MaterialGdts && C.Kinds.count(Kind::Model) && !T.Cancelled)
    {
        const auto Start = std::chrono::steady_clock::now();
        MaterialGdt::WriteForModels(Run, T);
        Mon.AddTime("material_gdt", Seconds(Start));
    }
    for (const auto& [Name, N] : ExportWriters::SoundCounters())
    {
        const auto Was = SoundsBefore.find(Name);
        const size_t Here = N - (Was == SoundsBefore.end() ? 0 : Was->second);
        if (Here) T.Counters[Name] += Here;
    }
    if (const auto It = T.Counters.find(WmaSounds::NoFfmpegCounter); It != T.Counters.end()) Mon.Log(WmaSounds::NoFfmpegMessage(It->second));
    if (T.Cancelled) Mon.Log("cancelled after " + Ms(Mon.Elapsed()));
    size_t Written = 0;
    for (const auto& [K, V] : T.Kinds) Written += V.Written;
    Mon.Log("=== export end: " + std::to_string(T.ZonesWalked) + "/" + std::to_string(T.Zones) + " zones read to their end, " +
            std::to_string(Written) + " files written, " + std::to_string(T.Failures.size()) + " failures" +
            (T.Cancelled ? ", cancelled" : ""));
    Mon.Summarize(T);
    return T;
}
}
