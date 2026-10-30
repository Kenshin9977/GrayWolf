// The picked game's zones (IDiskGame::ListZones) shown by map, shared content and language, the
// export bar both tabs share, and the workers: the zone list, the install index, the export, the
// picked zones (or the picked assets' zones) resolved (ResolveZones) and exported by
// DiskGame::Export, the use case disk-export runs on the command line.

#include "stdafx.h"

#include "ImGuiLoadGamePanel.h"
#include "ImGuiLoadGamePanel.Common.h"
#include "ImGuiDialogs.h"
#include "ImGuiBlender.h"
#include "AppLog.h"
#include "DiskGame.h"
#include "ExportNaming.h"
#include "SettingsManager.h"
#include "Strings.h"
#include "WraithX.h"

#include "imgui.h"
#include "imgui_internal.h"   // OpenPopupEx: the Languages list dropped down by ui-record

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;
using namespace LoadGameUi;

namespace
{
    using ExportNaming::Thousands;

    const ImVec4 Amber(1.0f, 0.75f, 0.2f, 1.0f), Red(1.0f, 0.35f, 0.3f, 1.0f);

    // A zone named for one language's text and voices: its first word a language ("en_", "french_")
    // or a rating-safe variant of one ("arasafe_", "k15safe_")
    bool LanguageZone(const std::string& Zone)
    {
        static const char* const Languages[] = {
            "en", "fr", "de", "ge", "es", "it", "ru", "pl", "ja", "jp", "ko", "kr", "zh", "cn", "tw", "pt", "br", "ar", "cz",
            "ea", "mx", "nl", "sa", "english", "french", "german", "spanish", "italian", "russian", "polish", "japanese",
            "korean", "chinese", "portuguese", "arabic", "czech" };
        const std::string Name = fs::path(Zone).filename().string();
        const size_t End = Name.find_first_of("_.");
        if (End == std::string::npos) return false;
        std::string First = Name.substr(0, End);
        for (char& C : First) C = (char)tolower((unsigned char)C);
        if (First.size() > 4 && First.compare(First.size() - 4, 4, "safe") == 0) return true;
        for (const char* L : Languages)
            if (First == L) return true;
        return false;
    }

    // Each zone's group (see DiskZoneGroups): a level's zones share its name, the shortest is the map
    std::vector<int> ZoneGroups(const std::vector<std::string>& Names, const std::vector<std::string>& Levels)
    {
        std::map<std::string, size_t> Base;   // level name -> its shortest zone
        for (size_t i = 0; i < Names.size(); i++)
        {
            if (Levels[i].empty() || LanguageZone(Names[i])) continue;
            const auto It = Base.find(Levels[i]);
            if (It == Base.end() || Names[i].size() < Names[It->second].size()) Base[Levels[i]] = i;
        }
        std::vector<int> Groups(Names.size(), 2);
        for (size_t i = 0; i < Names.size(); i++)
        {
            const auto It = Levels[i].empty() ? Base.end() : Base.find(Levels[i]);
            if (It != Base.end() && It->second == i) Groups[i] = 1;
            else if (!Levels[i].empty() || LanguageZone(Names[i])) Groups[i] = 3;
        }
        return Groups;
    }

    const char* const GroupNames[] = { "All", "Maps", "Shared content", "Languages & map variants" };

    // "1 204 models · 2 810 images · 96 sounds": the three kinds a zone holds most of
    std::string Holds(const std::map<DiskGame::Kind, size_t>& Counts)
    {
        std::vector<std::pair<size_t, DiskGame::Kind>> ByCount;
        for (const auto& [K, N] : Counts) ByCount.emplace_back(N, K);
        std::sort(ByCount.begin(), ByCount.end(), [](const auto& A, const auto& B) { return A.first > B.first; });
        std::string Text;
        for (size_t i = 0; i < ByCount.size() && i < 3; i++) Text += (Text.empty() ? "" : "  ·  ") + KindCount(ByCount[i].second, ByCount[i].first);
        return Text;
    }

    // A coloured box around what follows (EndBanner closes it)
    void BeginBanner(const char* Id, const ImVec4& Fill, const ImVec4& Border)
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Fill);
        ImGui::PushStyleColor(ImGuiCol_Border, Border);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
        ImGui::BeginChild(Id, ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    }

    void EndBanner()
    {
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }
}

void LoadGamePanel::ChooseDiskGame()
{
    // not while the folder is typed: a half-typed "E:\" would be walked for its fast files
    Disk = nullptr;
    if (GameEditing) return;
    Detecting = false;
    if (GameBuf[0] != 0 && SettingsManager::GetSetting("disk_reader_for", "") == GameBuf
        && (Disk = DiskGame::Find(SettingsManager::GetSetting("disk_reader", ""))) != nullptr)
    {
        DiskChosenFor = GameBuf;
        return;
    }
    // a detected install names its reader: the HQ folder holds several games, its first fast file
    // would tell only one of them
    if (GameBuf[0] != 0 && !InstallDiskId.empty() && InstallPath == GameBuf
        && (Disk = DiskGame::Find(InstallDiskId)) != nullptr)
    {
        DiskChosenFor = GameBuf;
        return;
    }
    if (GameBuf[0] != 0)
    {
        // asked again each frame until the worker's answer is in (DiskChosenFor still differs)
        const std::string Folder = GameBuf;
        Disk = Detected.Get(Folder, 1e30, [Folder] { return DiskGame::Detect(Folder); }, &Detecting);
        if (Detecting) return;
    }
    DiskChosenFor = GameBuf;
    // a folder with no fast file to tell (CoD1, CoD2: zips): the detected install names its reader
    if (Disk == nullptr && GameBuf[0] != 0 && !InstallDiskId.empty()) Disk = DiskGame::Find(InstallDiskId);
}

void LoadGamePanel::DrawReadWith()
{
    if (DiskChosenFor != GameBuf) ChooseDiskGame();
    ImGui::TextUnformatted("Read with");
    ImGui::SetNextItemWidth(-90);
    if (!ImGui::BeginCombo("##readwith", Disk ? Disk->Title() : "select...")) return;
    const DiskGame::IDiskGame* Picked = nullptr;
    for (const DiskGame::IDiskGame* G : DiskGame::Games())
    {
        ImGui::PushID(G->Id());
        if (ImGui::Selectable(G->Title(), Disk == G)) Picked = G;
        ImGui::PopID();
    }
    ImGui::EndCombo();
    if (Picked == nullptr) return;
    Disk = Picked;
    SettingsManager::SetSetting("disk_reader_for", GameBuf);
    SettingsManager::SetSetting("disk_reader", Disk->Id());
}

