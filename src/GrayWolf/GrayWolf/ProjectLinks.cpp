#include "stdafx.h"
#include "ProjectLinks.h"
#include "UiAsync.h"   // the browser opens off the frame

#include "imgui.h"

#include <d3d11.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include <nanosvg/nanosvg.h>
#include <nanosvg/nanosvgrast.h>

using Microsoft::WRL::ComPtr;

namespace
{
    struct Link { const char* Id; const char* Url; const char* Tooltip; const char* Path; };

    // simple-icons 13.21.0 (CC0), each a single path on a 24 x 24 box
    const Link Links[] = {
        { "github", "https://github.com/Kenshin9977/GrayWolf", "GitHub: sources, issues, releases",
          "M12 .297c-6.63 0-12 5.373-12 12 0 5.303 3.438 9.8 8.205 11.385.6.113.82-.258.82-.577 0-.285-.01-1.04-.015-2.04-3.338.724-4.042-1.61-4.042-1.61C4.422 18.07 3.633 17.7 3.633 17.7c-1.087-.744.084-.729.084-.729 1.205.084 1.838 1.236 1.838 1.236 1.07 1.835 2.809 1.305 3.495.998.108-.776.417-1.305.76-1.605-2.665-.3-5.466-1.332-5.466-5.93 0-1.31.465-2.38 1.235-3.22-.135-.303-.54-1.523.105-3.176 0 0 1.005-.322 3.3 1.23.96-.267 1.98-.399 3-.405 1.02.006 2.04.138 3 .405 2.28-1.552 3.285-1.23 3.285-1.23.645 1.653.24 2.873.12 3.176.765.84 1.23 1.91 1.23 3.22 0 4.61-2.805 5.625-5.475 5.92.42.36.81 1.096.81 2.22 0 1.606-.015 2.896-.015 3.286 0 .315.21.69.825.57C20.565 22.092 24 17.592 24 12.297c0-6.627-5.373-12-12-12" },
        { "discord", "https://discord.com/invite/dZ24q8S", "Discord",
          "M20.317 4.3698a19.7913 19.7913 0 00-4.8851-1.5152.0741.0741 0 00-.0785.0371c-.211.3753-.4447.8648-.6083 1.2495-1.8447-.2762-3.68-.2762-5.4868 0-.1636-.3933-.4058-.8742-.6177-1.2495a.077.077 0 00-.0785-.037 19.7363 19.7363 0 00-4.8852 1.515.0699.0699 0 00-.0321.0277C.5334 9.0458-.319 13.5799.0992 18.0578a.0824.0824 0 00.0312.0561c2.0528 1.5076 4.0413 2.4228 5.9929 3.0294a.0777.0777 0 00.0842-.0276c.4616-.6304.8731-1.2952 1.226-1.9942a.076.076 0 00-.0416-.1057c-.6528-.2476-1.2743-.5495-1.8722-.8923a.077.077 0 01-.0076-.1277c.1258-.0943.2517-.1923.3718-.2914a.0743.0743 0 01.0776-.0105c3.9278 1.7933 8.18 1.7933 12.0614 0a.0739.0739 0 01.0785.0095c.1202.099.246.1981.3728.2924a.077.077 0 01-.0066.1276 12.2986 12.2986 0 01-1.873.8914.0766.0766 0 00-.0407.1067c.3604.698.7719 1.3628 1.225 1.9932a.076.076 0 00.0842.0286c1.961-.6067 3.9495-1.5219 6.0023-3.0294a.077.077 0 00.0313-.0552c.5004-5.177-.8382-9.6739-3.5485-13.6604a.061.061 0 00-.0312-.0286zM8.02 15.3312c-1.1825 0-2.1569-1.0857-2.1569-2.419 0-1.3332.9555-2.4189 2.157-2.4189 1.2108 0 2.1757 1.0952 2.1568 2.419 0 1.3332-.9555 2.4189-2.1569 2.4189zm7.9748 0c-1.1825 0-2.1569-1.0857-2.1569-2.419 0-1.3332.9554-2.4189 2.1569-2.4189 1.2108 0 2.1757 1.0952 2.1568 2.419 0 1.3332-.946 2.4189-2.1568 2.4189Z" },
        { "kofi", "https://ko-fi.com/kenshin9977", "Support GrayWolf on Ko-fi",
          "M11.351 2.715c-2.7 0-4.986.025-6.83.26C2.078 3.285 0 5.154 0 8.61c0 3.506.182 6.13 1.585 8.493 1.584 2.701 4.233 4.182 7.662 4.182h.83c4.209 0 6.494-2.234 7.637-4a9.5 9.5 0 0 0 1.091-2.338C21.792 14.688 24 12.22 24 9.208v-.415c0-3.247-2.13-5.507-5.792-5.87-1.558-.156-2.65-.208-6.857-.208m0 1.947c4.208 0 5.09.052 6.571.182 2.624.311 4.13 1.584 4.13 4v.39c0 2.156-1.792 3.844-3.87 3.844h-.935l-.156.649c-.208 1.013-.597 1.818-1.039 2.546-.909 1.428-2.545 3.064-5.922 3.064h-.805c-2.571 0-4.831-.883-6.078-3.195-1.09-2-1.298-4.155-1.298-7.506 0-2.181.857-3.402 3.012-3.714 1.533-.233 3.559-.26 6.39-.26m6.547 2.287c-.416 0-.65.234-.65.546v2.935c0 .311.234.545.65.545 1.324 0 2.051-.754 2.051-2s-.727-2.026-2.052-2.026m-10.39.182c-1.818 0-3.013 1.48-3.013 3.142 0 1.533.858 2.857 1.949 3.897.727.701 1.87 1.429 2.649 1.896a1.47 1.47 0 0 0 1.507 0c.78-.467 1.922-1.195 2.623-1.896 1.117-1.039 1.974-2.364 1.974-3.897 0-1.662-1.247-3.142-3.039-3.142-1.065 0-1.792.545-2.338 1.298-.493-.753-1.246-1.298-2.312-1.298" },
    };

