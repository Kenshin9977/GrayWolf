#include "stdafx.h"

// The Viewport's screenshots: the folder (Settings > Screenshots), the Screenshot bar, the render and
// its save, and GH_UI_SHOT's triggers (a headless caller's grab of the window or of the 3D view).
#include "ImGuiShell.Internal.h"
#include "AppLog.h"           // spdlog
#include "ImGuiDialogs.h"      // SetBuf, AbsoluteFolder
#include "UiAsync.h"           // dialogs, shell opens and disk lookups off the frame
#include "Strings.h"
#include "SettingsManager.h"   // the screenshot's folder and scale

#include "imgui.h"
#include "DirectXTex.h"       // CaptureTexture / SaveToWICFile (self-screenshot)

#include <shlobj.h>          // SHGetKnownFolderPath (Pictures)

#include <atomic>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

namespace ShellUi
{
    // Where the viewport's screenshots go: the screenshot_dir setting; by default the folder the old
    // save dialog last used, else Pictures\GrayWolf. Created when a shot is saved.
    std::string ScreenshotDir()
    {
        static const std::string Default = [] {
            const std::string Last = SettingsManager::GetSetting("preview_shot_dir", "");
            if (!Last.empty()) return Last;
            PWSTR Pictures = nullptr;
            std::string Dir;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_DEFAULT, nullptr, &Pictures)))
                Dir = Strings::ToNormalString(Pictures) + "\\GrayWolf";
            CoTaskMemFree(Pictures);
            return Dir.empty() ? AbsoluteFolder("screenshots") : Dir;
        }();
        const std::string Set = SettingsManager::GetSetting("screenshot_dir", "");
        return Set.empty() ? Default : AbsoluteFolder(Set);
    }

    // The screenshot folder in Explorer, made first when no shot was saved yet.
    void OpenScreenshotDir()
    {
        const std::string Dir = ScreenshotDir();
        UiAsync::Run("create " + Dir, [Dir] {
            std::error_code Ec;
            std::filesystem::create_directories(Strings::ToUnicodeString(Dir), Ec);
            UiAsync::OpenInShell(Dir);
        });
    }

    // Settings > Screenshots: the folder, picked or typed, opened, or back to its default.
    void DrawScreenshotSettingsMenu()
    {
        static char Buf[1024] = {};
        if (std::string Picked; UiAsync::Picked("screenshots.dir", Picked))
        {
            SettingsManager::SetSetting("screenshot_dir", Picked);
            SetBuf(Buf, sizeof(Buf), Picked);
        }
        if (!ImGui::BeginMenu("Settings")) return;   // appended to the Settings menu ImGuiUpdates opens
        if (ImGui::BeginMenu("Screenshots"))
        {
            if (!ImGui::IsAnyItemActive()) SetBuf(Buf, sizeof(Buf), ScreenshotDir());
            ImGui::TextUnformatted("Folder of the viewport's screenshots (Screenshot, Ctrl+Shift+S)");
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 30.0f);
            ImGui::InputText("##screenshotdir", Buf, sizeof(Buf));
            if (ImGui::IsItemDeactivatedAfterEdit()) SettingsManager::SetSetting("screenshot_dir", Buf);
            if (ImGui::Button("Browse...")) UiAsync::PickFolder("screenshots.dir", "Select the screenshots folder", ScreenshotDir());
            ImGui::SameLine();
            if (ImGui::Button("Open")) OpenScreenshotDir();
            ImGui::SameLine();
            ImGui::BeginDisabled(SettingsManager::GetSetting("screenshot_dir", "").empty());
            if (ImGui::Button("Default")) SettingsManager::SetSetting("screenshot_dir", "");
            ImGui::EndDisabled();
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    void RenderMeshToRT(ViewportState& V, ID3D11Device* Dev, ID3D11DeviceContext* Ctx, int W, int H)
    {
        if (!V.Vrt.Resize(Dev, W, H)) return;
        V.Renderer.SetLighting(V.Lighting);
        const float Bg[4] = { 0.086f, 0.086f, 0.098f, 1.0f };
        const PreviewCamera Cam = OrbitPreviewCamera(V.Mesh, V.Yaw, V.Pitch, V.DistScale, V.Pan);
        V.Renderer.Render(Ctx, V.Mesh.Valid ? &V.Mesh : nullptr, Cam, V.Vrt, Bg);
        if (!V.PendingShot.Wanted || !V.Mesh.Valid) return;
        const ShotRequest Req = std::exchange(V.PendingShot, ShotRequest{});
        const int SW = W * V.ShotScale, SH = H * V.ShotScale;
        const std::string Size = std::to_string(SW) + "x" + std::to_string(SH);
        auto Image = std::make_shared<DirectX::ScratchImage>();
        const auto LastShot = V.LastShot;
        if (!V.Renderer.Capture(Dev, Ctx, V.Mesh, Cam, SW, SH, *Image))
        {
            std::lock_guard<std::mutex> Lk(LastShot->M);
            LastShot->Status = "could not render the screenshot (" + Size + ")";
            LastShot->File.clear();
            LastShot->Until = UiAsync::Detail::Now() + 8.0;
            return;
        }
        const std::string Dir = Req.Path.empty() ? ScreenshotDir() : std::string();
        UiAsync::Run("save screenshot " + Size, [Image, Req, Dir, Size, LastShot] {
            const HRESULT Com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // WIC's encoder
            std::string File = Req.Path;
            if (File.empty())
            {
                const std::time_t Now = std::time(nullptr);
                std::tm Local{};
                localtime_s(&Local, &Now);
                char Stamp[32];
                std::strftime(Stamp, sizeof(Stamp), "%Y%m%d-%H%M%S", &Local);
                const std::string Base = Dir + (Dir.empty() || Dir.back() == '\\' || Dir.back() == '/' ? "" : "\\") + Req.Name + "_" + Stamp;
                File = Base + ".png";
                std::error_code Ec;
                for (int N = 2; std::filesystem::exists(Strings::ToUnicodeString(File), Ec); N++) File = Base + "_" + std::to_string(N) + ".png";
            }
            const bool Saved = SavePreviewPng(*Image->GetImage(0, 0, 0), File);
            if (SUCCEEDED(Com)) CoUninitialize();
            spdlog::info("viewport screenshot {}: {}{}", Size, Saved ? "" : "not saved, ", File);
            std::lock_guard<std::mutex> Lk(LastShot->M);
            LastShot->Status = Saved ? "saved " + Size + "  " + File : "could not save " + File + " (" + Size + ")";
            LastShot->File = Saved ? File : std::string();
            LastShot->Until = UiAsync::Detail::Now() + 8.0;
        });
    }

    // The row above a 3D view: Screenshot (or Ctrl+Shift+S), its scale, and the last save's path.
    // ModelName names the file.
    void DrawScreenshotBar(ViewportState& V, std::string ModelName)
    {
        PreviewLighting& Lighting = V.Lighting;
        for (char& c : ModelName) if (std::strchr("<>:\"/\\|?*", c) || (unsigned char)c < 32) c = '_';
        const bool Shortcut = !ImGui::GetIO().WantTextInput && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S);
        if ((ImGui::SmallButton("Screenshot") || Shortcut) && V.Mesh.Valid) V.PendingShot = { true, "", ModelName };
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Save what the view shows as a PNG, background transparent (Ctrl+Shift+S), in\n%s\n"
                              "(right-click: open that folder; Settings > Screenshots to change it)", ScreenshotDir().c_str());
        if (ImGui::BeginPopupContextItem("shotmenu"))
        {
            if (ImGui::MenuItem("Open the screenshots folder")) OpenScreenshotDir();
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        static const char* Scales[] = { "1x", "2x", "4x" };
        int ScaleIdx = V.ShotScale == 4 ? 2 : V.ShotScale - 1;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 3.5f);
        if (ImGui::Combo("##shotscale", &ScaleIdx, Scales, 3))
        {
            V.ShotScale = 1 << ScaleIdx;
            SettingsManager::SetSetting("preview_shot_scale", std::to_string(V.ShotScale));
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Screenshot size: the view's size times this");

        // saved when a slider is let go: the settings file is not rewritten every frame of a drag
        const auto SaveLighting = [&] {
            SettingsManager::SetSetting("preview_exposure", std::to_string(Lighting.Exposure));
            SettingsManager::SetSetting("preview_lights", std::to_string(Lighting.Lights));
            SettingsManager::SetSetting("preview_specular", Lighting.Specular ? "true" : "false");
        };
        ImGui::SameLine(0, ImGui::GetFontSize() * 1.5f);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
        ImGui::SliderFloat("##exposure", &Lighting.Exposure, -4.0f, 4.0f, "exposure %+.1f");
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveLighting();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Exposure in stops (double-click to type, right-click resets)");
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { Lighting.Exposure = 0.0f; SaveLighting(); }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
        ImGui::SliderFloat("##lights", &Lighting.Lights, 0.0f, 3.0f, "lights %.2fx");
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveLighting();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Strength of the three lights; the environment stays (right-click resets)");
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { Lighting.Lights = 1.0f; SaveLighting(); }
        ImGui::SameLine();
        if (ImGui::Checkbox("Specular", &Lighting.Specular)) SaveLighting();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off: every surface a plain dielectric, its colour whole (to judge a material)");
        std::string Status, File;
        {
            std::lock_guard<std::mutex> Lk(V.LastShot->M);
            if (UiAsync::Detail::Now() < V.LastShot->Until) { Status = V.LastShot->Status; File = V.LastShot->File; }
        }
        if (!Status.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", Status.c_str());
            if (!File.empty())
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("Show")) UiAsync::OpenInShell(File, true);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the file in its folder");
            }
        }
    }

    // Self-screenshot on demand: when the trigger file exists, save the freshly rendered backbuffer
    // (before Present, since DISCARD invalidates it after) to a PNG - lets a headless caller grab the
    // exact UI without stealing focus. Dev aid. The triggers are looked for by a worker, never by
    // the frame (a probe of a sleeping drive would stall it).
    void PollUiShotTriggers(ShellState& S, IDXGISwapChain* Swap)
    {
        ViewportState& V = S.Viewport;
        // GH_UI_SHOT=<path without extension> gives one instance its own trigger/png pair
        static const std::string ShotBase = [] {
            char B[512] = {};
            const DWORD N = GetEnvironmentVariableA("GH_UI_SHOT", B, sizeof(B));
            if (N > 0 && N < sizeof(B)) return std::string(B);
            return (std::filesystem::temp_directory_path() / "graywolf_ui_shot").string();
        }();
        // the UI trigger's there; the viewport one's scale (0: none)
        static UiAsync::Cached<std::pair<bool, int>> Triggers;
        static std::atomic<bool> Consuming{ false };   // a trigger being removed: not seen twice
        const auto [UiTrigger, ViewportScale] = Triggers.Get(ShotBase, 0.1, [] {
            std::error_code Ec;
            int Scale = 0;
            if (std::filesystem::exists(ShotBase + ".viewport.trigger", Ec))
            {
                std::ifstream Trigger(ShotBase + ".viewport.trigger");
                Scale = (Trigger >> Scale && (Scale == 1 || Scale == 2 || Scale == 4)) ? Scale : -1;
            }
            return std::make_pair(std::filesystem::exists(ShotBase + ".trigger", Ec), Scale);
        });
        if (Consuming.load()) {}
        else if (UiTrigger)
        {
            Consuming = true;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> Back;
            auto Shot = std::make_shared<DirectX::ScratchImage>();
            if (SUCCEEDED(Swap->GetBuffer(0, __uuidof(ID3D11Texture2D), &Back)) && SUCCEEDED(DirectX::CaptureTexture(S.Dev.Get(), S.Ctx.Get(), Back.Get(), *Shot)))
                UiAsync::Run("ui shot", [Shot] {
                    const HRESULT Com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    DirectX::SaveToWICFile(*Shot->GetImage(0, 0, 0), DirectX::WIC_FLAGS_NONE, DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG),
                                           std::filesystem::path(ShotBase + ".png").wstring().c_str());
                    std::error_code Ec;
                    std::filesystem::remove(ShotBase + ".trigger", Ec);
                    if (SUCCEEDED(Com)) CoUninitialize();
                    Triggers.Forget();
                    Consuming = false;
                });
            else Consuming = false;
        }
        // <base>.viewport.trigger: the Screenshot button's render, written to <base>.viewport.png
        // on the next frame the 3D view draws (the trigger may hold the scale)
        else if (ViewportScale != 0 && !V.PendingShot.Wanted && !V.Loader.Busy())
        {
            if (ViewportScale > 0) V.ShotScale = ViewportScale;
            V.PendingShot = { true, ShotBase + ".viewport.png", "" };
            Consuming = true;
            UiAsync::Run("ui shot trigger", [] {
                std::error_code Ec;
                std::filesystem::remove(ShotBase + ".viewport.trigger", Ec);
                Triggers.Forget();
                Consuming = false;
            });
        }
    }
}
