#include "stdafx.h"

#include "FxPreviewPanel.h"

#include "FxPreviewRender.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <future>
#include <list>
#include <map>

struct FxPreviewPanel::Impl
{
    Microsoft::WRL::ComPtr<ID3D11Device> Dev;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> Ctx;
    FxPreview::Renderer Renderer;
    FxPreview::Target Target;

    // the effect read on a worker (its images indexed): the latest asked for is shown
    struct Loaded { std::shared_ptr<FxPreview::Scene> Scene; std::string Error; };
    std::list<std::pair<uint64_t, std::future<Loaded>>> Pending;
    uint64_t Wanted = 0;
    std::string Path, Error;
    std::shared_ptr<FxPreview::Scene> Scene;

    // transport
    bool Playing = true, Loop = true, Grid = true, ForwardUp = false;
    double TimeMs = 0;
    float Speed = 1.0f;
    uint32_t Seed = 1;
    // camera
    float Yaw = 0.9f, Pitch = 0.32f, DistScale = 1.0f, Pan[3] = {};
    // the last frame's numbers
    size_t Particles = 0, Effects = 0;
    bool Capped = false;

    Impl(ID3D11Device* D, ID3D11DeviceContext* C) : Dev(D), Ctx(C), Renderer(D) {}

    void Restart()
    {
        TimeMs = 0;
        Playing = true;
    }

    void Apply()
    {
        if (!Scene || !Scene->Sim) return;
        FxPreview::SimOptions O = Scene->Sim->Options();
        O.Seed = Seed;
        O.ForwardUp = ForwardUp;
        Scene->Sim->SetOptions(O);
        Scene->Frame();
    }