// Worker: the game's zone names, the saved ticks applied, the levels' names (read once from the
// game's strings, then kept) and the asset counts of an install index already built.
void LoadGamePanel::DoListDiskZones()
{
    WraithX::InitializeAPI();
    const DiskGame::IDiskGame* Game = Disk;
    const std::string Install = GameBuf;
    {
        std::lock_guard<std::mutex> Lk(Mutex);
        Status = "reading the zone list\xE2\x80\xA6";
        // another game's index is not this one's
        DiskIndex.reset();
        DiskAssets.reset();
        DiskIndexDate.clear();
    }
    std::vector<std::string> Names;
    std::string Error;
    const DiskGame::ExportOptions Options = DiskOptions(*Game, Install);
    const bool Ok = Game->ListZones(Install, Options, Names, Error);
    const std::string Saved = "," + SettingsManager::GetSetting(std::string("disk_zones_") + Game->Id(), "") + ",";

    std::vector<std::string> Levels(Names.size());
    size_t Named = 0;
    if (Ok)
    {
        std::string Why;
        const auto Step = [&](size_t Done, size_t Count) {
            std::lock_guard<std::mutex> Lk(Mutex);
            Status = "reading the map names from the game's text (" + std::to_string(Done) + "/" + std::to_string(Count) + " zones)\xE2\x80\xA6";
        };
        if (const auto Found = DiskGame::LevelNames::For(*Game, Options, Install, Step, Why))
            for (size_t i = 0; i < Names.size(); i++) Named += !(Levels[i] = Found->Of(Names[i])).empty();
        else if (!Why.empty())
            AppLog::Log().warn("load game: no level names for {}: {}", Game->Id(), Why);
    }

    {
        std::lock_guard<std::mutex> Lk(Mutex);
        DiskZoneGroups = ZoneGroups(Names, Levels);
        DiskZoneNames = std::move(Names);
        DiskZoneLevels = std::move(Levels);
        DiskZoneFlags.assign(DiskZoneNames.size(), 0);
        for (size_t i = 0; i < DiskZoneNames.size(); i++)
            DiskZoneFlags[i] = Saved.find("," + DiskZoneNames[i] + ",") != std::string::npos;
        DiskZonesFor = ListedFor(*Game, Options);
        Status = Ok ? std::to_string(DiskZoneNames.size()) + " zones" + (Named ? ", " + std::to_string(Named) + " named maps" : "")
                    : Error;
    }
    ReadZoneCounts(*Game, Options, Install);
}

void LoadGamePanel::ReadZoneCounts(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& Options, const std::string& Install)
{
    auto Ix = DiskGame::ZoneIndex::Kept(Game, Options, Install);
    std::vector<std::map<DiskGame::Kind, size_t>> Counts(Ix ? Ix->Zones().size() : 0);
    std::vector<uint64_t> Footprints(Counts.size());
    for (size_t Z = 0; Z < Counts.size(); Z++)
    {
        for (const auto& A : Ix->Defined(Z)) Counts[Z][A.K]++;
        Footprints[Z] = Ix->Footprint(Z);
    }
    // the size samples measured last time for this install, if any
    DiskGame::Sizes::Calibration Sizes;
    if (Ix) DiskGame::Sizes::Load(DiskGame::KeptFile(Game, Options, Install, ".sizes"), Sizes);
    bool Counted = false;
    {
        std::lock_guard<std::mutex> Lk(Mutex);
        DiskZoneCounted = Counted = Ix && Ix->Zones() == DiskZoneNames;
        if (DiskZoneCounted)
        {
            DiskZoneCounts = std::move(Counts);
            DiskZoneFootprints = std::move(Footprints);
            DiskSizes = std::move(Sizes);
        }
        else
        {
            DiskZoneCounts.assign(DiskZoneNames.size(), {});
            DiskZoneFootprints.assign(DiskZoneNames.size(), 0);
            DiskSizes = {};
        }
    }
    // the asset tab searches it
    KeepIndex(Counted ? std::move(Ix) : nullptr);
}

// Worker: the picked zones' kinds sampled (the ticked ones and those exported by default, so that
// ticking one later needs no new sample), measured into DiskSizes and kept beside the index.
void LoadGamePanel::MeasureSizes(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& Options, const std::string& Install,
                                 const DiskGame::ZoneIndex& Ix, const std::set<DiskGame::Kind>* Only)
{
    std::vector<size_t> Picked;
    std::set<size_t> Levels;
    std::set<DiskGame::Kind> Kinds;
    {
        std::lock_guard<std::mutex> Lk(Mutex);
        for (size_t Z = 0; Z < DiskZoneNames.size() && Z < Ix.Zones().size(); Z++)
            if (DiskZoneFlags[Z] && Ix.Zones()[Z] == DiskZoneNames[Z])
            {
                Picked.push_back(Z);
                if (Game.LevelNames() != nullptr && !DiskZoneLevels[Z].empty()) Levels.insert(Z);
            }
        if (Only != nullptr) Kinds = *Only;
        else
            for (size_t K = 0; K < DiskKindFlags.size(); K++)
                if (DiskKindFlags[K] || DiskGame::Kinds[K].ByDefault) Kinds.insert(DiskGame::Kinds[K].Id);
    }
    const auto Start = std::chrono::steady_clock::now();
    size_t Done = 0;
    DiskGame::Sizes::Calibration Kept;   // the sounds' headers read by an earlier count, not read again
    {
        std::lock_guard<std::mutex> Lk(Mutex);
        Kept = DiskSizes;
    }
    const DiskGame::Sizes::Calibration Sizes = DiskGame::Sizes::Measure(Game, Options, Install, Ix, Picked, Levels, Kinds, [&](const std::string& What) {
        std::lock_guard<std::mutex> Lk(Mutex);
        DiskFraction = std::min(1.0f, (float)Done++ / (float)(Kinds.size() + 1));
        DiskStep = "size estimate: " + What;
    }, &DiskCancel, &Kept);
    if (DiskCancel.load()) return;
    DiskGame::Sizes::Save(DiskGame::KeptFile(Game, Options, Install, ".sizes"), Sizes);
    std::lock_guard<std::mutex> Lk(Mutex);
    DiskSizes = Sizes;
    Status += ", sizes sampled in " + Clock(std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count());
}

// Worker: the install index, built as an export pulling dependencies builds it, then the counts.
void LoadGamePanel::DoIndexDiskZones()
{
    WraithX::InitializeAPI();
    const DiskGame::IDiskGame& Game = *Disk;
    const std::string Install = GameBuf;
    const DiskGame::ExportOptions Options = DiskOptions(Game, Install);
    {
        std::lock_guard<std::mutex> Lk(Mutex);
        DiskFraction = 0.0f;
        DiskStep = "opening the first zone...";
        Status = DiskStep;
        LastRun = {};
    }
    std::string Error;
    const auto Start = std::chrono::steady_clock::now();
    const auto Ix = DiskGame::ZoneIndex::Build(Game, Options, Install, [&](size_t Done, size_t Count) {
        std::lock_guard<std::mutex> Lk(Mutex);
        DiskFraction = Count ? (float)Done / (float)Count : 0.0f;
        DiskStep = "listing what each zone holds: zone " + std::to_string(std::min(Done + 1, Count)) + "/" + std::to_string(Count);
        Status = DiskStep;
    }, &DiskCancel, Error);
    if (Ix) ReadZoneCounts(Game, Options, Install);
    {
        std::lock_guard<std::mutex> Lk(Mutex);
        DiskFraction = 1.0f;
        Status = Ix ? "install index: " + Thousands(Ix->Entries) + " assets in " + std::to_string(Ix->Zones().size()) + " zones, " +
                          (Ix->Built ? "built in " : "read in ") +
                          Clock(std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count())
                    : "install index not built: " + Error;
        // a build that failed says why where the export's result would be
        if (!Ix && !DiskCancel.load())
        {
            LastRun.Has = true;
            LastRun.Headline = "The install could not be indexed";
            LastRun.ZoneErrors = { Error };
        }
    }
}

void LoadGamePanel::DoMeasureMissing()
{
    WraithX::InitializeAPI();
    const DiskGame::IDiskGame& Game = *Disk;
    const std::string Install = GameBuf;
    const DiskGame::ExportOptions Options = DiskOptions(Game, Install);
    std::shared_ptr<const DiskGame::ZoneIndex> Ix;
    {
        std::lock_guard<std::mutex> Lk(Mutex);
        Ix = DiskIndex;
        DiskFraction = 0.0f;
        DiskStep = "size estimate: measuring a sample...";
    }
    if (Ix) MeasureSizes(Game, Options, Install, *Ix, &MeasureMissing);
}

