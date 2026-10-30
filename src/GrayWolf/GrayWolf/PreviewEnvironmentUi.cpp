#include "stdafx.h"

#include "PreviewEnvironmentUi.h"
#include "PreviewEnvironment.h"
#include "ModelPreviewGPU.h"   // PreviewLighting
#include "SettingsManager.h"
#include "ImGuiTheme.h"

#include "imgui.h"

#include <cmath>
#include <string>

namespace
{
    void Save(const PreviewLighting& L)
    {
        SettingsManager::SetSetting("preview_environment", L.Environment);
        SettingsManager::SetSetting("preview_env_rotation", std::to_string(L.EnvRotation));
        SettingsManager::SetSetting("preview_env_intensity", std::to_string(L.EnvIntensity));
        SettingsManager::SetSetting("preview_bloom", L.Bloom ? "true" : "false");
        SettingsManager::SetSetting("preview_bloom_strength", std::to_string(L.BloomStrength));
        SettingsManager::SetSetting("preview_env_backdrop", L.Backdrop ? "true" : "false");
        SettingsManager::SetSetting("preview_env_backdrop_blur", std::to_string(L.BackdropBlur));
    }

    const PreviewEnvironmentInfo* Find(const std::string& Id)
    {
        for (const auto& E : PreviewEnvironments()) if (E.Id == Id) return &E;
        return nullptr;
    }
}

void LoadPreviewEnvironmentSettings(PreviewLighting& L)
{
    const PreviewLighting Default;
    L.Environment = SettingsManager::GetSetting("preview_environment", Default.Environment);
    // a baked one no longer shipped: the default, else the procedural sky (the default's files missing)
    // a baked one no longer shipped: the default, else the procedural sky (the default's files missing)
    if (!Find(L.Environment)) L.Environment = Find(Default.Environment) ? Default.Environment : std::string(kStudioEnvironment);
    L.EnvRotation = SettingsManager::GetSettingFloat("preview_env_rotation", Default.EnvRotation, -180.0f, 180.0f);
    L.EnvIntensity = SettingsManager::GetSettingFloat("preview_env_intensity", Default.EnvIntensity, 0.0f, 4.0f);
    L.Bloom = SettingsManager::GetSetting("preview_bloom", Default.Bloom ? "true" : "false") == "true";
    L.BloomStrength = SettingsManager::GetSettingFloat("preview_bloom_strength", Default.BloomStrength, 0.0f, 2.0f);
    L.Backdrop = SettingsManager::GetSetting("preview_env_backdrop", Default.Backdrop ? "true" : "false") == "true";
    L.BackdropBlur = SettingsManager::GetSettingFloat("preview_env_backdrop_blur", Default.BackdropBlur, 0.0f, 1.0f);
}

void DrawPreviewEnvironmentControls(PreviewLighting& L)
{
    const PreviewLighting Default;
    const PreviewEnvironmentInfo* Current = Find(L.Environment);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.5f);
    if (ImGui::BeginCombo("##environment", Current ? Current->Name.c_str() : L.Environment.c_str()))
    {
        for (const auto& E : PreviewEnvironments())
            if (ImGui::Selectable(E.Name.c_str(), E.Id == L.Environment))
            {
                L.Environment = E.Id;
                Save(L);
            }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The environment that lights the model (image based light)");

    ImGui::SameLine(0, ImGui::GetFontSize() * 0.2f);
    if (Theme::IconButton("envsettings", Theme::Icon::Sliders, "Environment turn and strength, backdrop, bloom", L.Bloom))
        ImGui::OpenPopup("envpopup");
    if (ImGui::BeginPopup("envpopup"))
    {
        const float W = ImGui::GetFontSize() * 11.0f;
        const auto Row = [&](const char* Label, const char* Id, float* V, float Lo, float Hi, const char* Fmt, float Reset, const char* Tip) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(Label);
            ImGui::SameLine(ImGui::GetFontSize() * 6.0f);
            ImGui::SetNextItemWidth(W);
            ImGui::SliderFloat(Id, V, Lo, Hi, Fmt);
            if (ImGui::IsItemDeactivatedAfterEdit()) Save(L);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s (double-click to type, right-click resets)", Tip);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { *V = Reset; Save(L); }
        };
        Row("Turn", "##envturn", &L.EnvRotation, -180.0f, 180.0f, "%.0f\xC2\xB0", Default.EnvRotation, "The environment turned about the up axis");
        Row("Strength", "##envstrength", &L.EnvIntensity, 0.0f, 4.0f, "%.2fx", Default.EnvIntensity, "The environment's light");
        ImGui::Separator();
        if (ImGui::Checkbox("Backdrop", &L.Backdrop)) Save(L);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The environment shown behind the model, blurred (screenshots stay transparent)");
        ImGui::BeginDisabled(!L.Backdrop);
        Row("Blur", "##backdropblur", &L.BackdropBlur, 0.0f, 1.0f, "%.2f", Default.BackdropBlur, "How blurred the backdrop is");
        ImGui::EndDisabled();
        ImGui::Separator();
        if (ImGui::Checkbox("Bloom", &L.Bloom)) Save(L);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A soft glow around the brightest parts (highlights, emission)");
        ImGui::BeginDisabled(!L.Bloom);
        Row("Amount", "##bloomstrength", &L.BloomStrength, 0.0f, 2.0f, "%.2f", Default.BloomStrength, "The glow's strength");
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
}
