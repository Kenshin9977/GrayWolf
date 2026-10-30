#include "stdafx.h"

// The viewport's quality settings as ImGui controls (the Viewport toolbar's quality button).
#include "PreviewPost.h"

#include "imgui.h"

#include <iterator>

bool DrawPreviewQualityControls(PreviewQuality& Q)
{
    const PreviewQuality Before = Q;
    bool Commit = false;   // saved when a slider is let go, a box ticked or a choice made
    const float Width = ImGui::GetFontSize() * 9.0f;

    ImGui::TextDisabled("Ambient occlusion");
    Commit |= ImGui::Checkbox("XeGTAO##ao", &Q.Ao);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Screen space ambient occlusion (Intel's XeGTAO) on the environment light, not on the three lights");
    ImGui::BeginDisabled(!Q.Ao);
    ImGui::SetNextItemWidth(Width);
    ImGui::SliderFloat("Strength##ao", &Q.AoStrength, 0.0f, 3.0f, "%.2f");
    Commit |= ImGui::IsItemDeactivatedAfterEdit();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { Q.AoStrength = PreviewQuality{}.AoStrength; Commit = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The occlusion raised to this power: 1 as computed, more darkens (right-click resets)");
    ImGui::SetNextItemWidth(Width);
    ImGui::SliderFloat("Radius##ao", &Q.AoRadius, 0.01f, 0.5f, "%.3f", ImGuiSliderFlags_Logarithmic);
    Commit |= ImGui::IsItemDeactivatedAfterEdit();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { Q.AoRadius = PreviewQuality{}.AoRadius; Commit = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("How far the occlusion reaches, as a part of the model's size (right-click resets)");
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::TextDisabled("Anti-aliasing");
    struct Mode { const char* Name; int Msaa; bool Smaa; };
    static const Mode Modes[] = { { "Off", 1, false }, { "SMAA", 1, true }, { "MSAA 2x", 2, false }, { "MSAA 4x", 4, false },
                                  { "MSAA 8x", 8, false }, { "MSAA 4x + SMAA", 4, true }, { "MSAA 8x + SMAA", 8, true } };
    int Current = -1;
    for (int i = 0; i < (int)std::size(Modes); i++) if (Modes[i].Msaa == Q.Msaa && Modes[i].Smaa == Q.Smaa) Current = i;
    ImGui::SetNextItemWidth(Width);
    if (ImGui::BeginCombo("Edges##aa", Current >= 0 ? Modes[Current].Name : "Custom"))
    {
        for (int i = 0; i < (int)std::size(Modes); i++)
            if (ImGui::Selectable(Modes[i].Name, i == Current)) { Q.Msaa = Modes[i].Msaa; Q.Smaa = Modes[i].Smaa; Commit = true; }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("MSAA samples the geometry's edges; SMAA smooths the finished image (cheaper, softer)");
    static const int Stills[] = { 1, 4, 8, 16, 32, 64 };
    ImGui::SetNextItemWidth(Width);
    char Label[32];
    snprintf(Label, sizeof(Label), Q.StillSamples > 1 ? "%d frames" : "Off", Q.StillSamples);
    if (ImGui::BeginCombo("Still##accum", Label))
    {
        for (const int N : Stills)
        {
            snprintf(Label, sizeof(Label), N > 1 ? "%d frames" : "Off", N);
            if (ImGui::Selectable(Label, N == Q.StillSamples)) { Q.StillSamples = N; Commit = true; }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Once the view holds still, frames shifted by a fraction of a pixel are averaged into a clean image\n"
                          "(supersampling; screenshots and render-model always take them all)");
    if (Commit) SavePreviewQuality(Q);
    return !(Q == Before);
}
