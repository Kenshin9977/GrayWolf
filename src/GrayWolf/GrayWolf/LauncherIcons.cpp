#include "stdafx.h"

#include "LauncherIcons.h"

#include <d3d11.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr int IconPixels = 48;   // drawn smaller, so it stays sharp at any DPI

    std::wstring RegString(HKEY Root, const wchar_t* Key, const wchar_t* Value)
    {
        wchar_t Buf[1024]{};
        DWORD Size = sizeof(Buf) - sizeof(wchar_t);
        if (RegGetValueW(Root, Key, Value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, Buf, &Size) != ERROR_SUCCESS) return L"";
        return Buf;
    }

    // Battle.net's DisplayIcon from its uninstall key ("path" or "path,index")
    std::wstring BattleNetExe()
    {
        for (const wchar_t* Root : { L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Battle.net",
                                     L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Battle.net" })
        {
            std::wstring Icon = RegString(HKEY_LOCAL_MACHINE, Root, L"DisplayIcon");
            if (Icon.empty()) continue;
            if (const size_t Comma = Icon.find(L','); Comma != std::wstring::npos) Icon.resize(Comma);
            if (!Icon.empty() && Icon.front() == L'"') Icon = Icon.substr(1, Icon.size() - 2);
            return Icon;
        }
        return L"";
    }

    HICON Extract(const std::wstring& File, int Index)
    {
        HICON Icon = nullptr;
        if (File.empty() || SHDefExtractIconW(File.c_str(), Index, 0, &Icon, nullptr, IconPixels) != S_OK) return nullptr;
        return Icon;
    }

    // The Xbox app's icon as the Start menu shows it: a packaged app has no exe icon to extract, so
    // its shell item draws it (a premultiplied 32-bit bitmap, made a straight-alpha icon here)
    HICON XboxAppIcon()
    {
        const HRESULT Com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        HICON Icon = nullptr;
        ComPtr<IShellItemImageFactory> Item;
        HBITMAP Bitmap = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(L"shell:AppsFolder\\Microsoft.GamingApp_8wekyb3d8bbwe!Microsoft.Xbox.App",
                                                  nullptr, IID_PPV_ARGS(&Item))) &&
            SUCCEEDED(Item->GetImage({ IconPixels, IconPixels }, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &Bitmap)))
        {
            BITMAP Bm{};
            if (GetObject(Bitmap, sizeof(Bm), &Bm) && Bm.bmBitsPixel == 32 && Bm.bmBits)
            {
                // a DIB section: its rows are bottom-up when bmHeight is positive, which an icon's colour bitmap keeps
                auto* Px = static_cast<uint8_t*>(Bm.bmBits);
                for (size_t K = 0; K < (size_t)Bm.bmWidthBytes * Bm.bmHeight; K += 4)
                    if (const uint8_t A = Px[K + 3]; A != 0 && A != 255)
                        for (int C = 0; C < 3; ++C) Px[K + C] = (uint8_t)std::min(255, Px[K + C] * 255 / A);
                HBITMAP Mask = CreateBitmap(Bm.bmWidth, Bm.bmHeight, 1, 1, nullptr);
                ICONINFO Ii{ TRUE, 0, 0, Mask, Bitmap };
                Icon = CreateIconIndirect(&Ii);
                DeleteObject(Mask);
            }
            DeleteObject(Bitmap);
        }
        if (SUCCEEDED(Com)) CoUninitialize();
        return Icon;
    }

    HICON IconFor(const std::string& Source)
    {
        if (Source == "Steam") return Extract(RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamExe"), 0);
        if (Source == "Battle.net") return Extract(BattleNetExe(), 0);
        if (Source == "Xbox")
            if (HICON Icon = XboxAppIcon()) return Icon;
        SHSTOCKICONINFO Info{};
        Info.cbSize = sizeof(Info);
        if (FAILED(SHGetStockIconInfo(SIID_FOLDER, SHGSI_ICONLOCATION, &Info))) return nullptr;
        return Extract(Info.szPath, Info.iIcon);
    }

    // The icon's pixels, straight BGRA, top-down: its colour bitmap's alpha, or its mask for an old
    // icon with none
    bool Pixels(HICON Icon, int& W, int& H, std::vector<uint8_t>& Out)
    {
        ICONINFO Ii{};
        if (!GetIconInfo(Icon, &Ii)) return false;
        bool Ok = false;
        BITMAP Bm{};
        if (Ii.hbmColor && GetObject(Ii.hbmColor, sizeof(Bm), &Bm))
        {
            W = Bm.bmWidth;
            H = Bm.bmHeight;
            BITMAPINFO Bi{};
            Bi.bmiHeader.biSize = sizeof(Bi.bmiHeader);
            Bi.bmiHeader.biWidth = W;
            Bi.bmiHeader.biHeight = -H;
            Bi.bmiHeader.biPlanes = 1;
            Bi.bmiHeader.biBitCount = 32;
            Bi.bmiHeader.biCompression = BI_RGB;
            Out.assign((size_t)W * H * 4, 0);
            HDC Dc = GetDC(nullptr);
            Ok = GetDIBits(Dc, Ii.hbmColor, 0, H, Out.data(), &Bi, DIB_RGB_COLORS) == H;
            bool AnyAlpha = false;
            for (size_t K = 3; K < Out.size(); K += 4) AnyAlpha |= Out[K] != 0;
            if (Ok && !AnyAlpha && Ii.hbmMask)
            {
                std::vector<uint8_t> Mask((size_t)W * H * 4);
                if (GetDIBits(Dc, Ii.hbmMask, 0, H, Mask.data(), &Bi, DIB_RGB_COLORS) == H)
                    for (size_t K = 0; K < Out.size(); K += 4) Out[K + 3] = Mask[K] ? 0 : 255;
            }
            ReleaseDC(nullptr, Dc);
        }
        if (Ii.hbmColor) DeleteObject(Ii.hbmColor);
        if (Ii.hbmMask) DeleteObject(Ii.hbmMask);
        return Ok;
    }

    ComPtr<ID3D11ShaderResourceView> Upload(ID3D11Device* Dev, HICON Icon)
    {
        int W = 0, H = 0;
        std::vector<uint8_t> Px;
        if (!Pixels(Icon, W, H, Px)) return nullptr;
        D3D11_TEXTURE2D_DESC Td{};
        Td.Width = W;
        Td.Height = H;
        Td.MipLevels = 1;
        Td.ArraySize = 1;
        Td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        Td.SampleDesc.Count = 1;
        Td.Usage = D3D11_USAGE_IMMUTABLE;
        Td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA Sd{ Px.data(), (UINT)W * 4, 0 };
        ComPtr<ID3D11Texture2D> T;
        ComPtr<ID3D11ShaderResourceView> S;
        if (FAILED(Dev->CreateTexture2D(&Td, &Sd, &T)) || FAILED(Dev->CreateShaderResourceView(T.Get(), nullptr, &S))) return nullptr;
        return S;
    }
}

namespace LauncherIcons
{
    ID3D11ShaderResourceView* For(ID3D11Device* Dev, const std::string& Source)
    {
        static std::mutex Lock;
        static std::map<std::string, ComPtr<ID3D11ShaderResourceView>> Cache;   // one per source, tried once
        if (Dev == nullptr) return nullptr;
        const std::string Key = Source == "Steam" || Source == "Battle.net" || Source == "Xbox" ? Source : "folder";
        std::lock_guard<std::mutex> Guard(Lock);
        if (const auto It = Cache.find(Key); It != Cache.end()) return It->second.Get();
        ComPtr<ID3D11ShaderResourceView> S;
        if (HICON Icon = IconFor(Key))
        {
            S = Upload(Dev, Icon);
            DestroyIcon(Icon);
        }
        return (Cache[Key] = S).Get();
    }
}