// Worker: the picked zones resolved (or the zones defining the picked assets, the run then limited
// to them), then DiskGame::Export with the progress and the cancel flag.
void LoadGamePanel::DoDiskExport()
{
    WraithX::InitializeAPI();
    const DiskGame::IDiskGame& Game = *Disk;
    const std::string Install = GameBuf;
    const DiskGame::ExportOptions Options = DiskOptions(Game, Install);
    DiskGame::Context C;
    C.OutRoot = (fs::path(AbsoluteFolder(DiskOutBuf)) / Game.Id()).string();
    if (ExportOnly)
    {
        C.Only = ExportOnly;
        C.Kinds = ExportKinds;
    }
    else
    {
        if (Game.FiltersByName()) C.NameFilter = DiskNameFilterBuf;
        const std::set<DiskGame::Kind> Exported = Game.Kinds();
        for (size_t i = 0; i < DiskKindFlags.size(); i++)
            if (DiskKindFlags[i] && Exported.count(DiskGame::AllKinds()[i])) C.Kinds.insert(DiskGame::AllKinds()[i]);
    }
    {
        std::lock_guard<std::mutex> Lk(Mutex);
        Log.clear();
        LastRun = {};
        DiskStall.clear();
        DiskFraction = 0.0f;
        DiskStep = "preparing " + std::to_string(DiskZonesToExport.size()) + " zone(s)...";
        Status = DiskStep;
        DiskEta.Reset(C.Kinds);
        DiskEtaText = "estimating...";
    }
    std::vector<std::string> Zones;
    std::string Error;
    const auto Start = std::chrono::steady_clock::now();
    const bool Resolved = Game.ResolveZones(Install, DiskZonesToExport, Options, Zones, Error);
    C.ResolveSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
    if (!Resolved)
    {
        AppLog::Log().warn("disk export: the picked zones did not resolve: {}", Error);
        std::lock_guard<std::mutex> Lk(Mutex);
        Status = Error;
        LastRun.Has = true;
        LastRun.Headline = "The picked zones were not found";
        LastRun.ZoneErrors = { Error };
        return;
    }

    const auto OnProgress = [&](const DiskGame::Progress& P) {
        if (P.FromWatchdog)
        {
            std::lock_guard<std::mutex> Lk(Mutex);
            DiskStall = P.Stalled;
            return;
        }
        // a zone's share of the bar: reading it, then each kind in turn, filled as its assets go
        const size_t Kinds = C.Kinds.size();
        const size_t Step = P.Current ? (size_t)std::distance(C.Kinds.begin(), C.Kinds.find(*P.Current)) + 1 : 0;
        const float Within = P.Count ? (float)P.Done / (float)P.Count : 0.0f;
        size_t Written = 0;
        for (const auto& [K, V] : P.Running->Kinds) Written += V.Written;
        std::string Where = P.Current ? DiskGame::KindFolder(*P.Current) : std::string("reading");
        if (P.Count) Where += " " + Thousands(P.Done) + "/" + Thousands(P.Count);
        if (!P.Asset.empty()) Where += " " + P.Asset;
        std::lock_guard<std::mutex> Lk(Mutex);
        DiskFraction = ((float)(P.Zone * (Kinds + 1) + Step) + Within) / (float)(P.Zones * (Kinds + 1));
        DiskStep = P.ZoneName + " (" + std::to_string(P.Zone + 1) + "/" + std::to_string(P.Zones) + "): "
                   + Where + ", " + Thousands(Written) + " files written";
        Status = DiskStep;
        DiskEta.Observe(P);
        DiskEtaText = DiskEta.Text();
    };
    const DiskGame::Totals T = DiskGame::Export(Game, Options, Zones, C, OnProgress, &DiskCancel);
    // pulling dependencies may have built the install index: its counts
    if (!DiskZoneCounted) ReadZoneCounts(Game, Options, Install);

    size_t Written = 0;
    for (const auto& [K, V] : T.Kinds) Written += V.Written;
    const double Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
    std::lock_guard<std::mutex> Lk(Mutex);
    std::istringstream Report(DiskGame::Report(T));
    for (std::string Line; std::getline(Report, Line);) Log.push_back(Line);
    Status = std::string(T.Cancelled ? "cancelled: " : "") + std::to_string(T.ZonesWalked) + "/" + std::to_string(T.Zones)
             + " zones read to their end, " + std::to_string(Written) + " files written to " + C.OutRoot + " in " + Clock(Seconds);
    // the banners: what was written, then what did not go
    LastRun.Has = true;
    LastRun.Cancelled = T.Cancelled;
    LastRun.Written = Written;
    LastRun.Failures = T.Failures.size();
    LastRun.ZoneErrors = T.ZoneErrors;
    LastRun.OutDir = C.OutRoot;
    LastRun.RunLog = DiskGame::RunLogPath(C);
    const std::string What = C.Only ? Thousands(C.Only->size()) + (C.Only->size() == 1 ? " asset" : " assets")
                                    : std::to_string(T.ZonesWalked) + (T.ZonesWalked == 1 ? " zone" : " zones");
    size_t Pulled = 0;
    for (const auto& [K, N] : T.Pulled) Pulled += N;
    LastRun.Headline = std::string(T.Cancelled ? "Stopped: " : "") + What + " exported"
                       + (Pulled ? ", with " + Thousands(Pulled) + " dependencies" : std::string());
    std::string Kinds;
    for (const auto& [K, V] : T.Kinds)
        if (V.Written) Kinds += (Kinds.empty() ? "" : ", ") + KindCount(K, V.Written);
    LastRun.Detail = (Kinds.empty() ? std::string("Nothing written") : Kinds + " written") + " in " + Clock(Seconds) + "  \xC2\xB7  " + C.OutRoot;
    DiskStall.clear();
    DiskFraction = 1.0f;
    DiskExportedTo = C.OutRoot;
    if (Written > 0) DiskJustFinished.store(true);
}

bool LoadGamePanel::Working()
{
    if (Busy.load() || Detecting || !ScannedOnce) return true;
    if (Disk == nullptr) return false;
    std::lock_guard<std::mutex> Lock(Mutex);
    return DiskZonesFor != ListedFor(*Disk, DiskOptions(*Disk, GameBuf));
}

// The zone list, read for the game, folder and settings shown (a worker reads it when missing)
bool LoadGamePanel::ZonesListed()
{
    bool Listed = false;
    {
        std::lock_guard<std::mutex> Lock(Mutex);
        Listed = DiskZonesFor == ListedFor(*Disk, DiskOptions(*Disk, GameBuf));
    }
    if (Listed && !(Busy.load() && BusyLabel == ListLabel)) return true;
    if (!Busy.load()) StartWork(ListLabel, &LoadGamePanel::DoListDiskZones);
    return false;
}

