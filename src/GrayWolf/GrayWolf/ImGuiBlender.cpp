#include "stdafx.h"

#include "ImGuiBlender.h"

#include "BlenderLauncher.h"
#include "ImGuiDialogs.h"
#include "SettingsManager.h"
#include "UiAsync.h"

#include "imgui.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <vector>

namespace
{
    // what the buttons read every frame, looked up again on a worker every few seconds: Blender's
    // detection walks Program Files, a folder's maps a directory listing
    constexpr double Refresh = 2.0;

    char PathBuf[1024] = {};   // Settings > Blender's field
    bool PathLoaded = false;

    UiAsync::Cached<Blender::Exe> Detected;
    Blender::Exe BlenderExe(bool* Pending = nullptr)
    {
        const std::string Set = SettingsManager::GetSetting("blender_path", "");
        return Detected.Get(Set, 5.0, [Set] { return Blender::Find(Set); }, Pending);
    }

    struct MapState
    {
        Blender::Map Map;
        bool Script = false;
        bool Blend = false;
    };

    // per map (its .blend's path): what the last click did, shown under its buttons; written by
    // the launch's worker
    std::mutex ResultsMutex;
    std::map<std::string, std::string> Results;
    std::string ResultOf(const std::string& Blend)
    {
        std::lock_guard<std::mutex> Lock(ResultsMutex);
        const auto It = Results.find(Blend);
        return It == Results.end() ? std::string() : It->second;
    }

    MapState StateOf(const Blender::Map& M) { return { M, M.HasScript(), M.HasBlend() }; }

    // a map file's state, and a folder's maps, as last looked up
    struct OneMap
    {
        bool IsMap = false;
        MapState State;
    };
    UiAsync::Cached<OneMap> Single;
    UiAsync::Cached<std::vector<MapState>> Listed;

    // Blender started on a worker: its detection and CreateProcess are not the frame's
    void Launch(const MapState& S, bool Reimport)
    {
        Blender::LaunchOptions O;
        O.Reimport = Reimport;
        O.Use = BlenderExe();
        {
            std::lock_guard<std::mutex> Lock(ResultsMutex);
            Results[S.Map.Blend] = "starting Blender...";
        }
        UiAsync::Run("blender " + S.Map.Name, [S, O] {
            std::string Did, Error;
            const bool Ok = Blender::Open(S.Map, O, Did, Error);
            std::lock_guard<std::mutex> Lock(ResultsMutex);
            Results[S.Map.Blend] = Ok ? Did : Error;
        });
        Single.Refresh();
        Listed.Refresh();
    }

    // the buttons of one map, on one line
    void Buttons(const MapState& S)
    {
        const bool Have = !BlenderExe().Path.empty();
        ImGui::PushID(S.Map.Blend.c_str());
        ImGui::BeginDisabled(!Have || (!S.Blend && !S.Script));
        if (ImGui::SmallButton("Open in Blender")) Launch(S, false);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            if (!Have) ImGui::SetTooltip("Blender not found: install it, or set blender.exe in Settings > Blender");
            else if (S.Blend) ImGui::SetTooltip("Opens %s", S.Map.Blend.c_str());
            else ImGui::SetTooltip("Imports the map into Blender (its _import.py) and saves it as\n%s:\nnext time, that .blend opens directly",
                                   S.Map.Blend.c_str());
        }
        if (S.Blend)
        {
            ImGui::SameLine();
            ImGui::BeginDisabled(!Have || !S.Script);
            if (ImGui::SmallButton("Re-import")) Launch(S, true);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(S.Script ? "Imports the map again (after a new export) and overwrites its .blend"
                                           : "No import script beside the map: export it again");
        }
        ImGui::PopID();
    }
}