    // The mark as a white Side x Side texture: its alpha is the shape, the draw call tints it
    ComPtr<ID3D11ShaderResourceView> Rasterize(ID3D11Device* Dev, const char* Path, int Side)
    {
        std::string Svg = std::string("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\"><path d=\"") + Path + "\"/></svg>";
        NSVGimage* Image = nsvgParse(Svg.data(), "px", 96.0f);   // parses in place
        if (Image == nullptr) return nullptr;
        NSVGrasterizer* Rast = nsvgCreateRasterizer();
        std::vector<uint8_t> Px((size_t)Side * Side * 4, 0);
        if (Rast != nullptr) nsvgRasterize(Rast, Image, 0, 0, Side / 24.0f, Px.data(), Side, Side, Side * 4);
        nsvgDeleteRasterizer(Rast);
        nsvgDelete(Image);
        if (Rast == nullptr) return nullptr;
        for (size_t K = 0; K < Px.size(); K += 4) Px[K] = Px[K + 1] = Px[K + 2] = 255;
        D3D11_TEXTURE2D_DESC Td{};
        Td.Width = Side;
        Td.Height = Side;
        Td.MipLevels = 1;
        Td.ArraySize = 1;
        Td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        Td.SampleDesc.Count = 1;
        Td.Usage = D3D11_USAGE_IMMUTABLE;
        Td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA Sd{ Px.data(), (UINT)Side * 4, 0 };
        ComPtr<ID3D11Texture2D> T;
        ComPtr<ID3D11ShaderResourceView> S;
        if (FAILED(Dev->CreateTexture2D(&Td, &Sd, &T)) || FAILED(Dev->CreateShaderResourceView(T.Get(), nullptr, &S))) return nullptr;
        return S;
    }

    // One texture per mark and size (the size follows the DPI), made on the frame it is first drawn
    ID3D11ShaderResourceView* Mark(ID3D11Device* Dev, const Link& L, int Side)
    {
        static std::map<std::pair<std::string, int>, ComPtr<ID3D11ShaderResourceView>> Cache;
        auto [It, New] = Cache.try_emplace({ L.Id, Side });
        if (New) It->second = Rasterize(Dev, L.Path, Side);
        return It->second.Get();
    }
}

namespace ProjectLinks
{
    void DrawMenuBarIcons(ID3D11Device* Dev)
    {
        if (Dev == nullptr) return;
        const float H = ImGui::GetFrameHeight();
        const int Side = (int)std::lround(ImGui::GetTextLineHeight());
        const float Gap = ImGui::GetStyle().ItemSpacing.x * 0.5f;
        const float Width = IM_ARRAYSIZE(Links) * H + (IM_ARRAYSIZE(Links) - 1) * Gap;
        const float X = ImGui::GetWindowContentRegionMax().x - Width;
        if (X <= ImGui::GetCursorPosX()) return;   // a window too narrow: the menus first
        ImGui::SetCursorPosX(X);
        ImDrawList* DL = ImGui::GetWindowDrawList();
        for (const Link& L : Links)
        {
            if (&L != Links) ImGui::SameLine(0.0f, Gap);
            const ImVec2 Min = ImGui::GetCursorScreenPos();
            ImGui::PushID(L.Id);
            const bool Hit = ImGui::InvisibleButton("##link", ImVec2(H, H));
            ImGui::PopID();
            const bool Hovered = ImGui::IsItemHovered();
            if (Hovered)
            {
                DL->AddRectFilled(Min, ImVec2(Min.x + H, Min.y + H), ImGui::GetColorU32(ImGuiCol_HeaderHovered), ImGui::GetStyle().FrameRounding);
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("%s", L.Tooltip);
            }
            if (ID3D11ShaderResourceView* Tex = Mark(Dev, L, Side))
            {
                const ImVec2 At(std::floor(Min.x + (H - Side) * 0.5f), std::floor(Min.y + (H - Side) * 0.5f));
                DL->AddImage((ImTextureID)(intptr_t)Tex, At, ImVec2(At.x + Side, At.y + Side), ImVec2(0, 0), ImVec2(1, 1),
                             ImGui::GetColorU32(Hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled));
            }
            if (Hit) UiAsync::OpenUrl(L.Url);
        }
    }
}
