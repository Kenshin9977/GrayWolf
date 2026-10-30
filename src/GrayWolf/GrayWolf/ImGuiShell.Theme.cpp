#include "stdafx.h"

// The theme (ImGuiTheme.h): fonts, colours, and the widgets every panel draws alike.
#include "ImGuiTheme.h"

#include <windows.h>

namespace Theme
{
    namespace
    {
        float Scale = 1.0f;

        bool Exists(const wchar_t* Path) { return GetFileAttributesW(Path) != INVALID_FILE_ATTRIBUTES; }

        // A Segoe UI face at Size, then the icons and CODOL's Chinese merged into it
        ImFont* AddFace(const char* File, const wchar_t* WFile, float Size)
        {
            ImGuiIO& io = ImGui::GetIO();
            if (!Exists(WFile)) return nullptr;
            ImFontConfig Fc; Fc.OversampleH = 2; Fc.OversampleV = 2;
            ImFont* F = io.Fonts->AddFontFromFileTTF(File, Size, &Fc);
            if (!F) return nullptr;
            if (Exists(L"C:\\Windows\\Fonts\\segmdl2.ttf"))
            {
                // the icons sit on the text's line, a little smaller than its cap height, each as wide as tall
                static const ImWchar Icons[] = { 0xE700, 0xF8FF, 0 };
                ImFontConfig Ic; Ic.MergeMode = true; Ic.GlyphOffset = ImVec2(0, Size * 0.14f); Ic.GlyphMinAdvanceX = Size * 1.05f;
                io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segmdl2.ttf", Size * 0.9f, &Ic, Icons);
            }
            // the games' level names: CODOL's Chinese, from Microsoft YaHei, when the system has it
            if (Exists(L"C:\\Windows\\Fonts\\msyh.ttc"))
            {
                ImFontConfig Cjk; Cjk.MergeMode = true; Cjk.OversampleH = 1; Cjk.OversampleV = 1;
                io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", Size, &Cjk, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
            }
            return F;
        }
    }

    Fonts& Font()
    {
        static Fonts F;
        return F;
    }