    void Poll()
    {
        for (auto It = Pending.begin(); It != Pending.end();)
        {
            if (It->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            {
                ++It;
                continue;
            }
            Loaded L = It->second.get();
            if (It->first == Wanted)
            {
                Scene = L.Scene;
                Error = L.Error;
                Yaw = 0.9f, Pitch = 0.32f, DistScale = 1.0f, Pan[0] = Pan[1] = Pan[2] = 0;
                Restart();
            }
            It = Pending.erase(It);
        }
    }
};

FxPreviewPanel::FxPreviewPanel(ID3D11Device* Dev, ID3D11DeviceContext* Ctx) : P(std::make_unique<Impl>(Dev, Ctx)) {}

FxPreviewPanel::~FxPreviewPanel()
{
    for (auto& [Id, F] : P->Pending)
        if (F.valid()) F.wait();
}

bool FxPreviewPanel::IsEffectFile(const std::string& Path)
{
    if (Path.size() < 6) return false;
    std::string L = Path;
    for (char& C : L) C = (char)std::tolower((unsigned char)C), C = C == '/' ? '\\' : C;
    return L.compare(L.size() - 5, 5, ".json") == 0 && L.find("\\fx\\") != std::string::npos;
}

bool FxPreviewPanel::Load(const std::string& JsonPath)
{
    P->Poll();
    if (JsonPath == P->Path && (P->Scene || !P->Pending.empty())) return true;
    P->Path = JsonPath;
    P->Scene.reset();
    P->Error.clear();
    const uint64_t Id = ++P->Wanted;
    FxPreview::SimOptions O;
    O.Seed = P->Seed;
    O.ForwardUp = P->ForwardUp;
    P->Pending.emplace_back(Id, std::async(std::launch::async, [JsonPath, O] {
        Impl::Loaded L;
        auto S = std::make_shared<FxPreview::Scene>();
        if (S->Load(JsonPath, {}, L.Error, O)) L.Scene = std::move(S);
        return L;
    }));
    return true;
}

void FxPreviewPanel::Unload()
{
    P->Path.clear();
    P->Scene.reset();
    P->Error.clear();
    P->Wanted++;
}

bool FxPreviewPanel::Loaded() const { return P->Scene != nullptr; }

std::string FxPreviewPanel::Summary() const
{
    if (!P->Scene) return P->Error.empty() ? std::string("reading the effect\xE2\x80\xA6") : P->Error;
    const FxPreview::Effect& Fx = *P->Scene->Fx;
    std::map<std::string, int> Types;
    for (const auto& E : Fx.Elements) Types[E.TypeName]++;
    std::string Out = std::to_string(Fx.Elements.size()) + " elements (";
    bool First = true;
    for (const auto& [T, N] : Types)
    {
        Out += (First ? "" : ", ") + std::to_string(N) + " " + T;
        First = false;
    }
    char Len[64];
    std::snprintf(Len, sizeof(Len), ")  \xC2\xB7  %.2fs%s", P->Scene->DurationMsec / 1000.0, Fx.Loops() ? ", looping" : "");
    Out += Len;
    if (!Fx.Timed) Out += "  \xC2\xB7  no timings in this game's export: each element shown still";
    return Out;
}

void FxPreviewPanel::Draw()
{
    Impl& I = *P;
    I.Poll();
    const ImGuiIO& Io = ImGui::GetIO();
    if (!I.Scene)
    {
        ImGui::TextDisabled("%s", I.Error.empty() ? "reading the effect\xE2\x80\xA6" : ("could not read this effect: " + I.Error).c_str());
        return;
    }
    FxPreview::Scene& S = *I.Scene;

    // ---- transport
    const double Duration = std::max(1.0, S.DurationMsec);
    if (I.Playing)
    {
        I.TimeMs += Io.DeltaTime * 1000.0 * I.Speed;
        if (I.TimeMs > Duration)
        {
            if (I.Loop) I.TimeMs = std::fmod(I.TimeMs, Duration);
            else if (!S.Fx->Loops())
            {
                I.TimeMs = Duration;
                I.Playing = false;
            }
        }
    }
    if (ImGui::Button(I.Playing ? "Pause" : "Play", ImVec2(ImGui::GetFontSize() * 4.5f, 0)) ||
        (!Io.WantTextInput && ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Space, false)))
    {
        if (!I.Playing && I.TimeMs >= Duration && !S.Fx->Loops()) I.TimeMs = 0;
        I.Playing = !I.Playing;
    }
    ImGui::SameLine();
    if (ImGui::Button("Restart")) I.Restart();
    ImGui::SameLine();
    ImGui::Checkbox("Loop", &I.Loop);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Start again after %.2fs (the effect's run)", Duration / 1000.0);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
    ImGui::SliderFloat("##speed", &I.Speed, 0.05f, 4.0f, "speed %.2fx", ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) I.Speed = 1.0f;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Playback speed (right-click: 1x)");
    ImGui::SameLine();
    if (ImGui::Button("New seed"))
    {
        I.Seed = I.Seed * 1664525u + 1013904223u;
        I.Apply();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Other random draws: where, how big, how long each particle lives");
    ImGui::SameLine();
    if (ImGui::Checkbox("Forward up", &I.ForwardUp)) I.Apply();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Turn the effect so its forward axis (X) points up: an impact on the ground.\n"
                                                  "Off: as authored, Z up.");
    ImGui::SameLine();
    ImGui::Checkbox("Grid", &I.Grid);

    float Sec = (float)(I.TimeMs / 1000.0);
    ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 14.0f);
    if (ImGui::SliderFloat("##time", &Sec, 0.0f, (float)std::max(Duration, I.TimeMs) / 1000.0f, "%.2f s"))
    {
        I.TimeMs = Sec * 1000.0;
        I.Playing = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu particles%s  \xC2\xB7  %zu effects", I.Particles, I.Capped ? " (capped)" : "", I.Effects);
    if (I.Renderer.TexturesMissing() > 0)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("\xC2\xB7  %zu without image", I.Renderer.TexturesMissing());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Materials whose image is not among the exported ones (drawn as a soft dot).\n"
                              "Export the images with the effects (--type fx,image), and the zone that defines\n"
                              "them (a common zone) into a folder beside this one.");
    }

    // ---- the view
    const ImVec2 Avail = ImGui::GetContentRegionAvail();
    const int W = (int)std::max(16.0f, Avail.x), H = (int)std::max(16.0f, Avail.y);
    if (!I.Target.Resize(I.Dev.Get(), W, H) || !I.Renderer.Ok())
    {
        ImGui::TextDisabled("could not make the view");
        return;
    }
    I.Renderer.SetTextures(S.Textures);
    FxPreview::Frame F;
    S.Sim->Evaluate(I.TimeMs, F);
    I.Particles = F.Particles.size() + F.Trails.size();
    I.Effects = F.Effects;
    I.Capped = F.Capped;
    FxPreview::DrawOptions O;
    O.Grid = I.Grid;
    O.GridCenter[0] = S.Center[0];
    O.GridCenter[1] = S.Center[1];
    O.GridCenter[2] = S.Sim->GroundZ();
    const PreviewCamera Cam = FxPreview::OrbitCamera(S, I.Yaw, I.Pitch, I.DistScale, I.Pan);
    I.Renderer.Render(I.Ctx.Get(), F, Cam, I.Target, O);
    ImGui::Image((ImTextureID)(intptr_t)I.Target.OutputSRV.Get(), ImVec2((float)W, (float)H));
    if (ImGui::IsItemHovered())
    {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            const ImVec2 D = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
            I.Yaw += D.x * 0.01f;
            I.Pitch = std::clamp(I.Pitch + D.y * 0.01f, -1.5f, 1.5f);
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f))
        {
            const ImVec2 D = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle, 0.0f);
            DirectX::XMFLOAT3 Pan(I.Pan[0], I.Pan[1], I.Pan[2]);
            PanPreviewCamera(Cam, D.x, D.y, (float)H, Pan);
            I.Pan[0] = Pan.x, I.Pan[1] = Pan.y, I.Pan[2] = Pan.z;
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
        }
        if (Io.MouseWheel != 0.0f) I.DistScale = std::clamp(I.DistScale * (Io.MouseWheel > 0 ? 0.88f : 1.136f), 0.05f, 20.0f);
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) I.Yaw = 0.9f, I.Pitch = 0.32f, I.DistScale = 1.0f, I.Pan[0] = I.Pan[1] = I.Pan[2] = 0;
    }
}
