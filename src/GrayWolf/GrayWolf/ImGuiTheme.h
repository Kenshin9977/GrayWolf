#pragma once

// GrayWolf's look, in one place: the surfaces, the one accent (the README's orange), the fonts at three
// sizes, the icons (Segoe MDL2 Assets, merged into the fonts) and the few widgets every panel draws the
// same way (the primary button, an icon button, a pill, an unframed section).
#include "imgui.h"

namespace Theme
{
    // Surfaces, darkest to lightest: the higher a surface sits, the lighter it is (no borders, no shadows)
    inline constexpr ImVec4 Base     { 0.071f, 0.075f, 0.086f, 1.0f };   // behind the panels, the viewport
    inline constexpr ImVec4 Panel    { 0.102f, 0.106f, 0.122f, 1.0f };   // a docked panel
    inline constexpr ImVec4 Raised   { 0.149f, 0.153f, 0.173f, 1.0f };   // fields, buttons, table headers
    inline constexpr ImVec4 Overlay  { 0.180f, 0.184f, 0.208f, 1.0f };   // popups, menus, tooltips
    inline constexpr ImVec4 Text     { 0.925f, 0.929f, 0.941f, 1.0f };
    inline constexpr ImVec4 Muted    { 0.620f, 0.635f, 0.667f, 1.0f };   // 4.5:1 or more on every surface above
    // The accent: the primary action and the selection only
    inline constexpr ImVec4 Accent      { 0.961f, 0.647f, 0.141f, 1.0f };   // #f5a524
    inline constexpr ImVec4 AccentHover { 1.000f, 0.718f, 0.290f, 1.0f };
    inline constexpr ImVec4 AccentSoft  { 0.961f, 0.647f, 0.141f, 0.16f };  // a selected row, an active pill
    inline constexpr ImVec4 OnAccent    { 0.082f, 0.090f, 0.110f, 1.0f };   // text on the accent
    inline constexpr ImU32 AccentU32 = IM_COL32(245, 165, 36, 255);

    // The fonts: the body (Segoe UI), a semibold one for names and titles, and their sizes
    struct Fonts
    {
        ImFont* Body = nullptr;
        ImFont* Strong = nullptr;
        float BodySize = 16.0f, TitleSize = 21.0f, SmallSize = 13.5f;   // at 96 DPI, scaled by Load
    };
    Fonts& Font();
    // The fonts at that DPI scale (the icons merged into both) and the style
    void Load(float Dpi);

    // Segoe MDL2 Assets (Windows 10 and 11), merged into both fonts
    namespace Icon
    {
        inline constexpr const char* Search       = "\xEE\x9C\xA1";   // U+E721
        inline constexpr const char* Filter       = "\xEE\x9C\x9C";   // U+E71C
        inline constexpr const char* Sliders      = "\xEE\xA7\xA9";   // U+E9E9
        inline constexpr const char* Camera       = "\xEE\x9C\xA2";   // U+E722
        inline constexpr const char* Play         = "\xEE\x9D\xA8";   // U+E768
        inline constexpr const char* Pause        = "\xEE\x9D\xA9";   // U+E769
        inline constexpr const char* Folder       = "\xEE\xA2\xB7";   // U+E8B7
        inline constexpr const char* FolderOpen   = "\xEE\xA0\xB8";   // U+E838
        inline constexpr const char* Add          = "\xEE\x9C\x90";   // U+E710
        inline constexpr const char* Close        = "\xEE\x9C\x91";   // U+E711
        inline constexpr const char* Help         = "\xEE\xA2\x97";   // U+E897
        inline constexpr const char* Info         = "\xEE\xA5\x86";   // U+E946
        inline constexpr const char* Lights       = "\xEE\xA0\xAF";   // U+E82F
        inline constexpr const char* Exposure     = "\xEE\x9C\x86";   // U+E706
        inline constexpr const char* Grid         = "\xEF\x83\xA2";   // U+F0E2
        inline constexpr const char* List         = "\xEE\xA3\xBD";   // U+E8FD
        inline constexpr const char* Export       = "\xEE\xA2\x96";   // U+E896
        inline constexpr const char* Model        = "\xEF\x85\x98";   // U+F158
        inline constexpr const char* Image        = "\xEE\xAE\x9F";   // U+EB9F
        inline constexpr const char* Audio        = "\xEE\xA3\x96";   // U+E8D6
        inline constexpr const char* Anim         = "\xEE\x9C\x94";   // U+E714
        inline constexpr const char* Map          = "\xEE\x9C\x87";   // U+E707
        inline constexpr const char* Fx           = "\xEE\xA5\x85";   // U+E945
        inline constexpr const char* Game         = "\xEE\x9F\xBC";   // U+E7FC
        inline constexpr const char* Copy         = "\xEE\xA3\x88";   // U+E8C8
        inline constexpr const char* Library      = "\xEE\xA3\xB1";   // U+E8F1
        inline constexpr const char* ChevronDown  = "\xEE\x9C\x8D";   // U+E70D
        inline constexpr const char* ChevronRight = "\xEE\x9D\xAC";   // U+E76C
        inline constexpr const char* Settings     = "\xEE\x9C\x93";   // U+E713
        inline constexpr const char* External     = "\xEE\xA2\xA7";   // U+E8A7
    }

    // The left tabs: their icon and name shown, their ImGui id after "###" (kept from their first names,
    // which ui-record scripts focus by)
    inline constexpr const char* ExtractWindow = "\xEE\xA2\x96  Extract###Load Game";   // U+E896, a game's files read
    inline constexpr const char* LibraryWindow = "\xEE\xA3\xB1  Library###Assets";      // U+E8F1, the folders of exports

    // The text that follows in the semibold font at the title size (an asset's name)
    void PushTitle();
    void PushSmall();
    void PopFont();

    // The colours and font of the window's one main action, for a button drawn between the two
    void PushPrimary();
    void PopPrimary();
    // The window's one main action: accent filled, dark text (disabled: a plain raised button)
    bool PrimaryButton(const char* Label, const ImVec2& Size = ImVec2(0, 0), bool Enabled = true);
    // A square button holding an icon, flat until hovered; On: shown pressed (a toggle). Tooltip on hover.
    bool IconButton(const char* Id, const char* Glyph, const char* Tooltip, bool On = false);
    // A button showing an icon then a label, flat until hovered
    bool IconLabelButton(const char* Glyph, const char* Label, const ImVec2& Size = ImVec2(0, 0));
    // One choice of a segmented row (a type, list or grid): the active one in the accent's soft fill
    bool Pill(const char* Label, bool On, const ImVec2& Size = ImVec2(0, 0));
    // A section header without the full-width bar: a chevron and its label; true while open
    bool Section(const char* Label, bool DefaultOpen = false);
}