    void Load(float Dpi)
    {
        Scale = Dpi;
        ImGuiIO& io = ImGui::GetIO();
        Fonts& F = Font();
        F.Body = AddFace("C:\\Windows\\Fonts\\segoeui.ttf", L"C:\\Windows\\Fonts\\segoeui.ttf", F.BodySize * Dpi);
        if (!F.Body) F.Body = io.Fonts->AddFontDefault();
        F.Strong = AddFace("C:\\Windows\\Fonts\\seguisb.ttf", L"C:\\Windows\\Fonts\\seguisb.ttf", F.BodySize * Dpi);
        if (!F.Strong) F.Strong = F.Body;
        io.FontDefault = F.Body;
        io.FontGlobalScale = 1.0f;

        ImGui::StyleColorsDark();
        ImGuiStyle& St = ImGui::GetStyle();
        St.FontSizeBase = F.BodySize * Dpi;
        St.WindowRounding = 6.0f; St.ChildRounding = 6.0f; St.FrameRounding = 5.0f; St.PopupRounding = 6.0f;
        St.GrabRounding = 4.0f; St.TabRounding = 5.0f; St.ScrollbarRounding = 6.0f;
        St.WindowPadding = ImVec2(12, 10); St.FramePadding = ImVec2(9, 5); St.ItemSpacing = ImVec2(8, 7); St.CellPadding = ImVec2(7, 4);
        St.WindowBorderSize = 0.0f; St.ChildBorderSize = 0.0f; St.PopupBorderSize = 0.0f; St.FrameBorderSize = 0.0f; St.TabBarBorderSize = 0.0f;
        St.ScrollbarSize = 12.0f; St.GrabMinSize = 6.0f; St.SeparatorTextBorderSize = 1.0f;
        // a surface per height, one accent (the selection, the primary action); every colour below is
        // a literal for graywolf-tools ux/ux-check.py contrast, which reads this file
        St.Colors[ImGuiCol_Text]                 = ImVec4(0.925f, 0.929f, 0.941f, 1.0f);
        // secondary text at 4.5:1 or more on the window, the frames, the table headers and the
        // banners (WCAG 2.2 SC 1.4.3)
        St.Colors[ImGuiCol_TextDisabled]         = ImVec4(0.62f, 0.635f, 0.667f, 1.0f);
        St.Colors[ImGuiCol_WindowBg]             = ImVec4(0.102f, 0.106f, 0.122f, 1.0f);
        St.Colors[ImGuiCol_ChildBg]              = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        St.Colors[ImGuiCol_PopupBg]              = ImVec4(0.18f, 0.184f, 0.208f, 0.98f);
        St.Colors[ImGuiCol_MenuBarBg]            = ImVec4(0.071f, 0.075f, 0.086f, 1.0f);
        St.Colors[ImGuiCol_Border]               = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
        St.Colors[ImGuiCol_FrameBg]              = ImVec4(0.149f, 0.153f, 0.173f, 1.0f);
        St.Colors[ImGuiCol_FrameBgHovered]       = ImVec4(0.18f, 0.184f, 0.208f, 1.0f);
        St.Colors[ImGuiCol_FrameBgActive]        = ImVec4(0.204f, 0.208f, 0.235f, 1.0f);
        St.Colors[ImGuiCol_TitleBg]              = ImVec4(0.071f, 0.075f, 0.086f, 1.0f);
        St.Colors[ImGuiCol_TitleBgActive]        = ImVec4(0.071f, 0.075f, 0.086f, 1.0f);
        St.Colors[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.071f, 0.075f, 0.086f, 1.0f);
        St.Colors[ImGuiCol_ScrollbarBg]          = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        St.Colors[ImGuiCol_ScrollbarGrab]        = ImVec4(1.0f, 1.0f, 1.0f, 0.12f);
        St.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.20f);
        St.Colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(1.0f, 1.0f, 1.0f, 0.28f);
        St.Colors[ImGuiCol_CheckMark]            = ImVec4(0.961f, 0.647f, 0.141f, 1.0f);
        St.Colors[ImGuiCol_SliderGrab]           = ImVec4(1.0f, 1.0f, 1.0f, 0.28f);   // under the value's text: faint until dragged
        St.Colors[ImGuiCol_SliderGrabActive]     = ImVec4(1.0f, 0.718f, 0.29f, 1.0f);
        St.Colors[ImGuiCol_Button]               = ImVec4(0.149f, 0.153f, 0.173f, 1.0f);
        St.Colors[ImGuiCol_ButtonHovered]        = ImVec4(0.204f, 0.208f, 0.235f, 1.0f);
        St.Colors[ImGuiCol_ButtonActive]         = ImVec4(0.235f, 0.239f, 0.267f, 1.0f);
        // a selected row, a hovered one: the accent faint, so the text on it stays the body's
        St.Colors[ImGuiCol_Header]               = ImVec4(0.961f, 0.647f, 0.141f, 0.16f);
        St.Colors[ImGuiCol_HeaderHovered]        = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
        St.Colors[ImGuiCol_HeaderActive]         = ImVec4(0.961f, 0.647f, 0.141f, 0.24f);
        St.Colors[ImGuiCol_Separator]            = ImVec4(1.0f, 1.0f, 1.0f, 0.07f);
        St.Colors[ImGuiCol_SeparatorHovered]     = ImVec4(0.961f, 0.647f, 0.141f, 0.5f);
        St.Colors[ImGuiCol_SeparatorActive]      = ImVec4(0.961f, 0.647f, 0.141f, 0.8f);
        St.Colors[ImGuiCol_ResizeGrip]           = ImVec4(1.0f, 1.0f, 1.0f, 0.0f);
        St.Colors[ImGuiCol_ResizeGripHovered]    = ImVec4(0.961f, 0.647f, 0.141f, 0.5f);
        St.Colors[ImGuiCol_ResizeGripActive]     = ImVec4(0.961f, 0.647f, 0.141f, 0.8f);
        St.Colors[ImGuiCol_Tab]                  = ImVec4(0.071f, 0.075f, 0.086f, 1.0f);
        St.Colors[ImGuiCol_TabHovered]           = ImVec4(0.149f, 0.153f, 0.173f, 1.0f);
        St.Colors[ImGuiCol_TabSelected]          = ImVec4(0.102f, 0.106f, 0.122f, 1.0f);
        St.Colors[ImGuiCol_TabSelectedOverline]  = ImVec4(0.961f, 0.647f, 0.141f, 1.0f);
        St.Colors[ImGuiCol_TabDimmed]            = ImVec4(0.071f, 0.075f, 0.086f, 1.0f);
        St.Colors[ImGuiCol_TabDimmedSelected]    = ImVec4(0.102f, 0.106f, 0.122f, 1.0f);
        St.Colors[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(1.0f, 1.0f, 1.0f, 0.0f);
        St.Colors[ImGuiCol_DockingPreview]       = ImVec4(0.961f, 0.647f, 0.141f, 0.4f);
        St.Colors[ImGuiCol_DockingEmptyBg]       = ImVec4(0.071f, 0.075f, 0.086f, 1.0f);
        St.Colors[ImGuiCol_TableHeaderBg]        = ImVec4(0.149f, 0.153f, 0.173f, 1.0f);
        St.Colors[ImGuiCol_TableBorderStrong]    = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
        St.Colors[ImGuiCol_TableBorderLight]     = ImVec4(1.0f, 1.0f, 1.0f, 0.04f);
        St.Colors[ImGuiCol_TableRowBg]           = ImVec4(1.0f, 1.0f, 1.0f, 0.0f);
        St.Colors[ImGuiCol_TableRowBgAlt]        = ImVec4(1.0f, 1.0f, 1.0f, 0.02f);
        St.Colors[ImGuiCol_TextSelectedBg]       = ImVec4(0.961f, 0.647f, 0.141f, 0.30f);
        St.Colors[ImGuiCol_NavCursor]            = ImVec4(0.961f, 0.647f, 0.141f, 0.8f);
        St.Colors[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);
        // a progress bar's fill: the accent darkened, its overlaid text (ImGuiCol_Text) readable on it
        St.Colors[ImGuiCol_PlotHistogram]        = ImVec4(0.550f, 0.330f, 0.040f, 1.0f);
        St.ScaleAllSizes(Dpi);
    }