void LoadGamePanel::DrawZoneTab()
{
    if (!ZonesListed())
    {
        std::lock_guard<std::mutex> Lock(Mutex);
        DrewIndicator = true;
        Loading("Reading " + std::string(Disk->Title()) + ": " + Status);
        return;
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("A zone is one of the game's packages: a map, content the modes share, or one language's text and voices. "
                        "Exporting a zone writes all it holds of the kinds ticked in Formats & options.");
    ImGui::PopTextWrapPos();

    // the filter, the groups with their counts, and the index when the counts are missing
    bool Counted = false;
    size_t InGroup[4] = {};
    {
        std::lock_guard<std::mutex> Lock(Mutex);
        Counted = DiskZoneCounted && DiskZoneCounts.size() == DiskZoneNames.size();
        InGroup[0] = DiskZoneNames.size();
        for (size_t i = 0; i < DiskZoneGroups.size(); i++) InGroup[DiskZoneGroups[i]]++;
    }
    const float Buttons = ImGui::CalcTextSize("Rescan").x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetNextItemWidth(-Buttons);
    ImGui::InputTextWithHint("##diskzonefilter", "Filter by map or zone name, e.g. nuketown", DiskZoneFilterBuf, sizeof(DiskZoneFilterBuf));
    if (ImGui::IsItemDeactivatedAfterEdit()) Persist();
    ImGui::SameLine();
    if (ImGui::Button("Rescan##diskzones")) StartWork(ListLabel, &LoadGamePanel::DoListDiskZones);
    for (int G = 0; G < 4; G++)
    {
        if (G == 1 && InGroup[1] == 0) continue;   // a game that names no level
        if (G > 0) ImGui::SameLine();
        const bool On = ZoneGroup == G;
        if (!On) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
        const std::string Label = std::string(GroupNames[G]) + "  " + Thousands(InGroup[G]) + "##zonegroup" + std::to_string(G);
        if (ImGui::Button(Label.c_str())) ZoneGroup = G;
        if (!On) ImGui::PopStyleColor();
    }
    if (!Counted)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("  Asset counts per zone:");
        ImGui::SameLine();
        if (ImGui::SmallButton("Index the install"))
        {
            DiskCancel.store(false);
            DiskStartedAt = ImGui::GetTime();
            StartWork(IndexLabel, &LoadGamePanel::DoIndexDiskZones);
        }
    }

    std::vector<uint32_t> Visible;
    {
        std::lock_guard<std::mutex> Lock(Mutex);
        for (uint32_t i = 0; i < (uint32_t)DiskZoneNames.size(); i++)
            if ((ZoneGroup == 0 || DiskZoneGroups[i] == ZoneGroup) &&
                (ContainsNoCase(DiskZoneNames[i], DiskZoneFilterBuf) || ContainsNoCase(DiskZoneLevels[i], DiskZoneFilterBuf)))
                Visible.push_back(i);
    }
    DrawZoneTable(Visible);

    const size_t Selected = (size_t)std::count(DiskZoneFlags.begin(), DiskZoneFlags.end(), 1);
    ImGui::TextDisabled("%s / %s zones picked", Thousands(Selected).c_str(), Thousands(DiskZoneNames.size()).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Pick all shown##diskzones"))
    {
        for (const auto Idx : Visible) DiskZoneFlags[Idx] = 1;
        Persist();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear##diskzones"))
    {
        std::fill(DiskZoneFlags.begin(), DiskZoneFlags.end(), 0);
        Persist();
    }
}

void LoadGamePanel::DrawZoneTable(std::vector<uint32_t>& Visible)
{
    // the counts are rewritten by a worker (an index built, an export's)
    std::lock_guard<std::mutex> Lock(Mutex);
    const ImGuiTableFlags Flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
    const float Height = std::max(ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing(), ImGui::GetTextLineHeight() * 6);
    if (!ImGui::BeginTable("diskzones", 4, Flags, ImVec2(0, Height))) return;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 1.0f));   // rows a line tall
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Map / content", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort, 2.2f);
    ImGui::TableSetupColumn("Zone", ImGuiTableColumnFlags_WidthStretch, 1.8f);
    ImGui::TableSetupColumn("Holds", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_PreferSortDescending, 2.6f);
    ImGui::TableSetupColumn("On disk", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending,
                            ImGui::CalcTextSize("On disk").x + ImGui::GetFontSize() * 2.5f);
    ImGui::TableHeadersRow();
    if (ImGuiTableSortSpecs* Specs = ImGui::TableGetSortSpecs(); Specs != nullptr && Specs->SpecsCount > 0)
    {
        DiskZoneSortColumn = Specs->Specs[0].ColumnIndex;
        DiskZoneSortDescending = Specs->Specs[0].SortDirection == ImGuiSortDirection_Descending;
        Specs->SpecsDirty = false;
    }
    const bool Counted = DiskZoneCounted && DiskZoneCounts.size() == DiskZoneNames.size();
    // the maps first, then the shared content, then the languages; in each by the column picked
    const auto Label = [&](uint32_t Z) -> const std::string& { return DiskZoneGroups[Z] == 1 ? DiskZoneLevels[Z] : DiskZoneNames[Z]; };
    std::stable_sort(Visible.begin(), Visible.end(), [&](uint32_t A, uint32_t B) {
        const int Ga = DiskZoneGroups[A], Gb = DiskZoneGroups[B];
        if (Ga != Gb) return Ga < Gb;
        if (Counted && DiskZoneSortColumn == 2 && Total(DiskZoneCounts[A]) != Total(DiskZoneCounts[B]))
            return (Total(DiskZoneCounts[A]) < Total(DiskZoneCounts[B])) != DiskZoneSortDescending;
        if (Counted && DiskZoneSortColumn == 3 && DiskZoneFootprints[A] != DiskZoneFootprints[B])
            return (DiskZoneFootprints[A] < DiskZoneFootprints[B]) != DiskZoneSortDescending;
        const int Order = DiskZoneSortColumn == 1 ? _stricmp(DiskZoneNames[A].c_str(), DiskZoneNames[B].c_str())
                                                  : _stricmp(Label(A).c_str(), Label(B).c_str());
        return DiskZoneSortDescending && DiskZoneSortColumn < 2 ? Order > 0 : Order < 0;
    });
    // a group's header row before its first zone (-1 - group), when every group is shown
    std::vector<int64_t> Rows;
    Rows.reserve(Visible.size() + 3);
    int Last = 0;
    for (const uint32_t Z : Visible)
    {
        const int G = DiskZoneGroups[Z];
        if (ZoneGroup == 0 && G != Last) Rows.push_back(-1 - G);
        Last = G;
        Rows.push_back(Z);
    }

    ImGuiListClipper Clipper;
    Clipper.Begin((int)Rows.size());
    while (Clipper.Step())
        for (int Row = Clipper.DisplayStart; Row < Clipper.DisplayEnd; Row++)
        {
            ImGui::TableNextRow();
            if (Rows[Row] < 0)
            {
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(ImVec4(0.62f, 0.71f, 0.83f, 1.0f), "%s", GroupNames[-1 - Rows[Row]]);
                continue;
            }
            const uint32_t Idx = (uint32_t)Rows[Row];
            ImGui::PushID((int)Idx);
            ImGui::TableSetColumnIndex(0);
            bool Checked = DiskZoneFlags[Idx] != 0;
            if (ImGui::Checkbox(Label(Idx).c_str(), &Checked)) { DiskZoneFlags[Idx] = Checked; Persist(); }
            ImGui::TableSetColumnIndex(1);
            if (DiskZoneGroups[Idx] == 1 || DiskZoneLevels[Idx].empty()) ImGui::TextDisabled("%s", DiskZoneNames[Idx].c_str());
            else ImGui::TextDisabled("of %s", DiskZoneLevels[Idx].c_str());   // a variant: its name is in the first column
            ImGui::TableSetColumnIndex(2);
            if (!Counted)
                ImGui::TextDisabled("\xE2\x80\x93");   // not indexed yet
            else
            {
                const auto& Counts = DiskZoneCounts[Idx];
                ImGui::TextDisabled("%s", Counts.empty() ? "\xE2\x80\x93" : Holds(Counts).c_str());
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !Counts.empty())
                {
                    std::string Tip;
                    for (const auto& [K, N] : Counts) Tip += (Tip.empty() ? "" : "\n") + KindCount(K, N);
                    ImGui::SetTooltip("%s\n(from the install index: the assets stored in this zone)", Tip.c_str());
                }
            }
            ImGui::TableSetColumnIndex(3);
            if (Counted) ImGui::TextDisabled("%s", DiskGame::Sizes::Text(DiskZoneFootprints[Idx]).c_str());
            ImGui::PopID();
        }
    ImGui::PopStyleVar();
    ImGui::EndTable();
}

