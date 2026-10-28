// The window's lifecycle, the install scan, the game's card and the layout: the card, the two ways
// to pick (ImGuiLoadGamePanel.Assets.cpp, ImGuiLoadGamePanel.Disk.cpp), the export bar.

#include "stdafx.h"

#include "ImGuiLoadGamePanel.h"
#include "ImGuiLoadGamePanel.Common.h"
#include "ImGuiDialogs.h"
#include "LauncherIcons.h"
#include "DiskGame.h"
#include "SettingsManager.h"

#include "imgui.h"
#include "imgui_internal.h"   // ImGuiComboFlags_CustomPreview

#include <cctype>

using namespace LoadGameUi;

namespace
{
    const ImVec4 Green(0.44f, 0.81f, 0.54f, 1.0f), Amber(1.0f, 0.75f, 0.2f, 1.0f);

    // A dot of Color before the text that follows on the line
    void Dot(const ImVec4& Color)
    {
        const float H = ImGui::GetTextLineHeight();
        const ImVec2 P = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(P.x + H * 0.35f, P.y + H * 0.55f), H * 0.25f, ImGui::GetColorU32(Color));
        ImGui::Dummy(ImVec2(H * 0.7f, H));
        ImGui::SameLine(0.0f, 4.0f);
    }
}

LoadGamePanel::LoadGamePanel()
{
    // Last session's choices first; they are what the user is most likely to want again.
    SetBuf(GameBuf, sizeof(GameBuf), SettingsManager::GetSetting("game_folder", ""));
    SetBuf(DiskOutBuf, sizeof(DiskOutBuf), AbsoluteFolder(SettingsManager::GetSetting("disk_out", "exported_files")));
    const std::string Kinds = "," + SettingsManager::GetSetting("disk_kinds", "image") + ",";
    for (const DiskGame::KindInfo& K : DiskGame::Kinds)
        DiskKindFlags.push_back(Kinds.find(std::string(",") + K.Name + ",") != std::string::npos);
    SetBuf(DiskNameFilterBuf, sizeof(DiskNameFilterBuf), SettingsManager::GetSetting("disk_name_filter", ""));
    SetBuf(DiskZoneFilterBuf, sizeof(DiskZoneFilterBuf), SettingsManager::GetSetting("disk_zone_filter", ""));
    ExportByAsset = SettingsManager::GetSetting("disk_pick_by", "asset") != "zone";
    WantTab = ExportByAsset ? 0 : 1;
    Status = "no game loaded";
}

LoadGamePanel::~LoadGamePanel()
{
    if (Worker.joinable()) Worker.join();
}

void LoadGamePanel::Persist()
{
    SettingsManager::SetSetting("game_folder", GameBuf);
    SettingsManager::SetSetting("disk_out", DiskOutBuf);
    std::string Kinds;
    for (size_t i = 0; i < DiskKindFlags.size(); i++)
        if (DiskKindFlags[i]) Kinds += (Kinds.empty() ? "" : ",") + std::string(DiskGame::Kinds[i].Name);
    SettingsManager::SetSetting("disk_kinds", Kinds);
    SettingsManager::SetSetting("disk_name_filter", DiskNameFilterBuf);
    SettingsManager::SetSetting("disk_zone_filter", DiskZoneFilterBuf);
    SettingsManager::SetSetting("disk_pick_by", ExportByAsset ? "asset" : "zone");
    // the ticked zones, per game; not before the list was read, which would clear a saved one
    if (Disk != nullptr && (DiskZonesFor + "|").rfind(std::string(Disk->Id()) + "|" + GameBuf + "|", 0) == 0)
    {
        std::string Zones;
        for (size_t i = 0; i < DiskZoneNames.size(); i++)
            if (DiskZoneFlags[i]) Zones += (Zones.empty() ? "" : ",") + DiskZoneNames[i];
        SettingsManager::SetSetting(std::string("disk_zones_") + Disk->Id(), Zones);
    }
    // SetSetting already flushes the whole settings file on every call.
}

