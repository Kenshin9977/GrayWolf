// The "By asset" tab: the install index (every asset of every zone, DiskGame::ZoneIndex) searched
// by name, the assets ticked exported alone with what they need (Context::Only, the dependencies
// pulled as usual), one asset previewed by exporting it alone to a temporary folder the Assets
// browser then shows.

#include "stdafx.h"

#include "ImGuiLoadGamePanel.h"
#include "ImGuiLoadGamePanel.Common.h"
#include "ImGuiDialogs.h"
#include "AppLog.h"
#include "DiskGame.h"
#include "ExportNaming.h"
#include "SettingsManager.h"
#include "WraithX.h"

#include "imgui.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>

namespace fs = std::filesystem;
using namespace LoadGameUi;

namespace
{
    using ExportNaming::Thousands;

    // at most this many rows listed: a search that matches more says so
    constexpr size_t ShownAtMost = 20000;

    std::string Lower(std::string S)
    {
        for (char& C : S) C = (char)tolower((unsigned char)C);
        return S;
    }

    // "Oct 26": the day a file was last written
    std::string FileDay(const std::string& Path)
    {
        std::error_code Ec;
        const auto When = fs::last_write_time(Path, Ec);
        if (Ec) return {};
        const time_t T = std::chrono::system_clock::to_time_t(std::chrono::clock_cast<std::chrono::system_clock>(When));
        tm Local{};
        if (localtime_s(&Local, &T) != 0) return {};
        char B[32];
        strftime(B, sizeof(B), "%b %d", &Local);
        return B;
    }
}

// Worker (the zone list's, an index build's, an export's): the index kept, one row per asset at
// the first zone defining it, each name in lower case for the search
void LoadGamePanel::KeepIndex(std::unique_ptr<DiskGame::ZoneIndex> Ix)
{
    std::shared_ptr<const DiskGame::ZoneIndex> Shared(std::move(Ix));
    std::shared_ptr<AssetRows> Rows;
    std::string Day;
    if (Shared)
    {
        Rows = std::make_shared<AssetRows>();
        Rows->Rows.reserve(Shared->Entries);
        Rows->Lower.reserve(Shared->Entries);
        for (size_t Z = 0; Z < Shared->Zones().size(); Z++)
        {
            const auto& Defined = Shared->Defined(Z);
            for (size_t I = 0; I < Defined.size(); I++)
            {
                const auto In = Shared->DefinedIn(Defined[I]);
                if (!In.empty() && In.front() != Z) continue;   // listed at its first zone only
                Rows->Rows.emplace_back((uint32_t)Z, (uint32_t)I);
                Rows->Lower.push_back(Lower(Defined[I].Name));
            }
        }
        Day = FileDay(Shared->File);
    }
    std::lock_guard<std::mutex> Lock(Mutex);
    DiskIndex = std::move(Shared);
    DiskAssets = std::move(Rows);
    DiskIndexDate = Day;
}

// The rows matching the query, the kinds on and "picked only"; the counts per kind before the
// kinds are applied (the chips show them). Nothing is done while none of these changed.
void LoadGamePanel::Search()
{
    std::shared_ptr<const AssetRows> Rows;
    std::shared_ptr<const DiskGame::ZoneIndex> Ix;
    {
        std::lock_guard<std::mutex> Lock(Mutex);
        Rows = DiskAssets;
        Ix = DiskIndex;
    }
    std::string Key = std::string(AssetQueryBuf) + "|" + (AssetPickedOnly ? "picked " + std::to_string(AssetPicks.size()) : std::string()) + "|";
    for (const DiskGame::Kind K : AssetKindsOff) Key += std::to_string((int)K) + ",";
    if (Rows.get() == AssetSearchedIn && Key == AssetSearchedFor) return;
    AssetSearchedIn = Rows.get();
    AssetSearchedFor = Key;
    AssetHits.clear();
    AssetHitKinds.clear();
    AssetHitsTotal = 0;
    AssetHitZones = 0;
    if (!Rows || !Ix) return;

    const std::string Query = Lower(AssetQueryBuf);
    std::vector<uint8_t> ZoneSeen(Ix->Zones().size(), 0);
    for (uint32_t R = 0; R < (uint32_t)Rows->Rows.size(); R++)
    {
        if (!Query.empty() && Rows->Lower[R].find(Query) == std::string::npos) continue;
        const auto [Z, I] = Rows->Rows[R];
        const DiskGame::AssetId& A = Ix->Defined(Z)[I];
        if (AssetPickedOnly && !AssetPicks.count(A)) continue;
        AssetHitKinds[A.K]++;
        if (AssetKindsOff.count(A.K)) continue;
        AssetHitsTotal++;
        if (!ZoneSeen[Z]) { ZoneSeen[Z] = 1; AssetHitZones++; }
        if (AssetHits.size() < ShownAtMost) AssetHits.push_back(R);
    }
    // by kind (models first, as DiskGame::Kinds orders them), then by name
    std::sort(AssetHits.begin(), AssetHits.end(), [&](uint32_t A, uint32_t B) {
        const DiskGame::Kind Ka = Ix->Defined(Rows->Rows[A].first)[Rows->Rows[A].second].K;
        const DiskGame::Kind Kb = Ix->Defined(Rows->Rows[B].first)[Rows->Rows[B].second].K;
        if (Ka != Kb) return Ka < Kb;
        return Rows->Lower[A] < Rows->Lower[B];
    });
}