// The output folder, the kinds a zone export writes, the game's settings, the name filter
void LoadGamePanel::DrawOptions()
{
    const std::string Target = (fs::path(AbsoluteFolder(DiskOutBuf)) / Disk->Id()).string();
    // compact: small frames and checkboxes, the rows close together
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 2.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 3.0f));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Output");
    ImGui::SameLine();
    const float BrowseW = ImGui::CalcTextSize("Browse").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetNextItemWidth(-(BrowseW + ImGui::GetStyle().ItemSpacing.x));
    ImGui::InputText("##diskoutdir", DiskOutBuf, sizeof(DiskOutBuf));
    if (ImGui::IsItemDeactivatedAfterEdit()) Persist();
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) ImGui::SetTooltip("Exports go to %s", Target.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Browse\xE2\x80\xA6##diskout"))
        UiAsync::PickFolder("loadgame.output", "Select where to export to", AbsoluteFolder(DiskOutBuf));
    // the last runs' log: phases timed, slowest assets, stalls (DiskGame::Export)
    DiskGame::Context Out;
    Out.OutRoot = Target;
    const std::string RunLog = DiskGame::RunLogPath(Out);
    // the output folder may be on a slow or sleeping drive: looked up on a worker, the frame
    // reads the last answer (refreshed every few seconds, at once after an export)
    if (DiskJustFinished.load()) OutputState.Refresh();
    const auto [HasTarget, HasRunLog] = OutputState.Get(Target, 3.0, [Target, RunLog] {
        std::error_code Ec;
        return std::make_pair(fs::is_directory(Target, Ec), fs::exists(RunLog, Ec));
    });
    // the folder's buttons, its full path in their tooltip
    const auto PathTip = [](const std::string& Path) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", Path.c_str());
    };
    ImGui::BeginDisabled(!HasTarget);
    if (ImGui::SmallButton("Open folder##diskout")) UiAsync::OpenInShell(Target);
    ImGui::EndDisabled();
    PathTip(Target);
    ImGui::SameLine();
    ImGui::BeginDisabled(!HasRunLog);
    if (ImGui::SmallButton("Open export log##diskout")) UiAsync::OpenInShell(RunLog);
    ImGui::EndDisabled();
    PathTip(RunLog);
    ImGui::SameLine();
    if (ImGui::SmallButton("Session log##diskout")) UiAsync::OpenInShell(AppLog::SessionPath());
    PathTip(AppLog::SessionPath());
    ImGui::Separator();

    // the kinds the game's readers export, in even columns: the assets, then the data dumps folded
    // under one node; the others named in a tooltip
    ImGui::TextUnformatted("Kinds exported by zone");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Used when exporting by zone / map; by asset exports what you picked");
    const std::set<DiskGame::Kind> Exported = Disk->Kinds();
    const auto KindBoxes = [&](bool Research, int Columns) {
        if (!ImGui::BeginTable(Research ? "##diskdumpkinds" : "##diskkinds", Columns, ImGuiTableFlags_SizingStretchSame)) return;
        for (size_t i = 0; i < std::size(DiskGame::Kinds); i++)
        {
            if (DiskGame::Kinds[i].Research != Research || !Exported.count(DiskGame::Kinds[i].Id)) continue;
            ImGui::TableNextColumn();
            bool On = DiskKindFlags[i] != 0;
            if (ImGui::Checkbox(DiskGame::Kinds[i].Label, &On)) { DiskKindFlags[i] = On; Persist(); }
        }
        ImGui::EndTable();
    };
    KindBoxes(false, 4);
    size_t Dumps = 0, DumpsOn = 0;
    std::string Missing;
    for (size_t i = 0; i < std::size(DiskGame::Kinds); i++)
    {
        if (!Exported.count(DiskGame::Kinds[i].Id)) Missing += std::string(Missing.empty() ? "" : "\n") + DiskGame::Kinds[i].Label;
        else if (DiskGame::Kinds[i].Research) { Dumps++; DumpsOn += DiskKindFlags[i] != 0; }
    }
    bool Folded = false;
    if (Dumps)
    {
        const std::string Node = "Data dumps (" + std::to_string(DumpsOn) + " of " + std::to_string(Dumps) + " ticked)###diskdumps";
        if (ImGui::TreeNode(Node.c_str()))
        {
            ImGui::Unindent();
            KindBoxes(true, 2);
            ImGui::Indent();
            ImGui::TreePop();
        }
        else Folded = true;
    }
    if (!Missing.empty())
    {
        if (Folded) ImGui::SameLine();
        ImGui::TextDisabled("(%zu kinds not exported for %s)", std::count(Missing.begin(), Missing.end(), '\n') + 1, Disk->Title());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Not supported for this game yet:\n%s", Missing.c_str());
    }

    DrawLanguages();

    // the game's settings, each while one of the kinds it concerns is ticked or picked
    std::set<DiskGame::Kind> Picked;
    for (const auto& A : AssetPicks) Picked.insert(A.K);
    bool AnySetting = false;
    for (const DiskGame::Setting* Each : Disk->AllSettings())
    {
        const DiskGame::Setting& S = *Each;
        bool Shown = false;
        for (DiskGame::Kind K : S.Concerns) Shown = Shown || (ExportByAsset ? Picked.count(K) != 0 : DiskKindFlags[(size_t)K] != 0);
        if (!Shown) continue;
        if (!AnySetting)
        {
            ImGui::Separator();
            ImGui::TextUnformatted("Settings");
        }
        AnySetting = true;
        const std::string Key = SettingKey(*Disk, S);
        const std::string Value = SettingsManager::GetSetting(Key, S.Default);
        std::string Preview = Value;
        for (const auto& [V, L] : S.Values)
            if (V == Value) Preview = L;
        ImGui::TextUnformatted(S.Label);
        ImGui::SameLine(ImGui::GetFontSize() * 12.0f);
        ImGui::SetNextItemWidth(200);
        if (ImGui::BeginCombo(("##disksetting_" + Key).c_str(), Preview.c_str()))
        {
            for (const auto& [V, L] : S.Values)
                if (ImGui::Selectable(L.c_str(), V == Value)) SettingsManager::SetSetting(Key, V);
            ImGui::EndCombo();
        }
    }

    // the name filter, for a zone export of a game whose readers can filter
    if (!ExportByAsset)
    {
        ImGui::Separator();
        ImGui::TextUnformatted("Name filter");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ImGui::BeginDisabled(!Disk->FiltersByName());
        ImGui::InputTextWithHint("##disknamefilter", "All assets, or a,b: names containing a or b", DiskNameFilterBuf, sizeof(DiskNameFilterBuf));
        if (ImGui::IsItemDeactivatedAfterEdit()) Persist();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(Disk->FiltersByName() ? "Only the assets whose name contains one of these (comma-separated)"
                                                    : "This game's reader writes each kind whole: every asset is exported");
    }

    // the maps exported to this folder, each opened in Blender
    DrawMapsInBlender(Target);
    ImGui::PopStyleVar(2);
}