void LoadGamePanel::DoScan()
{
    auto Found = GameInstalls::Detect();

    std::lock_guard<std::mutex> Lock(Mutex);
    Installs = std::move(Found);
    InstallIndex = -1;
    ScannedOnce = true;
    Status = Installs.empty()
        ? "no game detected - set the folder manually"
        : (std::to_string(Installs.size()) + " game(s) detected");
}

void LoadGamePanel::ApplyInstall(const GameInstalls::Install& I)
{
    SetBuf(GameBuf, sizeof(GameBuf), I.Path);
    InstallDiskId = I.DiskId;
    InstallPath = I.Path;
    DiskChosenFor.clear();
}

void LoadGamePanel::StartWork(const char* Label, void (LoadGamePanel::*Fn)())
{
    if (Busy.load()) return;
    if (Worker.joinable()) Worker.join();

    BusyLabel = Label;
    Busy.store(true);
    Done.store(false);
    Worker = std::thread([this, Fn] { (this->*Fn)(); Busy.store(false); Done.store(true); });
}

void LoadGamePanel::JoinIfDone()
{
    if (Done.load() && !Busy.load() && Worker.joinable())
    {
        Worker.join();
        Done.store(false);
    }
}

bool LoadGamePanel::ConsumePreview(std::string& Dir, std::string& Name)
{
    if (!PreviewReady.exchange(false)) return false;
    std::lock_guard<std::mutex> Lock(Mutex);
    Dir = PreviewDir;
    Name = PreviewName;
    return true;
}

