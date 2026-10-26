// The picked game's section: the zones listed by the game (IDiskGame::ListZones), the picked ones
// resolved (ResolveZones) and exported by DiskGame::Export, the use case disk-export runs on the
// command line.

#include "stdafx.h"

#include "ImGuiLoadGamePanel.h"
#include "ImGuiDialogs.h"
#include "ImGuiBlender.h"
#include "AppLog.h"
#include "DiskGame.h"
#include "ExportNaming.h"
#include "SettingsManager.h"
#include "Strings.h"
#include "WraithX.h"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

namespace
{
    constexpr const char* ExportLabel = "disk export";
    constexpr const char* ListLabel = "zone list";
    constexpr const char* IndexLabel = "install index";

    using ExportNaming::Thousands;

    size_t Total(const std::map<DiskGame::Kind, size_t>& Counts)
    {
        size_t N = 0;
        for (const auto& [K, C] : Counts) N += C;
        return N;
    }

    std::string SettingKey(const DiskGame::IDiskGame& Game, const DiskGame::Setting& S)
    {
        return std::string("disk_") + Game.Id() + "_" + S.Key;
    }

    // What the game's hooks read: the install and the game's saved settings, the rest at its default.
    DiskGame::ExportOptions DiskOptions(const DiskGame::IDiskGame& Game, const std::string& Install)
    {
        DiskGame::ExportOptions O;
        O.Install = Install;
        for (const DiskGame::Setting* S : Game.AllSettings())
            O.Settings[S->Key] = SettingsManager::GetSetting(SettingKey(Game, *S), S->Default);
        return O;
    }

    // 75 as "1:15", 3725 as "1:02:05"
    std::string Clock(double Seconds)
    {
        const long long S = (long long)Seconds;
        char B[32];
        if (S >= 3600) snprintf(B, sizeof(B), "%lld:%02lld:%02lld", S / 3600, S / 60 % 60, S % 60);
        else snprintf(B, sizeof(B), "%lld:%02lld", S / 60, S % 60);
        return B;
    }

    // What a zone list was read for: another game, folder or setting reads it again.
    std::string ListedFor(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& O)
    {
        std::string Key = std::string(Game.Id()) + "|" + O.Install;
        for (const auto& [K, V] : O.Settings) Key += "|" + K + "=" + V;
        return Key;
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
        Status = "reading the zone list...";
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
            Status = "reading the level names from the game's strings (" + std::to_string(Done) + "/" + std::to_string(Count) + " zones)...";
        };
        if (const auto Found = DiskGame::LevelNames::For(*Game, Options, Install, Step, Why))
            for (size_t i = 0; i < Names.size(); i++) Named += !(Levels[i] = Found->Of(Names[i])).empty();
        else if (!Why.empty())
            AppLog::Log().warn("load game: no level names for {}: {}", Game->Id(), Why);
    }

    {
        std::lock_guard<std::mutex> Lk(Mutex);
        DiskZoneNames = std::move(Names);
        DiskZoneLevels = std::move(Levels);
        DiskZoneFlags.assign(DiskZoneNames.size(), 0);
        for (size_t i = 0; i < DiskZoneNames.size(); i++)
            DiskZoneFlags[i] = Saved.find("," + DiskZoneNames[i] + ",") != std::string::npos;
        DiskZonesFor = ListedFor(*Game, Options);
        Status = Ok ? std::to_string(DiskZoneNames.size()) + " zones" + (Named ? ", " + std::to_string(Named) + " named levels" : "")
                    : Error;
    }
    ReadZoneCounts(*Game, Options, Install);
}

void LoadGamePanel::ReadZoneCounts(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& Options, const std::string& Install)
{
    const auto Ix = DiskGame::ZoneIndex::Kept(Game, Options, Install);
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
    std::lock_guard<std::mutex> Lk(Mutex);
    DiskZoneCounted = Ix && Ix->Zones() == DiskZoneNames;
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

// Worker: the picked zones' kinds sampled (the ticked ones and those exported by default, so that
// ticking one later needs no new sample), measured into DiskSizes and kept beside the index.
void LoadGamePanel::MeasureSizes(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& Options, const std::string& Install,
                                 const DiskGame::ZoneIndex& Ix)
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
    }
    std::string Error;
    const auto Start = std::chrono::steady_clock::now();
    const auto Ix = DiskGame::ZoneIndex::Build(Game, Options, Install, [&](size_t Done, size_t Count) {
        std::lock_guard<std::mutex> Lk(Mutex);
        DiskFraction = Count ? (float)Done / (float)Count : 0.0f;
        DiskStep = "indexing the install: zone " + std::to_string(std::min(Done + 1, Count)) + "/" + std::to_string(Count);
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
    }
    // then the size estimate's samples, from the zones picked
    if (Ix && !DiskCancel.load()) MeasureSizes(Game, Options, Install, *Ix);
}

