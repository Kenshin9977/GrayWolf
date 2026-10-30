#include "stdafx.h"

#include "ImGuiUpdates.h"

#include "AppLog.h"
#include "SettingsManager.h"
#include "Updater.h"

#include "imgui.h"
#include "imgui_internal.h" // BeginViewportSideBar

namespace
{
    bool BannerDismissed = false;
    std::string ApplyError;

    bool SettingOn(const char* Key, const char* Default) { return SettingsManager::GetSetting(Key, Default) == "true"; }
}

void DrawUpdatesMenu()
{
    if (!ImGui::BeginMenu("Settings")) return;
    if (ImGui::BeginMenu("Updates"))
    {
        bool Auto = SettingOn("auto_update", "true");
        if (ImGui::MenuItem("Check for updates at startup", nullptr, &Auto))
            SettingsManager::SetSetting("auto_update", Auto ? "true" : "false");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("At startup: newer hash name packages (package_index) are installed,\n"
                              "a newer GrayWolf is only announced (Download / Later).");
        bool Gh = SettingOn("update_use_gh_token", "false");
        if (ImGui::MenuItem("Use the GitHub CLI's token (gh auth token)", nullptr, &Gh))
            SettingsManager::SetSetting("update_use_gh_token", Gh ? "true" : "false");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("For a private repository: the token is read from gh at each check,\n"
                              "sent to the GitHub API only, never saved. GRAYWOLF_GITHUB_TOKEN is used first.");
        ImGui::Separator();
        const Updater::Background::View V = Updater::Background::Get();
        if (ImGui::MenuItem(V.Busy ? "Checking\xE2\x80\xA6" : "Check for updates now", nullptr, false, !V.Busy))
        {
            BannerDismissed = false;
            Updater::Background::Start(false);
        }
        ImGui::TextDisabled("Installed version: %s", Updater::AppVersion().c_str());
        ImGui::TextDisabled("Repository: %s", SettingsManager::GetSetting("update_repo", "Kenshin9977/GrayWolf").c_str());
        if (!V.Line.empty()) ImGui::TextDisabled("%s", V.Line.c_str());
        ImGui::EndMenu();
    }
    ImGui::EndMenu();
}

bool DrawUpdateBanner()
{
    const Updater::Background::View V = Updater::Background::Get();
    if (V.AppVersion.empty() || BannerDismissed) return false;

    bool Restart = false;
    ImGuiViewport* Vp = ImGui::GetMainViewport();
    ImGui::PushStyleColor(ImGuiCol_MenuBarBg, ImVec4(0.20f, 0.33f, 0.50f, 1.0f));
    if (ImGui::BeginViewportSideBar("##update", Vp, ImGuiDir_Up, ImGui::GetFrameHeight(),
                                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar))
    {
        if (ImGui::BeginMenuBar())
        {
            if (!ApplyError.empty())
                ImGui::Text("Update failed to start: %s", ApplyError.c_str());
            else if (V.AppStaged)
                ImGui::Text("GrayWolf %s is downloaded and verified.", V.AppVersion.c_str());
            else if (V.Busy)
                ImGui::Text("Downloading GrayWolf %s\xE2\x80\xA6", V.AppVersion.c_str());
            else if (!V.Error.empty())
                ImGui::Text("Download failed: %s", V.Error.c_str());
            else
                ImGui::Text("GrayWolf %s is available (you have %s).", V.AppVersion.c_str(), Updater::AppVersion().c_str());

            if (V.AppStaged && ApplyError.empty())
            {
                if (ImGui::SmallButton("Restart now"))
                {
                    std::string Msg;
                    if (Updater::Background::ApplyAndRestart(Msg)) Restart = true;
                    else ApplyError = Msg;
                }
            }
            else if (!V.Busy && !V.AppStaged)
            {
                if (ImGui::SmallButton(V.Error.empty() ? "Download" : "Retry")) Updater::Background::DownloadApp();
            }
            if (ImGui::SmallButton("Later")) BannerDismissed = true;
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    return Restart;
}