// The languages exported: the install's (auto), all, or a set; the install's as its launcher says
void LoadGamePanel::DrawLanguages()
{
    const std::string Folder = GameBuf;
    const Languages::Detected Found = InstallLanguage.Get(Folder, 10.0, [Folder] { return Languages::OfInstall(Folder); });
    // the saved option as words: "auto", "all", codes
    const std::string Saved = SettingsManager::GetSetting(Languages::SettingKey, Languages::GuiDefault);
    std::vector<std::string> Words;
    {
        std::stringstream In(ExportNaming::Lower(Saved));
        for (std::string W; std::getline(In, W, ',');)
        {
            W.erase(0, W.find_first_not_of(' '));
            W.erase(W.find_last_not_of(' ') + 1);
            if (W == "auto" || W == "all") Words.push_back(W);
            else if (std::string C = Languages::CodeOf(W); !C.empty()) Words.push_back(C);
        }
    }
    const auto Has = [&](const std::string& W) { return std::find(Words.begin(), Words.end(), W) != Words.end(); };
    const bool All = Has("all") || Words.empty();
    const bool Auto = Has("auto");
    const std::string Own = Found.Main().empty() ? std::string() : Languages::NameOf(Found.Main());
    const auto Save = [&](std::vector<std::string> W) {
        std::string Text;
        for (const auto& Each : W)
            if (!Each.empty()) Text += (Text.empty() ? "" : ",") + Each;
        SettingsManager::SetSetting(Languages::SettingKey, Text.empty() ? std::string("all") : Text);
    };

    // what is exported, in a few words
    std::string Preview;
    if (All || (Auto && Own.empty())) Preview = "All languages";
    else
    {
        if (Auto) Preview = Own + " (the install's)";
        for (const auto& W : Words)
            if (W != "auto" && !(Auto && W == Found.Main())) Preview += (Preview.empty() ? "" : " + ") + Languages::NameOf(W);
    }
    ImGui::Separator();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Languages");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The localized sounds and zones exported (voices, subtitles); what belongs to no language is always exported.\n"
                          "A zone you pick is exported whatever its language.");
    ImGui::SameLine(ImGui::GetFontSize() * 12.0f);
    ImGui::SetNextItemWidth(200);
    if (std::exchange(OpenLanguages, false)) ImGui::OpenPopupEx(ImHashStr("##ComboPopup", 0, ImGui::GetID("##disklanguages")));
    if (ImGui::BeginCombo("##disklanguages", Preview.c_str(), ImGuiComboFlags_HeightLarge))
    {
        const std::string AutoLabel = Own.empty() ? std::string("The install's (not known: all)") : "The install's: " + Own;
        if (ImGui::Selectable(AutoLabel.c_str(), Auto && Words.size() == 1)) Save({ "auto" });
        if (ImGui::Selectable("All languages", All)) Save({ "all" });
        ImGui::Separator();
        ImGui::TextDisabled("Or these:");
        bool AutoOn = Auto && !All;
        if (ImGui::Checkbox(("The install's" + (Own.empty() ? std::string() : " (" + Own + ")") + "##langauto").c_str(), &AutoOn))
        {
            std::vector<std::string> W;
            if (AutoOn) W.push_back("auto");
            for (const auto& Each : Words)
                if (Each != "auto" && Each != "all") W.push_back(Each);
            Save(W);
        }
        for (const Languages::Language& L : Languages::All())
        {
            bool On = !All && Has(L.Code);
            const bool Installed = std::find(Found.Installed.begin(), Found.Installed.end(), L.Code) != Found.Installed.end();
            const std::string Label = std::string(L.Name) + (Installed ? "  (installed)" : "") + "##lang" + L.Code;
            if (ImGui::Checkbox(Label.c_str(), &On))
            {
                std::vector<std::string> W;
                for (const auto& Each : Words)
                    if (Each != "all" && Each != L.Code) W.push_back(Each);
                if (On) W.push_back(L.Code);
                Save(W);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (Own.empty()) ImGui::TextDisabled("install: not known");
    else ImGui::TextDisabled("install: %s", Languages::Describe(Found).c_str());
    if (ImGui::IsItemHovered())
    {
        std::string Installed;
        for (const auto& L : Found.Installed) Installed += (Installed.empty() ? "" : ", ") + Languages::NameOf(L);
        if (Found.Source.empty())
            ImGui::SetTooltip("Neither Battle.net's .product.db nor a Steam app manifest names this install's language:\n"
                              "\"the install's\" exports every language");
        else
            ImGui::SetTooltip("As %s's files say (%s)%s%s", Found.Source.c_str(), Found.Raw.c_str(),
                              Installed.empty() ? "" : "\nInstalled: ", Installed.c_str());
    }
}

// The last export's result: what was written (open it, show it in Assets), then what did not go
void LoadGamePanel::DrawLastRun()
{
    LastRunInfo R;
    std::vector<std::string> Lines;
    {
        std::lock_guard<std::mutex> Lock(Mutex);
        if (!LastRun.Has) return;
        R = LastRun;
        Lines = Log;
    }
    bool Dismiss = false;
    if (!R.OutDir.empty())
    {
        const bool Ok = R.Written > 0;
        BeginBanner("##lastrun_ok", Ok ? ImVec4(0.12f, 0.23f, 0.17f, 1.0f) : ImVec4(0.17f, 0.18f, 0.20f, 1.0f),
                    Ok ? ImVec4(0.18f, 0.42f, 0.29f, 1.0f) : ImVec4(1, 1, 1, 0.08f));
        ImGui::TextUnformatted(R.Headline.c_str());
        ImGui::TextDisabled("%s", R.Detail.c_str());
        if (Ok)
        {
            if (ImGui::Button("Show in Library")) ShowAssets.store(true);
            ImGui::SameLine();
        }
        if (ImGui::Button("Open folder")) UiAsync::OpenInShell(R.OutDir);
        ImGui::SameLine();
        if (ImGui::Button("Open export log")) UiAsync::OpenInShell(R.RunLog);
        ImGui::SameLine();
        if (ImGui::Button("Dismiss")) Dismiss = true;
        if (!Lines.empty() && ImGui::TreeNode("Details##lastrun"))
        {
            ImGui::BeginChild("##lastrunlog", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 8), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
            for (const auto& Line : Lines) ImGui::TextUnformatted(Line.c_str());
            ImGui::EndChild();
            ImGui::TreePop();
        }
        EndBanner();
    }
    if (!R.ZoneErrors.empty() || R.Failures > 0)
    {
        BeginBanner("##lastrun_err", ImVec4(0.23f, 0.13f, 0.14f, 1.0f), ImVec4(0.48f, 0.23f, 0.25f, 1.0f));
        if (!R.ZoneErrors.empty())
        {
            ImGui::TextColored(Red, "%s", R.OutDir.empty() ? R.Headline.c_str()
                                          : (std::to_string(R.ZoneErrors.size()) + (R.ZoneErrors.size() == 1 ? " zone" : " zones") + " did not open").c_str());
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(R.ZoneErrors.front().c_str());
            ImGui::PopTextWrapPos();
            if (R.ZoneErrors.size() > 1) ImGui::TextDisabled("and %zu more (see the export log)", R.ZoneErrors.size() - 1);
            if (ImGui::SmallButton("Copy errors"))
            {
                std::string All;
                for (const auto& E : R.ZoneErrors) All += E + "\n";
                ImGui::SetClipboardText(All.c_str());
            }
        }
        if (R.Failures > 0)
            ImGui::TextColored(Amber, "%s assets could not be written (see the export log)", Thousands(R.Failures).c_str());
        if (R.OutDir.empty())
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("Dismiss##err")) Dismiss = true;
        }
        EndBanner();
    }
    if (Dismiss)
    {
        std::lock_guard<std::mutex> Lock(Mutex);
        LastRun = {};
    }
}

