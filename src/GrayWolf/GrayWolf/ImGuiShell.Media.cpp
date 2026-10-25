#include "stdafx.h"

// The Viewport's image and audio views.
#include "ImGuiShell.Internal.h"
#include "AppLog.h"           // timed steps
#include "ModelPreview.h"      // ScratchImageToRGBA8
#include "CoDIWITranslator.h"  // TranslateIWI (raw CoD images)
#include "CoDAssetType.h"      // XImageDDS

#include "imgui.h"
#include "DirectXTex.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace ShellUi
{
    // Decodes an image (PNG/JPG via WIC, DDS, TGA, or a raw CoD .iwi via our translator) into an SRV
    // for display via ImGui::Image. Ext is the lowercase extension without the dot.
    ComPtr<ID3D11ShaderResourceView> LoadImageSRV(ID3D11Device* Dev, const std::string& Path, const std::string& Ext, int& OutW, int& OutH)
    {
        TexMetadata Meta{}; ScratchImage Img;
        const std::wstring W(Path.begin(), Path.end());
        HRESULT Hr = E_FAIL;
        AppLog::Timer Read("image read+decode");
        if (Ext == "dds")      Hr = LoadFromDDSFile(W.c_str(), DDS_FLAGS_NONE, &Meta, Img);
        else if (Ext == "tga") Hr = LoadFromTGAFile(W.c_str(), &Meta, Img);
        else if (Ext == "iwi")
        {
            std::ifstream In(Path, std::ios::binary);
            if (!In) return nullptr;
            std::vector<uint8_t> Raw((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
            if (Raw.empty()) return nullptr;
            auto Buf = std::make_unique<uint8_t[]>(Raw.size());
            std::memcpy(Buf.get(), Raw.data(), Raw.size());
            auto Dds = CoDIWITranslator::TranslateIWI(Buf, (uint32_t)Raw.size());
            if (!Dds || Dds->DataBuffer == nullptr || Dds->DataSize == 0) return nullptr;
            Hr = LoadFromDDSMemory((const uint8_t*)Dds->DataBuffer, (size_t)Dds->DataSize, DDS_FLAGS_NONE, &Meta, Img);
        }
        else Hr = LoadFromWICFile(W.c_str(), WIC_FLAGS_NONE, &Meta, Img);
        Read.Detail(Ext + " " + std::to_string(Meta.width) + "x" + std::to_string(Meta.height));
        if (FAILED(Hr))
        {
            AppLog::Log().warn("image {} did not decode: hr {:#x}", Path, (unsigned)Hr);
            return nullptr;
        }

        // Decompress BCn / convert to straight RGBA8 (the same DirectXTex call as ModelPreview.cpp's).
        AppLog::Timer Rgba("image to RGBA8");
        int W8 = 0, H8 = 0; std::vector<uint8_t> Pixels;
        if (!ScratchImageToRGBA8(Img, Meta, W8, H8, Pixels)) return nullptr;

        OutW = W8; OutH = H8;
        AppLog::Timer Upload("image GPU upload");
        D3D11_TEXTURE2D_DESC Td{}; Td.Width = OutW; Td.Height = OutH; Td.MipLevels = 1; Td.ArraySize = 1;
        Td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; Td.SampleDesc.Count = 1; Td.Usage = D3D11_USAGE_IMMUTABLE; Td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA Sd{}; Sd.pSysMem = Pixels.data(); Sd.SysMemPitch = (UINT)OutW * 4;
        ComPtr<ID3D11Texture2D> T; if (FAILED(Dev->CreateTexture2D(&Td, &Sd, &T))) return nullptr;
        ComPtr<ID3D11ShaderResourceView> S; Dev->CreateShaderResourceView(T.Get(), nullptr, &S); return S;
    }

    // Shows an image at its own pixel size by default, with Fit/100%/+/- to zoom and a scrollable
    // region to pan once zoomed past the available space. Zoom is the caller's persistent state (see
    // ImageZoom/LastZoomedImage).
    void DrawZoomableImage(ID3D11ShaderResourceView* SRV, int W, int H, float& Zoom)
    {
        if (SRV == nullptr || W <= 0 || H <= 0) return;

        if (ImGui::Button("-")) Zoom = (std::max)(0.05f, Zoom / 1.25f);
        ImGui::SameLine();
        if (ImGui::Button("100%")) Zoom = 1.0f;
        ImGui::SameLine();
        if (ImGui::Button("Fit"))
        {
            const ImVec2 Avail = ImGui::GetContentRegionAvail();
            const float FitScale = (std::min)(Avail.x / (float)W, (std::max)(1.0f, Avail.y - 4.0f) / (float)H);
            Zoom = (FitScale > 0.0f) ? (std::min)(1.0f, FitScale) : 1.0f;
        }
        ImGui::SameLine();
        if (ImGui::Button("+")) Zoom = (std::min)(16.0f, Zoom * 1.25f);
        ImGui::SameLine();
        ImGui::TextDisabled("%.0f%%", Zoom * 100.0f);

        if (ImGui::BeginChild("zoomable_image", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar))
        {
            const ImVec2 Sz((float)W * Zoom, (float)H * Zoom);
            const ImVec2 Avail = ImGui::GetContentRegionAvail();
            // Centered only while it still fits - once zoomed past the child, scrollbars take
            // over and a centering offset would just clip the far edge unreachable.
            if (Sz.x < Avail.x) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (Avail.x - Sz.x) * 0.5f);
            if (Sz.y < Avail.y) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (Avail.y - Sz.y) * 0.5f);

            ImGui::Image((ImTextureID)(intptr_t)SRV, Sz);

            // Ctrl+wheel to zoom while hovering - plain wheel is left alone so it still scrolls/
            // pans like any other scrollable region.
            if (ImGui::IsWindowHovered() && ImGui::GetIO().KeyCtrl && ImGui::GetIO().MouseWheel != 0.0f)
                Zoom = (std::max)(0.05f, (std::min)(16.0f,
                    Zoom * (ImGui::GetIO().MouseWheel > 0.0f ? 1.1f : 1.0f / 1.1f)));
        }
        ImGui::EndChild();
    }

    // Transport + waveform for whatever is loaded in Audio.
    // FormatLabel is only the display string (e.g. "flac").
    void DrawAudioPlayer(AudioPlayer& Audio, const std::string& FormatLabel)
    {
        auto Fmt = [](double s, char* out, size_t n) {
            if (s < 0) s = 0; int m = (int)(s / 60); int sec = (int)(s - m * 60);
            int cs = (int)((s - (int)s) * 100); snprintf(out, n, "%d:%02d.%02d", m, sec, cs);
        };

        if (!Audio.IsLoaded())
        {
            ImGui::TextDisabled("could not decode this audio (%s)", FormatLabel.c_str());
            return;
        }

        const double Pos = Audio.Position(), Dur = Audio.Duration();
        const bool Playing = Audio.IsPlaying();

        if (ImGui::Button(Playing ? "Pause" : "Play", ImVec2(90, 0))) Audio.TogglePlay();
        ImGui::SameLine();
        if (ImGui::Button("Stop", ImVec2(90, 0))) Audio.Stop();
        ImGui::SameLine();
        // Plain ASCII: segoeui.ttf lacks these symbol glyphs (U+25B6/U+23F8/U+25A0) whatever the ranges.
        ImGui::TextUnformatted(Playing ? "  > playing" : (Pos > 0.0 && Pos < Dur ? "  || paused" : "  [] stopped"));

        ImGui::Spacing();

        const std::vector<float>& Lo = Audio.PeaksMin();
        const std::vector<float>& Hi = Audio.PeaksMax();
        ImVec2 WaveSz(ImGui::GetContentRegionAvail().x, 120.0f);
        ImVec2 P0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##wave", WaveSz);
        const bool WaveActive = ImGui::IsItemActive();
        ImDrawList* DL = ImGui::GetWindowDrawList();
        const ImU32 ColBg   = ImGui::GetColorU32(ImGuiCol_FrameBg);
        const ImU32 ColWave = IM_COL32(120, 170, 220, 200);
        const ImU32 ColPlayed = IM_COL32(200, 150, 62, 230);
        const ImU32 ColHead = IM_COL32(235, 235, 235, 255);
        DL->AddRectFilled(P0, ImVec2(P0.x + WaveSz.x, P0.y + WaveSz.y), ColBg, 4.0f);
        const float MidY = P0.y + WaveSz.y * 0.5f;
        const float PlayX = (Dur > 0.0) ? P0.x + WaveSz.x * (float)(Pos / Dur) : P0.x;
        if (!Lo.empty())
        {
            const int N = (int)Lo.size();
            for (int px = 0; px < (int)WaveSz.x; ++px)
            {
                const int idx = (int)((px / WaveSz.x) * N);
                const float lo = Lo[(std::min)(idx, N - 1)], hi = Hi[(std::min)(idx, N - 1)];
                const float x = P0.x + px;
                const float y0 = MidY - hi * (WaveSz.y * 0.48f);
                const float y1 = MidY - lo * (WaveSz.y * 0.48f);
                DL->AddLine(ImVec2(x, y0), ImVec2(x, y1), x <= PlayX ? ColPlayed : ColWave, 1.0f);
            }
        }
        DL->AddLine(ImVec2(PlayX, P0.y), ImVec2(PlayX, P0.y + WaveSz.y), ColHead, 1.5f);
        if (WaveActive && Dur > 0.0)
        {
            const float mx = ImGui::GetIO().MousePos.x - P0.x;
            Audio.Seek((mx / WaveSz.x) * Dur);
        }

        char cur[24], tot[24]; Fmt(Pos, cur, sizeof cur); Fmt(Dur, tot, sizeof tot);
        ImGui::Spacing();
        ImGui::ProgressBar(Dur > 0.0 ? (float)(Pos / Dur) : 0.0f, ImVec2(-FLT_MIN, 0), "");
        ImGui::Text("%s / %s", cur, tot);

        ImGui::Spacing();
        ImGui::TextDisabled("%s  \xC2\xB7  %u Hz  \xC2\xB7  %s",
            FormatLabel.c_str(), Audio.SampleRate(), Audio.Channels() == 1 ? "mono" : Audio.Channels() == 2 ? "stereo" : "multi");
    }
}