    void PushTitle() { ImGui::PushFont(Font().Strong, Font().TitleSize * Scale); }
    void PushSmall() { ImGui::PushFont(nullptr, Font().SmallSize * Scale); }
    void PopFont() { ImGui::PopFont(); }

    void PushPrimary()
    {
        ImGui::PushStyleColor(ImGuiCol_Button, Accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentHover);
        ImGui::PushStyleColor(ImGuiCol_Text, OnAccent);
        ImGui::PushFont(Font().Strong, 0.0f);
    }

    void PopPrimary()
    {
        ImGui::PopFont();
        ImGui::PopStyleColor(4);
    }

    bool PrimaryButton(const char* Label, const ImVec2& Size, bool Enabled)
    {
        if (!Enabled)
        {
            ImGui::BeginDisabled();
            const bool Hit = ImGui::Button(Label, Size);
            ImGui::EndDisabled();
            return Hit;
        }
        PushPrimary();
        const bool Hit = ImGui::Button(Label, Size);
        PopPrimary();
        return Hit;
    }

    bool IconButton(const char* Id, const char* Glyph, const char* Tooltip, bool On)
    {
        const float H = ImGui::GetFrameHeight();
        ImGui::PushID(Id);
        ImGui::PushStyleColor(ImGuiCol_Button, On ? AccentSoft : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, On ? ImVec4(Accent.x, Accent.y, Accent.z, 0.24f) : ImVec4(1, 1, 1, 0.08f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.14f));
        if (On) ImGui::PushStyleColor(ImGuiCol_Text, Accent);
        const bool Hit = ImGui::Button(Glyph, ImVec2(H, H));
        ImGui::PopStyleColor(On ? 4 : 3);
        if (Tooltip && *Tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", Tooltip);
        ImGui::PopID();
        return Hit;
    }

    bool IconLabelButton(const char* Glyph, const char* Label, const ImVec2& Size)
    {
        char Text[256];
        snprintf(Text, sizeof(Text), "%s  %s", Glyph, Label);
        return ImGui::Button(Text, Size);
    }

    bool Pill(const char* Label, bool On, const ImVec2& Size)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, On ? AccentSoft : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, On ? ImVec4(Accent.x, Accent.y, Accent.z, 0.24f) : ImVec4(1, 1, 1, 0.07f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(Accent.x, Accent.y, Accent.z, 0.30f));
        ImGui::PushStyleColor(ImGuiCol_Text, On ? Accent : Muted);
        const bool Hit = ImGui::Button(Label, Size);
        ImGui::PopStyleColor(4);
        return Hit;
    }

    bool Section(const char* Label, bool DefaultOpen)
    {
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, Muted);
        ImGui::PushFont(Font().Strong, 0.0f);
        const bool Open = ImGui::TreeNodeEx(Label, ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth
                                                   | (DefaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0));
        ImGui::PopFont();
        ImGui::PopStyleColor(2);
        return Open;
    }
}