// The export of what the tab shown picks, on the worker
void LoadGamePanel::StartExport()
{
    DiskZonesToExport.clear();
    ExportOnly.reset();
    ExportKinds.clear();
    if (ExportByAsset)
    {
        // the zones defining the picked assets, each once, in the install's order; the run
        // limited to the picks (Context::Only), their dependencies pulled as usual
        std::shared_ptr<const DiskGame::ZoneIndex> Ix;
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            Ix = DiskIndex;
        }
        // each from its first zone of the run's languages (or of none), else its first zone
        DiskGame::ExportOptions Options = DiskOptions(*Disk, GameBuf);
        Languages::Choice Spoken;
        std::string Why;
        if (!DiskGame::RunLanguages(*Disk, Options, std::string(), Spoken, Why)) Spoken = Languages::Choice();
        std::set<size_t> Zones;
        for (const auto& A : AssetPicks)
        {
            ExportKinds.insert(A.K);
            const auto In = Ix->DefinedIn(A);
            const auto Kept = std::find_if(In.begin(), In.end(), [&](size_t Z) { return Spoken.Wants(Disk->LanguageOfZone(Ix->Zones()[Z])); });
            if (!In.empty()) Zones.insert(Kept != In.end() ? *Kept : In.front());
        }
        for (const size_t Z : Zones) DiskZonesToExport.push_back(Ix->Zones()[Z]);
        ExportOnly = std::make_shared<DiskGame::Selection>(AssetPicks);
    }
    else
        for (size_t i = 0; i < DiskZoneNames.size(); i++)
            if (DiskZoneFlags[i]) DiskZonesToExport.push_back(DiskZoneNames[i]);
    DiskCancel.store(false);
    DiskStartedAt = ImGui::GetTime();
    StartWork(ExportLabel, &LoadGamePanel::DoDiskExport);
}

void LoadGamePanel::DrawExportBar()
{
    ImGui::Separator();
    DrawLastRun();
    DrawDiskProgress();
    const bool Busyish = Busy.load();

    // what is picked, where it goes, and why Export is not ready when it is not
    const std::string Target = (fs::path(AbsoluteFolder(DiskOutBuf)) / Disk->Id()).string();
    std::string Picked, Note, Why;
    bool Ready = false;
    bool HaveIndex = false;
    {
        std::lock_guard<std::mutex> Lock(Mutex);
        HaveIndex = DiskAssets != nullptr;
    }
    if (ExportByAsset)
    {
        Picked = AssetPicks.empty() ? std::string("Nothing picked yet")
                                    : Thousands(AssetPicks.size()) + (AssetPicks.size() == 1 ? " asset picked" : " assets picked");
        Note = "Their images, materials and other dependencies are exported too, from whichever zone holds them";
        if (!HaveIndex) Why = "Index the install above to pick assets, or export a whole map by zone";
        else if (AssetPicks.empty()) Why = "Pick assets in the list to export them";
        Ready = HaveIndex && !AssetPicks.empty();
    }
    else
    {
        const size_t Zones = (size_t)std::count(DiskZoneFlags.begin(), DiskZoneFlags.end(), 1);
        const std::set<DiskGame::Kind> Exported = Disk->Kinds();
        std::string Kinds;
        for (size_t i = 0; i < DiskKindFlags.size(); i++)
            if (DiskKindFlags[i] && Exported.count(DiskGame::Kinds[i].Id)) Kinds += (Kinds.empty() ? "" : ", ") + DiskGame::KindFolder(DiskGame::Kinds[i].Id);
        Picked = Zones == 0 ? std::string("Nothing picked yet") : Thousands(Zones) + (Zones == 1 ? " zone picked" : " zones picked");
        Note = Kinds.empty() ? std::string("no kind ticked") : Kinds;
        if (Zones == 0) Why = "Pick a map or a zone above to export it";
        else if (Kinds.empty()) Why = "Tick at least one kind of asset in Formats & options";
        Ready = Zones > 0 && !Kinds.empty();
    }
    if (Ready && DiskOutBuf[0] == 0)
    {
        Why = "Set an output folder in Formats & options";
        Ready = false;
    }

    const float ExportWidth = ImGui::GetFontSize() * 9.0f;
    const float OptionsWidth = ImGui::CalcTextSize("Formats & options").x + ImGui::GetStyle().FramePadding.x * 2;
    const float Buttons = ExportWidth + OptionsWidth + ImGui::GetStyle().ItemSpacing.x * 2;
    // the text beside the buttons when the panel is wide, above them when it is narrow; the note
    // (the dependencies, the kinds ticked) in the first line's tooltip
    const bool Beside = ImGui::GetContentRegionAvail().x - Buttons >= ImGui::GetFontSize() * 22.0f;
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(Beside ? ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - Buttons : 0.0f);
    ImGui::TextUnformatted(Picked.c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Note.c_str());
    if (!ExportByAsset && !Note.empty())
    {
        ImGui::SameLine();
        ImGui::TextDisabled("\xC2\xB7 %s", Note.c_str());
    }
    if (!Why.empty() && !Busyish) ImGui::TextColored(Amber, "%s", Why.c_str());
    else if (ExportByAsset) ImGui::TextDisabled("to %s", Target.c_str());
    else DrawEstimate(Target);
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();

    if (Beside) ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - Buttons));
    else ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - Buttons));
    ImGui::BeginDisabled(Busyish);
    const float Tall = ImGui::GetFrameHeight() * 1.5f;
    if (ImGui::Button("Formats & options", ImVec2(OptionsWidth, Tall)) || OpenOptions) ImGui::OpenPopup("##diskoptions");
    OpenOptions = false;
    ImGui::SameLine();
    ImGui::BeginDisabled(!Ready);
    if (Ready) Theme::PushPrimary();   // not ready: a plain button, the accent kept for what can be done
    const std::string Label = ExportByAsset && !AssetPicks.empty()
        ? "Export " + Thousands(AssetPicks.size()) + (AssetPicks.size() == 1 ? " asset" : " assets")
        : std::string("Export");
    if (ImGui::Button((Label + "###diskexport").c_str(), ImVec2(ExportWidth, Tall))) StartExport();
    if (Ready) Theme::PopPrimary();
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    // a set width, at most most of the window high (scrolled past that): a long path or many kinds
    // do not push it off the screen
    const float OptionsW = ImGui::GetFontSize() * 34.0f;
    ImGui::SetNextWindowSizeConstraints(ImVec2(OptionsW, 0), ImVec2(OptionsW, ImGui::GetMainViewport()->WorkSize.y * 0.8f));
    if (ImGui::BeginPopup("##diskoptions"))
    {
        DrawOptions();
        ImGui::EndPopup();
    }
}