// The detected installs, the game folder and the reader it is read with
void LoadGamePanel::DrawSetup(ID3D11Device* Dev)
{
    ImGui::TextUnformatted("Detected installs");
    ImGui::SetNextItemWidth(-90);
    {
        std::lock_guard<std::mutex> Lock(Mutex);
        // where a game was installed from: its launcher's own icon (LauncherIcons), then the label
        const float Line = ImGui::GetTextLineHeight();
        const auto Row = [&](const GameInstalls::Install& I) {
            if (ID3D11ShaderResourceView* Icon = LauncherIcons::For(Dev, I.Source))
                ImGui::Image((ImTextureID)(intptr_t)Icon, ImVec2(Line, Line));
            else
                ImGui::Dummy(ImVec2(Line, Line));
            ImGui::SameLine(0.0f, Line * 0.5f);
            ImGui::TextUnformatted(I.Label.c_str());
        };
        const bool Picked = InstallIndex >= 0 && InstallIndex < (int)Installs.size();
        const char* InstallPreview = !ScannedOnce ? "(not scanned yet)"
            : Installs.empty() ? "(none found)"
            : Picked ? "" : "select...";

        if (ImGui::BeginCombo("##installs", InstallPreview, Picked ? ImGuiComboFlags_CustomPreview : 0))
        {
            for (int i = 0; i < (int)Installs.size(); i++)
            {
                const auto& I = Installs[i];
                // one ID per row, whatever the label: two installs of one game share a title
                ImGui::PushID(i);
                const bool Clicked = ImGui::Selectable("##install", InstallIndex == i, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, Line));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s  ·  %s", I.Source.c_str(), I.Path.c_str());
                ImGui::SetCursorScreenPos(ImGui::GetItemRectMin());
                Row(I);
                if (Clicked)
                {
                    InstallIndex = i;
                    ApplyInstall(I);
                    Persist();
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (Picked && ImGui::BeginComboPreview())
        {
            Row(Installs[InstallIndex]);
            ImGui::EndComboPreview();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Rescan")) StartWork("scan", &LoadGamePanel::DoScan);

    ImGui::TextUnformatted("Game folder");
    ImGui::SetNextItemWidth(-90);
    ImGui::InputText("##game", GameBuf, sizeof(GameBuf));
    // saved, and the folder's game looked for, once the field is left: not on every key
    GameEditing = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit()) Persist();
    ImGui::SameLine();
    if (ImGui::Button("Browse##game")) UiAsync::PickFolder("loadgame.folder", "Select the game install folder", GameBuf);
    DrawReadWith();
}

void LoadGamePanel::DrawGameCard(ID3D11Device* Dev)
{
    if (Disk == nullptr)
    {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.25f);
        ImGui::TextUnformatted("Pick a game");
        ImGui::PopFont();
        ImGui::TextDisabled("GrayWolf reads the game's files: the game is never launched.");
        ImGui::Spacing();
        DrawSetup(Dev);
        if (Detecting)
            ImGui::TextDisabled("looking for the game's files...");
        else if (GameBuf[0] != 0)
            ImGui::TextWrapped("No disk reader recognises this folder: pick one under \"Read with\".");
        return;
    }

    // the game's own exe icon (looked for once per folder, on a worker), else a badge of the
    // reader's first letters
    const float Side = ImGui::GetTextLineHeight() * 2.4f;
    const ImVec2 At = ImGui::GetCursorScreenPos();
    ImDrawList* Draw = ImGui::GetWindowDrawList();
    const std::string Folder = GameBuf;
    const std::string Exe = GameExeOf.Get(Folder, 1e30, [Folder] { return GameInstalls::GameExe(Folder); });
    if (ID3D11ShaderResourceView* Icon = LauncherIcons::ForFile(Dev, Exe))
        Draw->AddImage((ImTextureID)(intptr_t)Icon, At, ImVec2(At.x + Side, At.y + Side));
    else
    {
        std::string Badge;
        for (const char* C = Disk->Id(); *C != 0 && Badge.size() < 4; C++) Badge += (char)toupper((unsigned char)*C);
        Draw->AddRectFilled(At, ImVec2(At.x + Side, At.y + Side), ImGui::GetColorU32(ImGuiCol_Header), 6.0f);
        const ImVec2 Text = ImGui::CalcTextSize(Badge.c_str());
        Draw->AddText(ImVec2(At.x + (Side - Text.x) * 0.5f, At.y + (Side - Text.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), Badge.c_str());
    }
    ImGui::Dummy(ImVec2(Side, Side));
    ImGui::SameLine(0.0f, 12.0f);

    // the title, the launcher and the folder
    std::string Source;
    {
        std::lock_guard<std::mutex> Lock(Mutex);
        for (const auto& I : Installs)
            if (I.Path == GameBuf) Source = I.Source;
    }
    std::string Index;
    bool Indexed = false;
    {
        std::lock_guard<std::mutex> Lock(Mutex);
        if (DiskIndex)
        {
            Indexed = true;
            Index = "Indexed: " + ExportNaming::Thousands(DiskIndex->Entries) + " assets in " + ExportNaming::Thousands(DiskIndex->Zones().size()) + " zones";
            if (!DiskIndexDate.empty()) Index += " (" + DiskIndexDate + ")";
        }
    }
    const float Right = ImGui::CalcTextSize("Change game").x + ImGui::GetStyle().FramePadding.x * 2 + 8.0f;
    const bool Reading = Busy.load() && (BusyLabel == ListLabel || BusyLabel == IndexLabel);
    if (Reading) Index = BusyLabel == IndexLabel ? "Indexing the install..." : "Reading the install...";
    else if (!Indexed) Index = "Not indexed yet";
    const float IndexWidth = ImGui::CalcTextSize(Index.c_str()).x + ImGui::GetTextLineHeight();
    ImGui::BeginGroup();
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.15f);
    ImGui::TextUnformatted(Disk->Title());
    ImGui::PopFont();
    ImGui::TextDisabled("%s%s%s", Source.c_str(), Source.empty() ? "" : "  ·  ", GameBuf);
    ImGui::EndGroup();

    // the index's state and "Change game", right-aligned while they fit
    const float Avail = ImGui::GetContentRegionMax().x;
    ImGui::SameLine();
    const float X = Avail - Right - IndexWidth - 16.0f;
    if (X > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(X);
    else ImGui::NewLine();   // a narrow window: under the title
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(0, Side * 0.5f - ImGui::GetTextLineHeight() * 0.5f - ImGui::GetStyle().ItemSpacing.y));
    if (Reading) { Spinner(); DrewIndicator = true; }
    else Dot(Indexed ? Green : Amber);
    ImGui::TextUnformatted(Index.c_str());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(Indexed ? "What each zone of the install holds, listed once and kept for next time:\n"
                                    "built again by itself when the install or GrayWolf's tables change"
                                  : "Open \"By asset\" to list what each zone holds (once, kept for next time)");
    ImGui::EndGroup();
    ImGui::SameLine();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Side * 0.5f - ImGui::GetFrameHeight() * 0.5f);
    if (ImGui::Button("Change game")) ImGui::OpenPopup("##setup");
    ImGui::SetNextWindowSizeConstraints(ImVec2(560, 0), ImVec2(900, FLT_MAX));
    if (ImGui::BeginPopup("##setup"))
    {
        DrawSetup(Dev);
        ImGui::EndPopup();
    }
}

void LoadGamePanel::Draw(ID3D11Device* Dev)
{
    JoinIfDone();
    std::string Picked;
    if (UiAsync::Picked("loadgame.folder", Picked)) { SetBuf(GameBuf, sizeof(GameBuf), Picked); Persist(); }
    if (UiAsync::Picked("loadgame.output", Picked)) { SetBuf(DiskOutBuf, sizeof(DiskOutBuf), Picked); Persist(); }

    // One automatic scan on the first frame, on the worker so the window still paints: the sweep
    // takes about two seconds.
    if (!ScannedOnce && !AutoScanStarted)
    {
        AutoScanStarted = true;
        StartWork("scan", &LoadGamePanel::DoScan);
    }

    DrewIndicator = false;
    ImGui::Begin("Load Game");
    if (DiskChosenFor != GameBuf) ChooseDiskGame();
    const bool Busyish = Busy.load();

    ImGui::BeginDisabled(Busyish);
    DrawGameCard(Dev);
    ImGui::EndDisabled();

    if (Disk == nullptr)
    {
        std::lock_guard<std::mutex> Lock(Mutex);
        if (!Status.empty()) ImGui::TextDisabled("%s", Status.c_str());
        ImGui::End();
        return;
    }
    ImGui::Separator();

    // the two ways to pick, above the export bar (its height: last frame's)
    if (ImGui::BeginTabBar("##pickby"))
    {
        const auto Tab = [&](const char* Label, int Which, bool ByAsset, void (LoadGamePanel::*Body)()) {
            const ImGuiTabItemFlags Flags = WantTab == Which ? ImGuiTabItemFlags_SetSelected : 0;
            if (!ImGui::BeginTabItem(Label, nullptr, Flags)) return;
            if (ExportByAsset != ByAsset && WantTab < 0)
            {
                ExportByAsset = ByAsset;
                Persist();
            }
            ImGui::BeginChild("##pick", ImVec2(0, -BarHeight));
            ImGui::BeginDisabled(Busyish);
            (this->*Body)();
            ImGui::EndDisabled();
            ImGui::EndChild();
            ImGui::EndTabItem();
        };
        Tab("By asset", 0, true, &LoadGamePanel::DrawAssetTab);
        Tab("By zone / map", 1, false, &LoadGamePanel::DrawZoneTab);
        ImGui::EndTabBar();
    }
    if (WantTab >= 0)
    {
        ExportByAsset = WantTab == 0;
        WantTab = -1;
    }

    const float Top = ImGui::GetCursorPosY();
    DrawExportBar();
    BarHeight = ImGui::GetCursorPosY() - Top + ImGui::GetStyle().ItemSpacing.y;

    ImGui::End();
}