void LoadGamePanel::DrawAssetTab()
{
    if (!ZonesListed())
    {
        std::lock_guard<std::mutex> Lock(Mutex);
        DrewIndicator = true;
        Loading("Reading " + std::string(Disk->Title()) + ": " + Status);
        return;
    }
    std::shared_ptr<const AssetRows> Rows;
    std::shared_ptr<const DiskGame::ZoneIndex> Ix;
    size_t ZoneCount = 0;
    {
        std::lock_guard<std::mutex> Lock(Mutex);
        Rows = DiskAssets;
        Ix = DiskIndex;
        ZoneCount = DiskZoneNames.size();
        // the picks belong to the zone list they were made in: another game or folder starts over
        if (AssetPicksFor != DiskZonesFor)
        {
            AssetPicks.clear();
            AssetFocus = -1;
            AssetPicksFor = DiskZonesFor;
            AssetSearchedFor.clear();
        }
    }

    // not indexed yet: the one thing to do, and the way around it
    if (ZoneCount == 0)
    {
        // nothing to index: the folder is not the game's, or its reader found nothing in it
        std::string Why;
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            Why = Status;
        }
        ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight() * 2));
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "No zone of %s found in this folder", Disk->Title());
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(Why.c_str());
        ImGui::TextDisabled("Choose the game's install folder with \"Change game\" above, or another game under \"Read with\".");
        ImGui::PopTextWrapPos();
        return;
    }
    if (!Rows || !Ix)
    {
        ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight() * 2));
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.35f);
        ImGui::Text("List what %s holds", Disk->Title());
        ImGui::PopFont();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 40.0f));
        ImGui::Text("GrayWolf opens each of the game's %s zones once to list its models, images, animations and sounds. "
                    "Then you search and pick assets by name.", Thousands(ZoneCount).c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        Theme::PushPrimary();
        if (Busy.load() && BusyLabel == IndexLabel)
        {
            std::string Step;
            {
                std::lock_guard<std::mutex> Lock(Mutex);
                Step = DiskStep;
            }
            DrewIndicator = true;
            Loading("Indexing: " + Step);
        }
        else if (ImGui::Button("Index the install", ImVec2(ImGui::GetFontSize() * 12.0f, ImGui::GetFrameHeight() * 1.6f)))
        {
            DiskCancel.store(false);
            DiskStartedAt = ImGui::GetTime();
            StartWork(IndexLabel, &LoadGamePanel::DoIndexDiskZones);
        }
        Theme::PopPrimary();
        ImGui::TextDisabled("A few minutes the first time, then kept  \xC2\xB7  the game is never launched");
        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::TextDisabled("Just want a whole map?");
        ImGui::SameLine();
        if (ImGui::SmallButton("Browse by zone / map")) WantTab = 1;
        ImGui::SameLine();
        ImGui::TextDisabled("(no index needed)");
        return;
    }

    Search();
    // the side pane beside the list in a wide window, under it in a narrow one
    const ImVec2 Room = ImGui::GetContentRegionAvail();
    const bool Wide = Room.x > ImGui::GetFontSize() * 50.0f;
    const float Pane = std::min(ImGui::GetFontSize() * 22.0f, Room.x * 0.38f);
    // nothing clicked or picked yet: the list takes the room, a line under it says what to do
    const bool ShowPane = (AssetFocus >= 0 && AssetFocus < (int)Rows->Rows.size()) || !AssetPicks.empty();
    ImGui::BeginChild("##assetlist", !ShowPane ? ImVec2(0, -ImGui::GetTextLineHeightWithSpacing())
                                     : Wide    ? ImVec2(-Pane - ImGui::GetStyle().ItemSpacing.x, 0)
                                               : ImVec2(0, Room.y * 0.62f));
    {
        // the search, "picked only", the kinds as chips with their counts
        const std::string PickedLabel = "Picked only (" + Thousands(AssetPicks.size()) + ")";
        ImGui::SetNextItemWidth(-(ImGui::CalcTextSize(PickedLabel.c_str()).x + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x * 2));
        ImGui::InputTextWithHint("##assetquery", "Search by name: ak47, zombie, nuketown\xE2\x80\xA6", AssetQueryBuf, sizeof(AssetQueryBuf));
        ImGui::SameLine();
        ImGui::Checkbox(PickedLabel.c_str(), &AssetPickedOnly);
        const float Right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        bool First = true;
        for (const auto& [K, N] : AssetHitKinds)
        {
            const bool On = !AssetKindsOff.count(K);
            const std::string Label = DiskGame::KindFolder(K) + "  " + Thousands(N) + "##chip" + std::to_string((int)K);
            const float Width = ImGui::CalcTextSize(Label.c_str(), nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2;
            if (!First)
            {
                ImGui::SameLine();
                if (ImGui::GetCursorScreenPos().x + Width > Right) ImGui::NewLine();
            }
            First = false;
            if (!On)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            }
            if (ImGui::Button(Label.c_str()))
            {
                if (On) AssetKindsOff.insert(K);
                else AssetKindsOff.erase(K);
            }
            if (!On) ImGui::PopStyleColor(2);
        }
        if (AssetHitsTotal > AssetHits.size())
            ImGui::TextDisabled("%s results in %s zones: showing the first %s, type more of the name to narrow it",
                                Thousands(AssetHitsTotal).c_str(), Thousands(AssetHitZones).c_str(), Thousands(AssetHits.size()).c_str());
        else
            ImGui::TextDisabled("%s results in %s zones", Thousands(AssetHitsTotal).c_str(), Thousands(AssetHitZones).c_str());

        const ImGuiTableFlags Flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                                      ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
        if (ImGui::BeginTable("##assets", 4, Flags, ImVec2(0, 0)))
        {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 1.0f));   // rows a line tall
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("##pick", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 3.0f);
            ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("material").x + ImGui::GetFontSize());
            ImGui::TableSetupColumn("Zone", ImGuiTableColumnFlags_WidthStretch, 1.2f);
            ImGui::TableHeadersRow();
            ImGuiListClipper Clipper;
            Clipper.Begin((int)AssetHits.size());
            while (Clipper.Step())
                for (int Row = Clipper.DisplayStart; Row < Clipper.DisplayEnd; Row++)
                {
                    const uint32_t R = AssetHits[Row];
                    const auto [Z, I] = Rows->Rows[R];
                    const DiskGame::AssetId& A = Ix->Defined(Z)[I];
                    ImGui::PushID((int)R);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    bool Picked = AssetPicks.count(A) != 0;
                    if (ImGui::Checkbox("##pick", &Picked))
                    {
                        if (Picked) AssetPicks.insert(A);
                        else AssetPicks.erase(A);
                    }
                    ImGui::TableSetColumnIndex(1);
                    if (ImGui::Selectable(A.Name.c_str(), AssetFocus == (int)R,
                                          ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick))
                    {
                        AssetFocus = (int)R;
                        // a double click previews it
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && A.K != DiskGame::Kind::Anim) StartPreview(A);
                    }
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextDisabled("%s", DiskGame::KindName(A.K));
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextDisabled("%s", Ix->Zones()[Z].c_str());
                    ImGui::PopID();
                }
            ImGui::PopStyleVar();
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
    if (!ShowPane)
    {
        ImGui::TextDisabled("Tick an asset to pick it for export; click its name to see where it is stored, double-click to preview it.");
        return;
    }
    if (Wide) ImGui::SameLine();

    // the side pane: the asset clicked (where it is, pick it, preview it), then what is picked
    ImGui::BeginChild("##assetpane", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (AssetFocus < 0 || AssetFocus >= (int)Rows->Rows.size())
        ImGui::TextDisabled("Click an asset to see where it is and preview it.");
    else
    {
        const auto [Z, I] = Rows->Rows[AssetFocus];
        const DiskGame::AssetId A = Ix->Defined(Z)[I];
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(A.Name.c_str());
        ImGui::PopTextWrapPos();
        ImGui::TextDisabled("%s", DiskGame::KindName(A.K));
        ImGui::Spacing();
        const auto In = Ix->DefinedIn(A);
        ImGui::TextDisabled(In.size() == 1 ? "Stored in" : "Stored in %zu zones", In.size());
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            for (size_t k = 0; k < In.size() && k < 6; k++)
            {
                const std::string Level = In[k] < DiskZoneLevels.size() ? DiskZoneLevels[In[k]] : std::string();
                ImGui::BulletText("%s%s%s%s", Ix->Zones()[In[k]].c_str(), Level.empty() ? "" : " (", Level.c_str(), Level.empty() ? "" : ")");
            }
            if (In.size() > 6) ImGui::TextDisabled("and %zu more", In.size() - 6);
        }
        ImGui::Spacing();
        const bool Picked = AssetPicks.count(A) != 0;
        if (ImGui::Button(Picked ? "Unpick" : "Pick", ImVec2(ImGui::GetFontSize() * 6.0f, 0)))
        {
            if (Picked) AssetPicks.erase(A);
            else AssetPicks.insert(A);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(A.K == DiskGame::Kind::Anim);
        if (ImGui::Button("Preview", ImVec2(ImGui::GetFontSize() * 6.0f, 0))) StartPreview(A);
        ImGui::EndDisabled();
        if (A.K == DiskGame::Kind::Anim && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("An animation is not previewed: exported alone, it has no model to play on");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Preview exports it alone to a temporary folder and shows it in the Viewport (or double-click its row). "
                            "Its images and materials come along, from whichever zone holds them.");
        ImGui::PopTextWrapPos();
    }
    if (!AssetPicks.empty())
    {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Picked (%s)", Thousands(AssetPicks.size()).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##picks")) AssetPicks.clear();
        size_t Shown = 0;
        for (auto It = AssetPicks.begin(); It != AssetPicks.end() && Shown < 12; Shown++)
        {
            ImGui::PushID((int)Shown);
            const bool Remove = ImGui::SmallButton("x");
            ImGui::SameLine();
            ImGui::TextDisabled("%s", DiskGame::KindName(It->K));
            ImGui::SameLine();
            ImGui::TextUnformatted(It->Name.c_str());
            ImGui::PopID();
            It = Remove ? AssetPicks.erase(It) : std::next(It);
        }
        if (AssetPicks.size() > 12) ImGui::TextDisabled("and %s more (\"Picked only\" lists them)", Thousands(AssetPicks.size() - 12).c_str());
    }
    ImGui::EndChild();
}

