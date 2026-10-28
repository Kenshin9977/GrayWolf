#pragma once

// What the Load Game window's files share: the labels of its workers, the run's options as the
// settings give them, and a few formats.

#include "DiskGame.h"
#include "ExportNaming.h"
#include "SettingsManager.h"

#include "imgui.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <string>

namespace LoadGameUi
{
    inline constexpr const char* ExportLabel = "disk export";
    inline constexpr const char* ListLabel = "zone list";
    inline constexpr const char* IndexLabel = "install index";
    inline constexpr const char* PreviewLabel = "preview";
    inline constexpr const char* SizesLabel = "size samples";

    inline size_t Total(const std::map<DiskGame::Kind, size_t>& Counts)
    {
        size_t N = 0;
        for (const auto& [K, C] : Counts) N += C;
        return N;
    }

    inline std::string SettingKey(const DiskGame::IDiskGame& Game, const DiskGame::Setting& S)
    {
        return std::string("disk_") + Game.Id() + "_" + S.Key;
    }

    // What the game's hooks read: the install and the game's saved settings, the rest at its default.
    inline DiskGame::ExportOptions DiskOptions(const DiskGame::IDiskGame& Game, const std::string& Install)
    {
        DiskGame::ExportOptions O;
        O.Install = Install;
        for (const DiskGame::Setting* S : Game.AllSettings())
            O.Settings[S->Key] = SettingsManager::GetSetting(SettingKey(Game, *S), S->Default);
        return O;
    }

    // 75 as "1:15", 3725 as "1:02:05"
    inline std::string Clock(double Seconds)
    {
        const long long S = (long long)Seconds;
        char B[32];
        if (S >= 3600) snprintf(B, sizeof(B), "%lld:%02lld:%02lld", S / 3600, S / 60 % 60, S % 60);
        else snprintf(B, sizeof(B), "%lld:%02lld", S / 60, S % 60);
        return B;
    }

    // What a zone list was read for: another game, folder or setting reads it again.
    inline std::string ListedFor(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& O)
    {
        std::string Key = std::string(Game.Id()) + "|" + O.Install;
        for (const auto& [K, V] : O.Settings) Key += "|" + K + "=" + V;
        return Key;
    }

    // A turning arc the height of a line, then the next item on the same line: work under way
    inline void Spinner()
    {
        const float H = ImGui::GetTextLineHeight();
        const float R = H * 0.38f;
        const ImVec2 P = ImGui::GetCursorScreenPos();
        const ImVec2 C(P.x + R + 1.0f, P.y + H * 0.5f);
        ImDrawList* Draw = ImGui::GetWindowDrawList();
        const float Start = (float)ImGui::GetTime() * 6.0f;
        constexpr int Steps = 24;
        Draw->PathClear();
        for (int i = 0; i <= Steps; i++)
        {
            const float A = Start + 4.4f * (float)i / (float)Steps;
            Draw->PathLineTo(ImVec2(C.x + cosf(A) * R, C.y + sinf(A) * R));
        }
        Draw->PathStroke(ImGui::GetColorU32(ImGuiCol_CheckMark), 0, R * 0.38f);
        ImGui::Dummy(ImVec2(R * 2.0f + 2.0f, H));
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    }

    // What a worker is reading, under a spinner: the tabs while the game's zones and index come in
    inline void Loading(const std::string& What)
    {
        ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight()));
        Spinner();
        ImGui::TextUnformatted(What.c_str());
    }

    // The kind's folder name in the singular for one ("1 model"), as is for more ("3 models")
    inline std::string KindCount(DiskGame::Kind K, size_t N)
    {
        std::string Word = DiskGame::KindFolder(K);
        if (N == 1 && Word.size() > 1 && Word.back() == 's') Word.pop_back();
        return ExportNaming::Thousands(N) + " " + Word;
    }
}