void DrawBlenderSettingsMenu()
{
    // collected outside the menu: the dialog takes the focus, which closes it
    if (std::string Picked; UiAsync::Picked("blender.exe", Picked))
    {
        SettingsManager::SetSetting("blender_path", Picked);
        SetBuf(PathBuf, sizeof(PathBuf), Picked);
    }
    if (!ImGui::BeginMenu("Settings")) return;   // appended to the Settings menu ImGuiUpdates opens
    if (ImGui::BeginMenu("Blender"))
    {
        if (!PathLoaded || !ImGui::IsAnyItemActive()) SetBuf(PathBuf, sizeof(PathBuf), SettingsManager::GetSetting("blender_path", ""));
        PathLoaded = true;
        ImGui::TextUnformatted("blender.exe (leave empty to detect it)");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 30.0f);
        ImGui::InputTextWithHint("##blenderpath", "Detected from BLENDER, Program Files, Steam, registry, PATH", PathBuf, sizeof(PathBuf));
        if (ImGui::IsItemDeactivatedAfterEdit()) SettingsManager::SetSetting("blender_path", PathBuf);
        bool Pending = false;
        const Blender::Exe B = BlenderExe(&Pending);
        if (ImGui::Button("Browse\xE2\x80\xA6"))
        {
            const std::string Start = B.Path.substr(0, std::min(B.Path.size(), B.Path.find_last_of("\\/")));
            UiAsync::PickOpenFile("blender.exe", "Select blender.exe", { { L"Blender", L"blender.exe;blender-launcher.exe" } }, Start);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(PathBuf[0] == 0);
        if (ImGui::Button("Detect"))
        {
            SettingsManager::SetSetting("blender_path", "");
            PathBuf[0] = 0;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Clear the path above and find Blender automatically");
        if (Pending) ImGui::TextDisabled("Looking for Blender\xE2\x80\xA6");
        else if (B.Path.empty()) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Blender not found");
        else ImGui::TextDisabled("Using %s (%s)", B.Path.c_str(), B.From.c_str());
        ImGui::EndMenu();
    }
    ImGui::EndMenu();
}

void DrawOpenInBlender(const std::string& MapFile)
{
    const OneMap M = Single.Get(MapFile, Refresh, [MapFile] {
        OneMap Out;
        std::string Error;
        Blender::Map Map;
        if (Blender::MapOf(MapFile, Map, Error)) Out.State = StateOf(Map);
        Out.IsMap = Out.State.Script || Out.State.Blend;
        return Out;
    });
    if (!M.IsMap) return;
    Buttons(M.State);
    if (const std::string R = ResultOf(M.State.Map.Blend); !R.empty()) ImGui::TextDisabled("%s", R.c_str());
}

void DrawMapsInBlender(const std::string& Folder)
{
    const std::vector<MapState> Maps = Listed.Get(Folder, Refresh, [Folder] {
        std::vector<MapState> Out;
        if (!Folder.empty())
            for (const Blender::Map& M : Blender::MapsIn(Folder)) Out.push_back(StateOf(M));
        return Out;
    });
    if (Maps.empty()) return;

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Maps");
    ImGui::SameLine();
    ImGui::TextDisabled("(Open exported maps in Blender: imported once, then saved as a .blend beside the .glb)");
    const float Row = ImGui::GetFrameHeightWithSpacing();
    const float Height = Row * (float)std::min<size_t>(Maps.size(), 6) + Row;
    const ImGuiTableFlags Flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("blendermaps", 3, Flags, ImVec2(0, Height))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Map", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Blender", ImGuiTableColumnFlags_WidthFixed,
                            ImGui::CalcTextSize("Open in BlenderRe-import").x + ImGui::GetStyle().FramePadding.x * 4 + ImGui::GetStyle().ItemSpacing.x);
    ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch, 1.5f);
    ImGui::TableHeadersRow();
    for (const MapState& S : Maps)
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(S.Map.Name.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", S.Map.Glb.c_str());
        ImGui::TableSetColumnIndex(1);
        Buttons(S);
        ImGui::TableSetColumnIndex(2);
        if (const std::string R = ResultOf(S.Map.Blend); !R.empty()) ImGui::TextDisabled("%s", R.c_str());
        else ImGui::TextDisabled(S.Blend ? ".blend saved" : "Not imported yet");
    }
    ImGui::EndTable();
}