// Worker: the picked zones resolved, then DiskGame::Export with the progress and the cancel flag.
void LoadGamePanel::DoDiskExport()
{
    WraithX::InitializeAPI();
    const DiskGame::IDiskGame& Game = *Disk;
    const std::string Install = GameBuf;
    const DiskGame::ExportOptions Options = DiskOptions(Game, Install);
    DiskGame::Context C;
    C.OutRoot = (fs::path(AbsoluteFolder(DiskOutBuf)) / Game.Id()).string();
    if (Game.FiltersByName()) C.NameFilter = DiskNameFilterBuf;
    const std::set<DiskGame::Kind> Exported = Game.Kinds();
    for (size_t i = 0; i < DiskKindFlags.size(); i++)
        if (DiskKindFlags[i] && Exported.count(DiskGame::AllKinds()[i])) C.Kinds.insert(DiskGame::AllKinds()[i]);
    {
        std::lock_guard<std::mutex> Lk(Mutex);
        Log.clear();
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
    std::lock_guard<std::mutex> Lk(Mutex);
    std::istringstream Report(DiskGame::Report(T));
    for (std::string Line; std::getline(Report, Line);) Log.push_back(Line);
    Status = std::string(T.Cancelled ? "cancelled: " : "") + std::to_string(T.ZonesWalked) + "/" + std::to_string(T.Zones)
             + " zones read to their end, " + std::to_string(Written) + " files written to " + C.OutRoot + " in "
             + Clock(std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count())
             + (T.Failures.empty() ? "" : ", " + std::to_string(T.Failures.size()) + " failures (see the log)")
             // a zone that did not open says why (a missing Oodle DLL leaves every zone unread)
             + (T.ZoneErrors.empty() ? "" : "; " + std::to_string(T.ZoneErrors.size()) + " zone(s) did not open: " + T.ZoneErrors.front());
    DiskStall.clear();
    DiskFraction = 1.0f;
    DiskExportedTo = C.OutRoot;
    if (Written > 0) DiskJustFinished.store(true);
}

void LoadGamePanel::DrawDiskSection()
{
    ImGui::TextWrapped("%s is read straight from its files: no game needs to run.", Disk->Title());
    ImGui::Spacing();

    ImGui::TextUnformatted("Output folder");
    ImGui::SetNextItemWidth(-90.0f);
    ImGui::InputText("##diskoutdir", DiskOutBuf, sizeof(DiskOutBuf));
    if (ImGui::IsItemDeactivatedAfterEdit()) Persist();
    ImGui::SameLine();
    if (ImGui::Button("Browse##diskout", ImVec2(80, 0)))
        UiAsync::PickFolder("loadgame.output", "Select where to export to", AbsoluteFolder(DiskOutBuf));
    const std::string Target = (fs::path(AbsoluteFolder(DiskOutBuf)) / Disk->Id()).string();
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
    ImGui::BeginDisabled(!HasTarget);
    if (ImGui::SmallButton("Open##diskout")) UiAsync::OpenInShell(Target);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!HasRunLog);
    if (ImGui::SmallButton("Export log##diskout")) UiAsync::OpenInShell(RunLog);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("Session log##diskout")) UiAsync::OpenInShell(AppLog::SessionPath());
    ImGui::SameLine();
    ImGui::TextDisabled("written to %s", Target.c_str());

    // the kinds the game's readers export, wrapped to the panel's width: the assets, then the data
    // dumps folded under one node; the others named in a tooltip
    const std::set<DiskGame::Kind> Exported = Disk->Kinds();
    const auto KindBoxes = [&](bool Research) {
        const float Right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        bool First = true;
        for (size_t i = 0; i < std::size(DiskGame::Kinds); i++)
        {
            if (DiskGame::Kinds[i].Research != Research || !Exported.count(DiskGame::Kinds[i].Id)) continue;
            const float Width = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(DiskGame::Kinds[i].Label).x;
            if (!First)
            {
                ImGui::SameLine();
                if (ImGui::GetCursorScreenPos().x + Width > Right) ImGui::NewLine();
            }
            First = false;
            bool On = DiskKindFlags[i] != 0;
            if (ImGui::Checkbox(DiskGame::Kinds[i].Label, &On)) { DiskKindFlags[i] = On; Persist(); }
        }
    };
    KindBoxes(false);
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
            KindBoxes(true);
            ImGui::TreePop();
        }
        else Folded = true;
    }
    if (!Missing.empty())
    {
        if (Folded) ImGui::SameLine();
        ImGui::TextDisabled("(%zu kinds not exported for %s)", std::count(Missing.begin(), Missing.end(), '\n') + 1, Disk->Title());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Not exported yet (its readers have no exporter for them):\n%s", Missing.c_str());
    }
    // the game's settings, each while one of the kinds it concerns is ticked; the size estimate
    // at their right
    ImGui::BeginGroup();
    for (const DiskGame::Setting* Each : Disk->AllSettings())
    {
        const DiskGame::Setting& S = *Each;
        bool Shown = false;
        for (DiskGame::Kind K : S.Concerns) Shown = Shown || DiskKindFlags[(size_t)K] != 0;
        if (!Shown) continue;
        const std::string Key = SettingKey(*Disk, S);
        const std::string Value = SettingsManager::GetSetting(Key, S.Default);
        std::string Preview = Value;
        for (const auto& [V, Label] : S.Values)
            if (V == Value) Preview = Label;
        ImGui::TextUnformatted(S.Label);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160);
        if (ImGui::BeginCombo(("##disksetting_" + Key).c_str(), Preview.c_str()))
        {
            for (const auto& [V, Label] : S.Values)
                if (ImGui::Selectable(Label.c_str(), V == Value)) SettingsManager::SetSetting(Key, V);
            ImGui::EndCombo();
        }
    }
    ImGui::EndGroup();
    ImGui::SameLine(0.0f, ImGui::GetFontSize() * 2.0f);
    DrawEstimate(Target);

    // the name filter, shown disabled for a game whose readers write a kind whole
    ImGui::TextUnformatted("Names");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-90);
    ImGui::BeginDisabled(!Disk->FiltersByName());
    ImGui::InputTextWithHint("##disknamefilter", "all assets, or a,b: names containing a or b", DiskNameFilterBuf, sizeof(DiskNameFilterBuf));
    if (ImGui::IsItemDeactivatedAfterEdit()) Persist();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(Disk->FiltersByName() ? "Only the assets whose name contains one of these (comma-separated)"
                                                : "This game's reader writes each kind whole: every asset is exported");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Zones");
    // the worker rewrites the list: it is not drawn while that runs
    bool Listed = false;
    {
        std::lock_guard<std::mutex> Lock(Mutex);
        Listed = DiskZonesFor == ListedFor(*Disk, DiskOptions(*Disk, GameBuf));
    }
    if (!Listed || (Busy.load() && BusyLabel == ListLabel))
    {
        if (!Busy.load()) StartWork(ListLabel, &LoadGamePanel::DoListDiskZones);
        ImGui::TextDisabled("reading the zone list...");
        return;
    }
    const float Buttons = ImGui::CalcTextSize("Rescan").x + ImGui::CalcTextSize("Count assets").x +
                          ImGui::GetStyle().FramePadding.x * 4 + ImGui::GetStyle().ItemSpacing.x * 2;
    ImGui::SetNextItemWidth(-Buttons);
    ImGui::InputTextWithHint("##diskzonefilter", "filter by zone or level name", DiskZoneFilterBuf, sizeof(DiskZoneFilterBuf));
    if (ImGui::IsItemDeactivatedAfterEdit()) Persist();
    ImGui::SameLine();
    if (ImGui::Button("Rescan##diskzones")) StartWork(ListLabel, &LoadGamePanel::DoListDiskZones);
    ImGui::SameLine();
    if (ImGui::Button("Count assets##diskzones"))
    {
        DiskCancel.store(false);
        DiskStartedAt = ImGui::GetTime();
        StartWork(IndexLabel, &LoadGamePanel::DoIndexDiskZones);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Opens every zone once to list what each holds (the install index, kept for next time;\n"
                          "an export that pulls dependencies builds it too), then exports a small sample of each\n"
                          "kind from the picked zones to a temporary folder for the size estimate");
    std::vector<uint32_t> Visible;
    for (uint32_t i = 0; i < (uint32_t)DiskZoneNames.size(); i++)
        if (ContainsNoCase(DiskZoneNames[i], DiskZoneFilterBuf) || ContainsNoCase(DiskZoneLevels[i], DiskZoneFilterBuf))
            Visible.push_back(i);
    DrawZoneTable(Visible);
    const size_t Selected = (size_t)std::count(DiskZoneFlags.begin(), DiskZoneFlags.end(), 1);
    ImGui::TextDisabled("%zu / %zu zones selected", Selected, DiskZoneNames.size());
    ImGui::SameLine();
    if (ImGui::Button("Select shown##diskzones"))
    {
        for (const auto Idx : Visible) DiskZoneFlags[Idx] = 1;
        Persist();
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear##diskzones"))
    {
        std::fill(DiskZoneFlags.begin(), DiskZoneFlags.end(), 0);
        Persist();
    }

    ImGui::Spacing();
    const bool AnyKind = std::find(DiskKindFlags.begin(), DiskKindFlags.end(), 1) != DiskKindFlags.end();
    ImGui::BeginDisabled(Selected == 0 || !AnyKind || DiskOutBuf[0] == 0);
    const std::string Label = "Export " + std::to_string(Selected) + " zone(s)";
    if (ImGui::Button(Label.c_str(), ImVec2(160, 0)))
    {
        DiskZonesToExport.clear();
        for (size_t i = 0; i < DiskZoneNames.size(); i++)
            if (DiskZoneFlags[i]) DiskZonesToExport.push_back(DiskZoneNames[i]);
        DiskCancel.store(false);
        DiskStartedAt = ImGui::GetTime();
        StartWork(ExportLabel, &LoadGamePanel::DoDiskExport);
    }
    ImGui::EndDisabled();

    // the maps exported to this folder, each opened in Blender
    DrawMapsInBlender(Target);
}

void LoadGamePanel::DrawEstimate(const std::string& Target)
{
    namespace Sizes = DiskGame::Sizes;
    // what is picked now, from the counts and samples kept (nothing is read here)
    Sizes::Picks P;
    bool Have = false, Sampled = false;
    Sizes::Calibration Samples;
    {
        std::lock_guard<std::mutex> Lock(Mutex);
        Have = DiskZoneCounted && DiskZoneCounts.size() == DiskZoneNames.size() && DiskZoneFootprints.size() == DiskZoneNames.size() &&
               DiskZoneFlags.size() == DiskZoneNames.size();
        Sampled = Have && !DiskSizes.Empty();
        if (Sampled)
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

    const uint64_t Free = FreeSpace.Get(Target, 5.0, [Target] { return Sizes::FreeBytes(Target); });
    ImGui::BeginGroup();
    Sizes::Estimate E;
    if (!Sampled)
    {
        ImGui::TextDisabled("Count assets to estimate the export's size");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("The estimate needs the install index's counts and a small sample of each kind\n"
                              "exported for real from the picked zones: \"Count assets\" below does both");
    }
    else
    {
        E = Sizes::Compute(Samples, P);
        const std::string Size = (E.Approx ? "\xE2\x89\x88 " : "") + Sizes::Text(E.Bytes) + (E.Unknown ? " or more" : "");
        const bool Over = Free > 0 && E.Bytes > Free, Near = Free > 0 && E.Bytes > Free / 10 * 8;
        const ImVec4 Red(1.0f, 0.35f, 0.3f, 1.0f), Amber(1.0f, 0.75f, 0.2f, 1.0f);
        if (P.Zones == 0 || P.Kinds.empty()) ImGui::TextDisabled("Export size: nothing picked");
        else if (Over || Near) ImGui::TextColored(Over ? Red : Amber, "Export size: %s", Size.c_str());
        else ImGui::Text("Export size: %s", Size.c_str());
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            std::string Tip = "What the export would write, kind by kind (\xE2\x89\x88: an approximation):\n";
            for (const auto& [K, R] : E.Kinds)
                Tip += "\n" + DiskGame::KindFolder(K) + ": " + (R.Unknown ? std::string("?") : (R.Approx ? "\xE2\x89\x88 " : "") + Sizes::Text(R.Bytes)) +
                       "\n    " + R.Basis;
            Tip += "\n\nCounts are the install index's; sizes come from a sample of each kind exported for real\n"
                   "when the assets were counted, in the formats of then (another format is converted by a\n"
                   "typical ratio). What the assets need from other zones, pulled as dependencies, comes on top.";
            ImGui::SetTooltip("%s", Tip.c_str());
        }
    }
    if (Free > 0)
    {
        const bool Over = Sampled && P.Zones > 0 && E.Bytes > Free;
        if (Over) ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "%s free on its drive: not enough", Sizes::Text(Free).c_str());
        else ImGui::TextDisabled("%s free on its drive", Sizes::Text(Free).c_str());
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("The free space of the output folder's drive (%s)", Target.c_str());
    }
    ImGui::EndGroup();
}