void LoadGamePanel::DrawEstimate(const std::string& Target)
{
    namespace Sizes = DiskGame::Sizes;
    // what is picked now, from the counts and samples kept (nothing is read here)
    Sizes::Picks P;
    bool Have = false;
    Sizes::Calibration Samples;
    {
        std::lock_guard<std::mutex> Lock(Mutex);
        Have = DiskZoneCounted && DiskZoneCounts.size() == DiskZoneNames.size() && DiskZoneFootprints.size() == DiskZoneNames.size() &&
               DiskZoneFlags.size() == DiskZoneNames.size();
        if (Have)
        {
            Samples = DiskSizes;
            for (size_t Z = 0; Z < DiskZoneNames.size(); Z++)
            {
                if (!DiskZoneFlags[Z]) continue;
                P.Zones++;
                for (const auto& [K, N] : DiskZoneCounts[Z]) P.Counts[K] += N;
                P.Footprint += DiskZoneFootprints[Z];
                P.Picked.emplace_back(DiskZoneNames[Z], DiskZoneFootprints[Z]);
                if (!DiskZoneLevels[Z].empty())
                {
                    P.Levels++;
                    P.LevelFootprint += DiskZoneFootprints[Z];
                }
            }
        }
    }
    const std::set<DiskGame::Kind> Exported = Disk->Kinds();
    for (size_t i = 0; i < DiskKindFlags.size(); i++)
        if (DiskKindFlags[i] && Exported.count(DiskGame::Kinds[i].Id)) P.Kinds.insert(DiskGame::Kinds[i].Id);
    P.LevelsKnown = Disk->LevelNames() != nullptr;
    P.Settings = DiskOptions(*Disk, GameBuf).Settings;
    P.Formats = Sizes::FormatsNow();
    if (Have)
    {
        std::shared_ptr<const DiskGame::ZoneIndex> Ix;
        std::vector<size_t> Zones;
        std::string Key = P.Formats + "|";
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            Ix = DiskIndex;
            for (size_t Z = 0; Z < DiskZoneFlags.size(); Z++)
                if (DiskZoneFlags[Z]) Zones.push_back(Z);
        }
        for (DiskGame::Kind K : P.Kinds) Key += std::to_string((int)K) + ",";
        Key += "|" + std::to_string(ExportNaming::Fnv1a(std::string(reinterpret_cast<const char*>(Zones.data()), Zones.size() * sizeof(size_t))));
        // the languages exported: the saved choice, "auto" as the install's launcher says (read on a worker)
        const std::string Folder = GameBuf;
        const Languages::Detected Found = InstallLanguage.Get(Folder, 10.0, [Folder] { return Languages::OfInstall(Folder); });
        Languages::Choice Spoken;
        std::string Why;
        if (!Languages::Resolve(SettingsManager::GetSetting(Languages::SettingKey, Languages::GuiDefault), Found, Spoken, Why)) Spoken = Languages::Choice();
        Key += "|" + Spoken.Text();
        if (Ix && Key != PickWeightsFor)
        {
            PickWeights = Sizes::WeighPicks(*Ix, Zones, P.Kinds, P.Formats, Spoken);
            PickWeightsFor = Key;
        }
        P.Weights = PickWeights;
    }

    const uint64_t Free = FreeSpace.Get(Target, 5.0, [Target] { return Sizes::FreeBytes(Target); });
    std::string Size;
    Sizes::Estimate E;
    // the kinds ticked that neither the index's weights nor a sample estimate: named, measured
    // only when asked (a sample is a real export, a level's minutes long)
    std::set<DiskGame::Kind> Missing;
    if (Have && P.Zones > 0 && !P.Kinds.empty())
    {
        E = Sizes::Compute(Samples, P);
        std::string Names;
        for (const auto& [K, R] : E.Kinds)
            if (R.Unknown)
            {
                Missing.insert(K);
                Names += (Names.empty() ? "" : ", ") + DiskGame::KindFolder(K);
            }
        const bool Measuring = Busy.load() && BusyLabel == SizesLabel;
        if (Missing.size() < E.Kinds.size()) Size = (E.Approx ? "\xE2\x89\x88 " : "") + Sizes::Text(E.Bytes);
        if (!Missing.empty())
            Size += (Size.empty() ? "" : " + ") + Names + (Measuring ? " (measuring\xE2\x80\xA6)" : " (not estimated)");
    }
    const bool Over = !Size.empty() && Free > 0 && E.Bytes > Free, Near = !Size.empty() && Free > 0 && E.Bytes > Free / 10 * 8;
    const std::string Line = (Size.empty() ? std::string() : Size + "  ·  ") + "to " + Target +
                             (Free > 0 ? "  (" + Sizes::Text(Free) + " free" + (Over ? ": not enough)" : ")") : std::string());
    if (Over || Near) ImGui::TextColored(Over ? Red : Amber, "%s", Line.c_str());
    else ImGui::TextDisabled("%s", Line.c_str());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        std::string Tip;
        if (Size.empty())
            Tip = "The size estimate needs the install index's counts and sizes: index the install";
        else
        {
            Tip = "What the export would write, kind by kind (\xE2\x89\x88: an approximation):\n";
            for (const auto& [K, R] : E.Kinds)
                Tip += "\n" + DiskGame::KindFolder(K) + ": " + (R.Unknown ? std::string("?") : (R.Approx ? "\xE2\x89\x88 " : "") + Sizes::Text(R.Bytes)) +
                       "\n    " + R.Basis;
            Tip += "\n\nImages and sounds: what the install index weighed of each (its size, its format), each once.\n"
                   "Other kinds: the index's counts times a sample of each kind exported for real, taken only\n"
                   "when you ask (\"Estimate the rest\"; another format converted by a typical ratio). What the\n"
                   "assets need from other zones, pulled as dependencies, comes on top.";
        }
        ImGui::SetTooltip("%s", Tip.c_str());
    }
    // what would make the estimate whole, each on demand and on its own line (beside the wrapped
    // text above, a button would sit under "Formats & options" and take its clicks)
    bool Unweighed = false;
    {
        std::lock_guard<std::mutex> Lock(Mutex);
        Unweighed = DiskIndex && !DiskIndex->Weighed;   // an index of before the weights
    }
    if (Busy.load() || (Missing.empty() && !Unweighed)) return;
    if (!Missing.empty())
    {
        if (ImGui::SmallButton("Estimate the rest"))
        {
            MeasureMissing = Missing;
            DiskCancel.store(false);
            DiskStartedAt = ImGui::GetTime();
            StartWork(SizesLabel, &LoadGamePanel::DoMeasureMissing);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Exports a sample of each kind not estimated yet to a temporary folder and measures it:\n"
                              "seconds per kind, minutes for a map (a whole level is exported)");
        if (Unweighed) ImGui::SameLine();
    }
    if (Unweighed)
    {
        if (ImGui::SmallButton("Index again for exact sizes"))
        {
            DiskCancel.store(false);
            DiskStartedAt = ImGui::GetTime();
            StartWork(IndexLabel, &LoadGamePanel::DoIndexDiskZones);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("This index was built before image and sound sizes were recorded: rebuilt (a few minutes),\n"
                              "the estimate adds up their own sizes instead of sampling a few");
    }
}

void LoadGamePanel::DrawDiskProgress()
{
    std::lock_guard<std::mutex> Lock(Mutex);
    if (!Busy.load() || (BusyLabel != ExportLabel && BusyLabel != IndexLabel && BusyLabel != PreviewLabel && BusyLabel != SizesLabel)) return;
    ImGui::ProgressBar(DiskFraction, ImVec2(-90, 0), DiskStep.c_str());
    DrewIndicator = true;
    ImGui::SameLine();
    ImGui::BeginDisabled(DiskCancel.load());
    if (ImGui::Button("Cancel##diskexport", ImVec2(80, 0))) DiskCancel.store(true);
    ImGui::EndDisabled();
    if (BusyLabel == ExportLabel) ImGui::TextDisabled("%s elapsed  \xC2\xB7  %s", Clock(ImGui::GetTime() - DiskStartedAt).c_str(), DiskEtaText.c_str());
    else ImGui::TextDisabled("%s elapsed", Clock(ImGui::GetTime() - DiskStartedAt).c_str());
    if (!DiskStall.empty()) ImGui::TextColored(Amber, "%s", DiskStall.c_str());
    if (DiskCancel.load()) ImGui::TextDisabled("Stopping after the current step\xE2\x80\xA6");
}