void LoadGamePanel::StartPreview(const DiskGame::AssetId& A)
{
    if (Busy.load()) return;
    PreviewWanted = A;
    DiskCancel.store(false);
    DiskStartedAt = ImGui::GetTime();
    StartWork(PreviewLabel, &LoadGamePanel::DoPreview);
}

void LoadGamePanel::Script(const std::string& StepJson)
{
    const nlohmann::json St = nlohmann::json::parse(StepJson, nullptr, false);
    if (!St.is_object()) return;
    if (St.contains("tab")) WantTab = St["tab"].get<std::string>() == "zone" ? 1 : 0;
    if (St.contains("query")) SetBuf(AssetQueryBuf, sizeof(AssetQueryBuf), St["query"].get<std::string>());
    if (St.contains("dismiss"))
    {
        std::lock_guard<std::mutex> Lock(Mutex);
        LastRun = {};
    }
    std::shared_ptr<const AssetRows> Rows;
    std::shared_ptr<const DiskGame::ZoneIndex> Ix;
    {
        std::lock_guard<std::mutex> Lock(Mutex);
        Rows = DiskAssets;
        Ix = DiskIndex;
        if (St.contains("zones"))
            for (const auto& Z : St["zones"])
                for (size_t i = 0; i < DiskZoneNames.size(); i++)
                    if (DiskZoneNames[i] == Z.get<std::string>()) DiskZoneFlags[i] = 1;
    }
    // an asset's row by its name (the first of that name)
    const auto Row = [&](const std::string& Name) -> int {
        if (!Rows || !Ix) return -1;
        const std::string Want = Lower(Name);
        for (size_t R = 0; R < Rows->Lower.size(); R++)
            if (Rows->Lower[R] == Want) return (int)R;
        return -1;
    };
    const auto Asset = [&](int R) { return Ix->Defined(Rows->Rows[R].first)[Rows->Rows[R].second]; };
    if (St.contains("focus")) AssetFocus = Row(St["focus"].get<std::string>());
    if (St.contains("pick"))
        for (const auto& N : St["pick"])
            if (const int R = Row(N.get<std::string>()); R >= 0) AssetPicks.insert(Asset(R));
    if (St.contains("preview"))
        if (const int R = Row(St["preview"].get<std::string>()); R >= 0) StartPreview(Asset(R));
    if (St.contains("export") && St["export"].get<bool>()) StartExport();
    if (St.contains("options")) OpenOptions = St["options"].get<bool>();
    if (St.contains("languages")) SettingsManager::SetSetting(Languages::SettingKey, St["languages"].get<std::string>());
    if (St.contains("languages_open")) OpenLanguages = St["languages_open"].get<bool>();
    if (St.contains("index") && St["index"].get<bool>() && !Busy.load())
    {
        DiskCancel.store(false);
        DiskStartedAt = ImGui::GetTime();
        StartWork(IndexLabel, &LoadGamePanel::DoIndexDiskZones);
    }
}

