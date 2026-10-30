#include "stdafx.h"

// The shell's frame around its panels: the main menu bar, the status bar, the dock layout, the filter help.
#include "ImGuiShell.Internal.h"
#include "AppLog.h"           // session log, verbose
#include "UiAsync.h"           // shell opens off the frame
#include "Strings.h"
#include "SettingsManager.h"   // exportsnd
#include "ImGuiUpdates.h"      // Settings > Updates, the new-version banner
#include "ImGuiBlender.h"      // Settings > Blender

#include "imgui.h"
#include "imgui_internal.h"   // DockBuilder* for the initial layout

#include <algorithm>

namespace ShellUi
{
    // Settings > Sounds: the format every export writes sounds in (setting exportsnd, what
    // --audio-format sets for one run; ExportWriters::WriteSound)
    static void DrawSoundSettingsMenu()
    {
        if (!ImGui::BeginMenu("Settings")) return;   // appended to the Settings menu ImGuiUpdates opens
        if (ImGui::BeginMenu("Sounds"))
        {
            // Opus by default (ExportWriters::SoundFormatSetting)
            const std::string Current = Strings::ToUpper(SettingsManager::GetSetting("exportsnd", "OPUS"));
            const struct { const char* Value; const char* Label; const char* Tip; } Formats[] = {
                { "OPUS", "Opus (default: small, lossy)",
                  "The game's own Opus packets kept as they are when it stores Opus; any other sound decoded and encoded "
                  "(64 kb/s a channel, 48 kHz). About a tenth of WAV's size." },
                { "WAV", "WAV (lossless)", "As the game keeps them (PCM, ADPCM, MP3), or decoded to 16-bit PCM. The largest files." },
                { "FLAC", "FLAC (lossless, compressed)", "Lossless, about half of WAV's size." },
            };
            for (const auto& F : Formats)
            {
                if (ImGui::MenuItem(F.Label, nullptr, Current == F.Value)) SettingsManager::SetSetting("exportsnd", F.Value);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", F.Tip);
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    bool DrawMainMenuBar(bool& OpenHelp)
    {
        bool Quit = false;
        if (ImGui::BeginMainMenuBar())
        {
            if (ImGui::BeginMenu("File")) { if (ImGui::MenuItem("Exit")) Quit = true; ImGui::EndMenu(); }
            DrawUpdatesMenu();
            DrawBlenderSettingsMenu();
            DrawScreenshotSettingsMenu();
            DrawSoundSettingsMenu();
            if (ImGui::BeginMenu("Help"))
            {
                if (ImGui::MenuItem("Search syntax")) OpenHelp = true;
                ImGui::Separator();
                if (ImGui::MenuItem("Open session log")) UiAsync::OpenInShell(AppLog::SessionPath());
                bool Verbose = AppLog::Verbose();
                if (ImGui::MenuItem("Verbose log (every step timed)", nullptr, &Verbose)) AppLog::SetVerbose(Verbose);
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }
        // a newer GrayWolf: Download / Later, then Restart now (the helper replaces the files)
        if (DrawUpdateBanner()) Quit = true;
        return Quit;
    }

    // Bottom status bar (trims the work area before the dockspace claims the rest). Uses the
    // menu-bar idiom so the text is vertically centred in the frame height and never clipped.
    void DrawStatusBar(const ShellState& S)
    {
        const auto& Models = S.Catalog.Models;
        ImGuiViewport* MainVp = ImGui::GetMainViewport();
        if (ImGui::BeginViewportSideBar("##status", MainVp, ImGuiDir_Down, ImGui::GetFrameHeight(),
                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar))
        {
            if (ImGui::BeginMenuBar())
            {
                ImGui::Text("%d assets", (int)Models.size());
                ImGui::TextDisabled("|");
                if (S.Selected >= 0 && S.Selected < (int)Models.size()) ImGui::Text("Selected: %s", Models[S.Selected].Name.c_str());
                else ImGui::TextDisabled("No selection");
                const char* Hint = "drag: orbit   \xC2\xB7   middle drag: pan   \xC2\xB7   wheel: zoom";
                ImGui::SameLine(std::max(0.0f, ImGui::GetWindowWidth() - ImGui::CalcTextSize(Hint).x - 14.0f));
                ImGui::TextDisabled("%s", Hint);
                ImGui::EndMenuBar();
            }
        }
        ImGui::End();
    }

    // First frame: split the dockspace so Extract and Library fill the left and Viewport the rest, all
    // filling the window (otherwise ImGui leaves them as small floating windows in the corner).
    void BuildDockLayout(unsigned int DockId)
    {
        static bool LayoutBuilt = false;
        if (LayoutBuilt) return;
        LayoutBuilt = true;
        ImGui::DockBuilderRemoveNode(DockId);
        ImGui::DockBuilderAddNode(DockId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(DockId, ImGui::GetMainViewport()->Size);
        ImGuiID Left = 0, Right = 0;
        ImGui::DockBuilderSplitNode(DockId, ImGuiDir_Left, 0.38f, &Left, &Right);
        // Tabbed together: reading a game and browsing what was exported are the same job seen
        // from two ends, and they never need to be visible at once.
        ImGui::DockBuilderDockWindow(Theme::ExtractWindow, Left);
        ImGui::DockBuilderDockWindow(Theme::LibraryWindow, Left);
        ImGui::DockBuilderDockWindow("Viewport", Right);
        ImGui::DockBuilderFinish(DockId);
    }

    // Filter help dialog (opened from the '?' button or the Help menu)
    void DrawFilterHelp(bool OpenHelp)
    {
        if (OpenHelp) ImGui::OpenPopup("Search syntax");
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Search syntax", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted("Search matches the asset name");
            ImGui::Separator();
            ImGui::BulletText("space   =   AND  (every term must appear)");
            ImGui::BulletText("!term   =   the term must NOT appear");
            ImGui::BulletText("case-insensitive");
            ImGui::Spacing(); ImGui::TextDisabled("Examples");
            ImGui::BulletText("richtofen              names containing \"richtofen\"");
            ImGui::BulletText("zombie !head           \"zombie\" and not \"head\"");
            ImGui::BulletText("moon militarypolice    both terms");
            ImGui::Spacing();
            ImGui::TextDisabled("Tip: sort by any column by clicking its header.");
            ImGui::TextDisabled("Number ranges (bones, meshes, materials, size) are under Filters in the toolbar.");
            ImGui::Spacing();
            if (ImGui::Button("Got it", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
}
