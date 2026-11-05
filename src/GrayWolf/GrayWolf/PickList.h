#pragma once

// One multi-selection for every list of assets or zones (ImGui's multi-select: Shift+click the rows
// between, Ctrl+click one more, Ctrl+A, a box), over the list's own picks. A list draws its clipped
// rows through Rows, calling Item(i) before each row's Checkbox or Selectable; HeadersRow puts the
// box that picks them all in its header. A new list of things to pick goes through here.

#include "imgui.h"
#include "imgui_internal.h"   // ImGuiItemFlags_MixedValue: the header's box when some are picked

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>

namespace PickList
{
    enum Mode
    {
        Checks,      // ticks: a click ticks or unticks that row only, nothing else is cleared
        Selection,   // a selection: a click selects that row alone, Ctrl adds, Escape or a click on nothing clears
    };

    struct List
    {
        Mode M;
        int Count;                                // rows 0 .. Count-1
        std::function<void(int, bool)> Set;       // picks or unpicks row i
        std::function<void(bool)> All = nullptr;  // every row at once, when cheaper (a click clears a selection of thousands)
        ImGuiMultiSelectFlags Extra = 0;          // the box's kind (BoxSelect1d for rows, 2d for a grid)
        int Per = 1;                              // items per clipper row (a grid's columns)
        float RowHeight = -1.0f;                  // the clipper's row height, when known
    };

    namespace Detail
    {
        inline void Apply(const List& L, const ImGuiMultiSelectIO* Io)
        {
            for (const ImGuiSelectionRequest& Q : Io->Requests)
                if (Q.Type == ImGuiSelectionRequestType_SetAll && L.All) L.All(Q.Selected);
                else if (Q.Type == ImGuiSelectionRequestType_SetAll)
                    for (int i = 0; i < L.Count; i++) L.Set(i, Q.Selected);
                else if (Q.Type == ImGuiSelectionRequestType_SetRange)   // First after Last on a range upward
                    for (int i = (int)std::min(Q.RangeFirstItem, Q.RangeLastItem); i <= (int)std::max(Q.RangeFirstItem, Q.RangeLastItem); i++)
                        L.Set(i, Q.Selected);
        }
    }

    // The scope, a clipper over the rows (L.Per items each) for Body to step through, the scope
    // closed before returning (call it inside the table or child window). The row a Shift range
    // starts from stays submitted, or ImGui loses it.
    inline void Rows(const List& L, const std::function<void(ImGuiListClipper&)>& Body)
    {
        const ImGuiMultiSelectFlags Flags = L.M == Checks
            ? ImGuiMultiSelectFlags_NoAutoSelect | ImGuiMultiSelectFlags_NoAutoClear
            : ImGuiMultiSelectFlags_ClearOnEscape | ImGuiMultiSelectFlags_ClearOnClickVoid;
        ImGuiMultiSelectIO* Io = ImGui::BeginMultiSelect(Flags | L.Extra, -1, L.Count);
        Detail::Apply(L, Io);
        ImGuiListClipper Clipper;
        Clipper.Begin((L.Count + L.Per - 1) / L.Per, L.RowHeight);
        if (Io->RangeSrcItem != -1) Clipper.IncludeItemByIndex((int)Io->RangeSrcItem / L.Per);
        Body(Clipper);
        Clipper.End();
        Detail::Apply(L, ImGui::EndMultiSelect());
    }

    // the next item is row i of the list
    inline void Item(int i) { ImGui::SetNextItemSelectionUserData(i); }

    // The table's header row (in place of TableHeadersRow), a box in the first column's header for
    // the Total things the list stands for (the rows past what it shows too): ticked when all are
    // picked, mixed when some are; a click picks them all, or unpicks them when all were
    inline void HeadersRow(size_t Picked, size_t Total, const std::function<void(bool)>& SetAll, const char* What)
    {
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        for (int C = 0; C < ImGui::TableGetColumnCount(); C++)
        {
            if (!ImGui::TableSetColumnIndex(C)) continue;
            ImGui::PushID(C);
            if (C == 0)
            {
                bool All = Total > 0 && Picked == Total;
                ImGui::BeginDisabled(Total == 0);
                ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, Picked > 0 && Picked < Total);
                // the theme's header is the frames' colour: the box a shade lighter, or it is not seen
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered));
                if (ImGui::Checkbox("##all", &All)) SetAll(All);
                ImGui::PopStyleColor();
                ImGui::PopItemFlag();
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s: %zu of %zu picked\nShift+click two boxes: the rows between, Ctrl+A: every row listed", What, Picked, Total);
                ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            }
            ImGui::TableHeader(ImGui::TableGetColumnName(C));
            ImGui::PopID();
        }
    }
}