// Worker: PreviewWanted exported alone, with what it needs, to a temporary folder handed to the
// shell (ConsumePreview), which lists it and shows the asset
void LoadGamePanel::DoPreview()
{
    WraithX::InitializeAPI();
    const DiskGame::IDiskGame& Game = *Disk;
    const std::string Install = GameBuf;
    const DiskGame::ExportOptions Options = DiskOptions(Game, Install);
    const DiskGame::AssetId A = PreviewWanted;
    std::shared_ptr<const DiskGame::ZoneIndex> Ix;
    {
        std::lock_guard<std::mutex> Lock(Mutex);
        Ix = DiskIndex;
        LastRun = {};
        DiskFraction = 0.0f;
        DiskStep = "preview: " + A.Name;
        DiskStall.clear();
    }
    const auto Failed = [&](const std::string& Why) {
        std::lock_guard<std::mutex> Lock(Mutex);
        LastRun.Has = true;
        LastRun.Headline = "The preview of " + A.Name + " was not written";
        LastRun.ZoneErrors = { Why };
    };
    const auto In = Ix ? Ix->DefinedIn(A) : std::vector<size_t>();
    if (In.empty()) return Failed("no zone of the install index holds it");
    std::vector<std::string> Zones;
    std::string Error;
    if (!Game.ResolveZones(Install, { Ix->Zones()[In.front()] }, Options, Zones, Error)) return Failed(Error);

    DiskGame::Context C;
    std::error_code Ec;
    C.OutRoot = (fs::temp_directory_path(Ec) / "graywolf_preview" / Game.Id()).string();
    C.Only = std::make_shared<DiskGame::Selection>(DiskGame::Selection{ A });
    C.Kinds = { A.K };
    // the images as the game stores them (DDS), whatever the export setting: no full size decode
    // and PNG encode for the viewer to decode again (Cold War's skybox: 1.9 s -> 0.6 s)
    SettingsManager::SetRunSetting("exportimg", "DDS");
    const DiskGame::Totals T = DiskGame::Export(Game, Options, Zones, C, [&](const DiskGame::Progress& P) {
        if (P.FromWatchdog) return;
        std::lock_guard<std::mutex> Lock(Mutex);
        DiskFraction = P.Count ? (float)P.Done / (float)P.Count : DiskFraction;
        DiskStep = "Preview: " + A.Name + (P.Current ? " (" + DiskGame::KindFolder(*P.Current) + ")" : std::string(" (reading the zone)"));
    }, &DiskCancel);
    SettingsManager::ClearRunSetting("exportimg");
    size_t Written = 0;
    for (const auto& [K, V] : T.Kinds) Written += V.Written;
    if (T.Cancelled) return;
    if (Written == 0)
        return Failed(!T.ZoneErrors.empty() ? T.ZoneErrors.front() : !T.Failures.empty() ? T.Failures.front() : "its reader wrote no file for it");
    {
        std::lock_guard<std::mutex> Lock(Mutex);
        PreviewDir = C.OutRoot;
        PreviewName = A.Name;
        Status = "preview of " + A.Name + " written to " + C.OutRoot;
    }
    PreviewReady.store(true);
}