void LoadGamePanel::DrawZoneTable(std::vector<uint32_t>& Visible)
{
    // the counts are rewritten by a worker (an index built, an export's)
    std::lock_guard<std::mutex> Lock(Mutex);
    const ImGuiTableFlags Flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
    if (!ImGui::BeginTable("diskzones", 3, Flags, ImVec2(0, 220))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Zone", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort, 3.0f);
    ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("Assets", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending,
                            ImGui::CalcTextSize("Assets").x + ImGui::GetFontSize() * 1.5f);
    ImGui::TableHeadersRow();
    if (ImGuiTableSortSpecs* Specs = ImGui::TableGetSortSpecs(); Specs != nullptr && Specs->SpecsCount > 0)
    {
        DiskZoneSortColumn = Specs->Specs[0].ColumnIndex;
        DiskZoneSortDescending = Specs->Specs[0].SortDirection == ImGuiSortDirection_Descending;
        Specs->SpecsDirty = false;
    }
    const bool Counted = DiskZoneCounted && DiskZoneCounts.size() == DiskZoneNames.size();
    // by the column picked, then by zone; a zone that is not a level after the levels
    std::stable_sort(Visible.begin(), Visible.end(), [&](uint32_t A, uint32_t B) {
        if (DiskZoneSortColumn == 1 && DiskZoneLevels[A] != DiskZoneLevels[B])
        {
            if (DiskZoneLevels[A].empty() != DiskZoneLevels[B].empty()) return DiskZoneLevels[B].empty();
            return (_stricmp(DiskZoneLevels[A].c_str(), DiskZoneLevels[B].c_str()) < 0) != DiskZoneSortDescending;
        }
        if (DiskZoneSortColumn == 2 && Counted && Total(DiskZoneCounts[A]) != Total(DiskZoneCounts[B]))
            return (Total(DiskZoneCounts[A]) < Total(DiskZoneCounts[B])) != DiskZoneSortDescending;
        const int Order = _stricmp(DiskZoneNames[A].c_str(), DiskZoneNames[B].c_str());
        return DiskZoneSortColumn == 0 && DiskZoneSortDescending ? Order > 0 : Order < 0;
    });

    ImGuiListClipper Clipper;
    Clipper.Begin((int)Visible.size());
    while (Clipper.Step())
        for (int Row = Clipper.DisplayStart; Row < Clipper.DisplayEnd; Row++)
        {
            const uint32_t Idx = Visible[Row];
            ImGui::PushID((int)Idx);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            bool Checked = DiskZoneFlags[Idx] != 0;
            if (ImGui::Checkbox(DiskZoneNames[Idx].c_str(), &Checked)) { DiskZoneFlags[Idx] = Checked; Persist(); }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(DiskZoneLevels[Idx].c_str());
            ImGui::TableSetColumnIndex(2);
            if (!Counted)
                ImGui::TextDisabled("\xE2\x80\x93");   // not indexed yet
            else
            {
                const auto& Counts = DiskZoneCounts[Idx];
                ImGui::TextUnformatted(Thousands(Total(Counts)).c_str());
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !Counts.empty())
                {
                    std::string Tip;
                    for (const auto& [K, N] : Counts) Tip += (Tip.empty() ? "" : "\n") + Thousands(N) + " " + DiskGame::KindFolder(K);
                    ImGui::SetTooltip("%s\n(what the install index lists: the assets this zone holds whole)", Tip.c_str());
                }
            }
            ImGui::PopID();
        }
    ImGui::EndTable();
}

void LoadGamePanel::DrawDiskProgress()
{
    std::lock_guard<std::mutex> Lock(Mutex);
    if (Busy.load() && (BusyLabel == ExportLabel || BusyLabel == IndexLabel))
    {
        ImGui::ProgressBar(DiskFraction, ImVec2(-90, 0), DiskStep.c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(DiskCancel.load());
        if (ImGui::Button("Cancel##diskexport", ImVec2(80, 0))) DiskCancel.store(true);
        ImGui::EndDisabled();
        if (BusyLabel == IndexLabel) ImGui::TextDisabled("%s elapsed", Clock(ImGui::GetTime() - DiskStartedAt).c_str());
        else ImGui::TextDisabled("%s elapsed  \xC2\xB7  %s", Clock(ImGui::GetTime() - DiskStartedAt).c_str(), DiskEtaText.c_str());
        if (!DiskStall.empty()) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "%s", DiskStall.c_str());
        if (DiskCancel.load()) ImGui::TextDisabled("stopping after the current step...");
    }
    else if (!Status.empty())
        ImGui::TextWrapped("%s", Status.c_str());
}
