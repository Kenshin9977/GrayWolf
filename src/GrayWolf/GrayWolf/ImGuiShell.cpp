#include "stdafx.h"

#include "ImGuiShell.h"
#include "AppLog.h"           // session log, timed steps, UI stalls
#include "ModelPreview.h"      // LoadPreviewGeometry / PreviewGeometry
#include "ExportNaming.h"      // Lower
#include "ModelPreviewGPU.h"   // UploadPreviewMesh
#include "PreviewLoader.h"     // models built off the UI thread
#include "SeAnimReader.h"      // ReadSeAnim / SeAnimClip
#include "CastReader.h"        // ReadAnimClip, CastFile::ModelCounts / FileKind
#include "SkeletalPreview.h"   // EvaluateSkeletonPose / SkinPreviewGeometry
#include "CoDIWITranslator.h"  // TranslateIWI (raw CoD images)
#include "CoDAssetType.h"      // XImageDDS
#include "Sound.h"             // FLAC/Opus -> WAV for playback
#include "AudioPlayer.h"       // real audio playback (miniaudio) with cursor + waveform
#include "ImGuiLoadGamePanel.h" // the Load Game window: a game exported from its files
#include "ImGuiDialogs.h"      // SetBuf, AbsoluteFolder
#include "UiAsync.h"           // dialogs, shell opens and disk lookups off the frame
#include "Strings.h"
#include "SettingsManager.h"   // the screenshot's folder and scale
#include "ImGuiUpdates.h"      // Settings > Updates, the new-version banner
#include "ImGuiBlender.h"      // Settings > Blender, a map's Open in Blender

#include "imgui.h"
#include "imgui_internal.h"   // DockBuilder* for the initial layout
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "DirectXTex.h"       // CaptureTexture / SaveToWICFile (self-screenshot)
#include <nlohmann/json.hpp>           // filter presets

#include <d3d11.h>
#include <wrl/client.h>
#include <mmsystem.h>        // PlaySound (WAV playback)
#include <shlobj.h>          // SHGetKnownFolderPath (Pictures)
#include <ctime>

#include <vector>
#include <set>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <memory>
#include <fstream>
#include <filesystem>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <unordered_map>

#pragma comment(lib, "imgui.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "winmm.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// From imgui_impl_win32 (forward the raw window messages to ImGui).
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{
    // Window state the WndProc mutates.
    struct WinState { int W = 1280, H = 800; bool Resized = false; bool Quit = false; };

    LRESULT CALLBACK WndProc(HWND Hwnd, UINT Msg, WPARAM W, LPARAM L)
    {
        if (ImGui_ImplWin32_WndProcHandler(Hwnd, Msg, W, L)) return true;
        WinState* S = reinterpret_cast<WinState*>(GetWindowLongPtr(Hwnd, GWLP_USERDATA));
        switch (Msg)
        {
        case WM_SIZE:
            if (S && W != SIZE_MINIMIZED) { S->W = (int)LOWORD(L); S->H = (int)HIWORD(L); S->Resized = true; }
            return 0;
        case WM_GETMINMAXINFO:
        {
            // Clamp the maximized window to the monitor WORK area so the bottom status bar is never
            // hidden behind the Windows taskbar.
            MONITORINFO Mi{ sizeof(Mi) };
            if (GetMonitorInfo(MonitorFromWindow(Hwnd, MONITOR_DEFAULTTONEAREST), &Mi))
            {
                MINMAXINFO* Mm = reinterpret_cast<MINMAXINFO*>(L);
                Mm->ptMaxPosition.x = Mi.rcWork.left - Mi.rcMonitor.left;
                Mm->ptMaxPosition.y = Mi.rcWork.top  - Mi.rcMonitor.top;
                Mm->ptMaxSize.x = Mm->ptMaxTrackSize.x = Mi.rcWork.right  - Mi.rcWork.left;
                Mm->ptMaxSize.y = Mm->ptMaxTrackSize.y = Mi.rcWork.bottom - Mi.rcWork.top;
            }
            return 0;
        }
        case WM_CLOSE:   if (S) S->Quit = true; DestroyWindow(Hwnd); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        }
        return DefWindowProc(Hwnd, Msg, W, L);
    }

    // GPU resources for one loaded model; copies share them.
    using GpuMesh = PreviewGpuMesh;
    GpuMesh BuildGpuMesh(ID3D11Device* Dev, const PreviewGeometry& Geo) { return UploadPreviewMesh(Dev, Geo); }

    // Rebuilds only M's vertex buffer from freshly-skinned Pos/Nrm (ranges, SRVs and framing stay from
    // the bind-pose build): called every displayed frame during playback. Vertex count/topology must
    // match the original build; a mismatch is a no-op.
    void UpdateGpuMeshVertices(ID3D11Device* Dev, GpuMesh& M, const std::vector<float>& Pos, const std::vector<float>& Nrm, const std::vector<float>& UV)
    {
        const UINT VertexCount = (UINT)(Pos.size() / 3);
        if (VertexCount == 0 || VertexCount != M.VertexCount || Nrm.size() != Pos.size() || UV.size() != VertexCount * 2) return;
        const std::vector<float> Vtx = InterleavePreviewVertices(Pos, Nrm, UV);
        D3D11_BUFFER_DESC Bd{}; Bd.ByteWidth = (UINT)(Vtx.size() * sizeof(float)); Bd.Usage = D3D11_USAGE_IMMUTABLE; Bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA Sd{}; Sd.pSysMem = Vtx.data();
        ComPtr<ID3D11Buffer> NewVB;
        if (SUCCEEDED(Dev->CreateBuffer(&Bd, &Sd, &NewVB))) M.VB = NewVB;
    }

    bool NameMatches(const std::string& Query, const std::string& Name)
    {
        // Everything-style: whitespace-separated terms ANDed, '!' negates, case-insensitive.
        using ExportNaming::Lower;
        const std::string LName = Lower(Name);
        size_t i = 0;
        bool any = false;
        while (i < Query.size())
        {
            while (i < Query.size() && isspace((unsigned char)Query[i])) i++;
            size_t j = i; while (j < Query.size() && !isspace((unsigned char)Query[j])) j++;
            if (j > i)
            {
                any = true;
                std::string Term = Query.substr(i, j - i);
                bool Neg = false; if (Term[0]=='!'){ Neg=true; Term.erase(Term.begin()); }
                if (!Term.empty())
                {
                    const bool Has = LName.find(Lower(Term)) != std::string::npos;
                    if (Neg ? Has : !Has) return false;
                }
            }
            i = j;
        }
        (void)any; return true;
    }

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

    // Reads just the pixel dimensions from an image header (per format), cheaply, during the scan.
    void ReadImageDims(const std::string& Path, const std::string& Ext, int& W, int& H)
    {
        W = 0; H = 0;
        std::ifstream In(Path, std::ios::binary);
        if (!In) return;
        uint8_t B[32] = {};
        In.read((char*)B, sizeof(B));
        if (In.gcount() < 24) return;
        auto BE32 = [&](int o) { return (uint32_t)((B[o] << 24) | (B[o+1] << 16) | (B[o+2] << 8) | B[o+3]); };
        auto LE32 = [&](int o) { return (uint32_t)(B[o] | (B[o+1] << 8) | (B[o+2] << 16) | ((uint32_t)B[o+3] << 24)); };
        auto LE16 = [&](int o) { return (uint16_t)(B[o] | (B[o+1] << 8)); };
        if (Ext == "png")      { W = (int)BE32(16); H = (int)BE32(20); }
        else if (Ext == "dds") { H = (int)LE32(12); W = (int)LE32(16); }
        else if (Ext == "iwi") { W = (int)LE16(6);  H = (int)LE16(8); }
        else if (Ext == "tga") { W = (int)LE16(12); H = (int)LE16(14); }
    }

    // Reads a map .glb's mesh/vertex/tri counts cheaply: a GLB is a 12-byte header, a length-prefixed
    // JSON chunk, then the (often huge) binary chunk; only the JSON chunk is parsed.
    bool ReadGlbMeta(const std::string& Path, uint32_t& Meshes, uint32_t& Verts, uint32_t& Tris)
    {
        Meshes = Verts = Tris = 0;
        std::ifstream In(Path, std::ios::binary);
        if (!In) return false;
        uint8_t Hdr[12];
        if (!In.read((char*)Hdr, sizeof(Hdr)) || std::memcmp(Hdr, "glTF", 4) != 0) return false;
        uint32_t ChunkLen = 0, ChunkType = 0;
        if (!In.read((char*)&ChunkLen, 4) || !In.read((char*)&ChunkType, 4)) return false;
        if (ChunkType != 0x4E4F534Au) return false; // "JSON"
        std::string Json((size_t)ChunkLen, '\0');
        if (!In.read(Json.data(), (std::streamsize)ChunkLen)) return false;

        try
        {
            nlohmann::json J = nlohmann::json::parse(Json);
            if (!J.contains("meshes") || !J.contains("accessors")) return false;
            Meshes = (uint32_t)J["meshes"].size();
            const auto& Accessors = J["accessors"];
            for (const auto& Mesh : J["meshes"])
                for (const auto& Prim : Mesh.value("primitives", nlohmann::json::array()))
                {
                    const auto Attrs = Prim.value("attributes", nlohmann::json::object());
                    if (Attrs.contains("POSITION"))
                    {
                        const int Ai = Attrs["POSITION"].get<int>();
                        if (Ai >= 0 && Ai < (int)Accessors.size()) Verts += Accessors[Ai].value("count", 0u);
                    }
                    if (Prim.contains("indices"))
                    {
                        const int Ai = Prim["indices"].get<int>();
                        if (Ai >= 0 && Ai < (int)Accessors.size()) Tris += Accessors[Ai].value("count", 0u) / 3;
                    }
                }
            return true;
        }
        catch (...) { return false; }
    }
}

// Human-readable file size, Windows-Explorer style: 1024-based steps labelled KB/MB/GB.
static std::string FormatSize(long long Bytes)
{
    if (Bytes < 0) Bytes = 0;
    const char* Units[] = { "B", "KB", "MB", "GB", "TB" };
    double V = (double)Bytes; int U = 0;
    while (V >= 1024.0 && U < 4) { V /= 1024.0; ++U; }
    char Buf[32];
    if (U == 0)      snprintf(Buf, sizeof Buf, "%lld B", Bytes);
    else if (V >= 100) snprintf(Buf, sizeof Buf, "%.0f %s", V, Units[U]);
    else             snprintf(Buf, sizeof Buf, "%.1f %s", V, Units[U]);
    return Buf;
}

// Where the viewport's screenshots go: the screenshot_dir setting; by default the folder the old
// save dialog last used, else Pictures\GrayWolf. Created when a shot is saved.
static std::string ScreenshotDir()
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
static void OpenScreenshotDir()
{
    const std::string Dir = ScreenshotDir();
    UiAsync::Run("create " + Dir, [Dir] {
        std::error_code Ec;
        std::filesystem::create_directories(Strings::ToUnicodeString(Dir), Ec);
        UiAsync::OpenInShell(Dir);
    });
}

// Settings > Screenshots: the folder, picked or typed, opened, or back to its default.
static void DrawScreenshotSettingsMenu()
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

// Settings > Sounds: the format every export writes sounds in (setting exportsnd, what
// --audio-format sets for one run; ExportWriters::WriteSound)
static void DrawSoundSettingsMenu()
{
    if (!ImGui::BeginMenu("Settings")) return;   // appended to the Settings menu ImGuiUpdates opens
    if (ImGui::BeginMenu("Sounds"))
    {
        const std::string Current = Strings::ToUpper(SettingsManager::GetSetting("exportsnd", "WAV"));
        const struct { const char* Value; const char* Label; } Formats[] = {
            { "WAV", "WAV (as the game keeps them, or decoded)" },
            { "FLAC", "FLAC (lossless)" },
            { "OPUS", "Opus (the game's own packets when it keeps Opus, else encoded)" },
        };
        for (const auto& F : Formats)
            if (ImGui::MenuItem(F.Label, nullptr, Current == F.Value)) SettingsManager::SetSetting("exportsnd", F.Value);
        ImGui::EndMenu();
    }
    ImGui::EndMenu();
}

bool RunImGuiShell(const std::string& ModelsDir)
{
    // Crisp under display scaling.
    if (HMODULE User32 = GetModuleHandleW(L"user32.dll"))
    {
        using SetCtxFn = BOOL(WINAPI*)(HANDLE);
        if (auto Fn = reinterpret_cast<SetCtxFn>(GetProcAddress(User32, "SetProcessDpiAwarenessContext")))
            Fn(reinterpret_cast<HANDLE>(-4));
        else SetProcessDPIAware();
    }

    WinState State;

    HINSTANCE Inst = GetModuleHandle(nullptr);
    WNDCLASSEX Wc{}; Wc.cbSize=sizeof(Wc); Wc.style=CS_HREDRAW|CS_VREDRAW; Wc.lpfnWndProc=WndProc;
    Wc.hInstance=Inst; Wc.hCursor=LoadCursor(nullptr,IDC_ARROW); Wc.lpszClassName=L"GrayWolfImGuiShell";
    RegisterClassEx(&Wc);
    HWND Hwnd = CreateWindowEx(0, Wc.lpszClassName, L"GrayWolf - Asset Browser (ImGui + D3D11)",
                               WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, State.W, State.H,
                               nullptr, nullptr, Inst, nullptr);
    if (!Hwnd) return false;
    SetWindowLongPtr(Hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&State));
    UiAsync::SetOwner(Hwnd);   // the file dialogs' owner: on top of it, and it disabled while they are up

    DXGI_SWAP_CHAIN_DESC Scd{};
    Scd.BufferCount=2; Scd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; Scd.BufferDesc.Width=State.W; Scd.BufferDesc.Height=State.H;
    Scd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; Scd.OutputWindow=Hwnd; Scd.SampleDesc.Count=1; Scd.Windowed=TRUE; Scd.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    ComPtr<ID3D11Device> Dev; ComPtr<ID3D11DeviceContext> Ctx; ComPtr<IDXGISwapChain> Swap;
    const D3D_FEATURE_LEVEL Want[]={D3D_FEATURE_LEVEL_11_0}; D3D_FEATURE_LEVEL Got{};
    HRESULT Hr = D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,Want,1,D3D11_SDK_VERSION,&Scd,&Swap,&Dev,&Got,&Ctx);
    if (FAILED(Hr)) Hr = D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,Want,1,D3D11_SDK_VERSION,&Scd,&Swap,&Dev,&Got,&Ctx);
    if (FAILED(Hr)) { DestroyWindow(Hwnd); return false; }

    ComPtr<ID3D11RenderTargetView> BackRTV;
    auto CreateBackRTV = [&]{ BackRTV.Reset(); ComPtr<ID3D11Texture2D> B; if (SUCCEEDED(Swap->GetBuffer(0,__uuidof(ID3D11Texture2D),&B))) Dev->CreateRenderTargetView(B.Get(),nullptr,&BackRTV); };
    CreateBackRTV();

    ShowWindow(Hwnd, SW_SHOWMAXIMIZED);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr;

    float Dpi = 1.0f;
    if (HMODULE U = GetModuleHandleW(L"user32.dll"))
    {
        using GetDpiFn = UINT(WINAPI*)(HWND);
        if (auto Fn = reinterpret_cast<GetDpiFn>(GetProcAddress(U, "GetDpiForWindow")))
        {
            const UINT D = Fn(Hwnd);
            if (D > 0) Dpi = (float)D / 96.0f;
        }
    }

    // Segoe UI at physical pixel size; falls back to the default if unavailable.
    // The atlas only rasterizes glyphs in the ranges given (default: up to U+00FF), so the symbols this
    // shell uses beyond that (U+25B6/U+23F8/U+25A0, U+2212, U+2013, U+2026, U+2248) are added explicitly.
    // The games' level names (DiskGame::LevelNames) add Latin Extended (Polish, Czech...), Greek and
    // Cyrillic, from Segoe UI too, and CODOL's Chinese, merged from Microsoft YaHei (its common
    // simplified set, without oversampling: the atlas stays small) when the system has it.
    static ImVector<ImWchar> ExtraGlyphRanges;
    {
        static const ImWchar LatinExtended[] = { 0x0100, 0x024F, 0x0370, 0x03FF, 0x2000, 0x206F, 0 };
        ImFontGlyphRangesBuilder Builder;
        Builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
        Builder.AddRanges(LatinExtended);
        Builder.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
        Builder.AddText("\xE2\x96\xB6\xE2\x8F\xB8\xE2\x96\xA0\xE2\x88\x92\xE2\x80\x93\xE2\x80\xA6\xE2\x89\x88"); // ▶⏸■−–…≈
        Builder.BuildRanges(&ExtraGlyphRanges);
    }
    ImFontConfig Fc; Fc.OversampleH = 2; Fc.OversampleV = 2;
    if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f * Dpi, &Fc, ExtraGlyphRanges.Data))
        io.Fonts->AddFontDefault();
    else if (GetFileAttributesW(L"C:\\Windows\\Fonts\\msyh.ttc") != INVALID_FILE_ATTRIBUTES)
    {
        ImFontConfig Cjk; Cjk.MergeMode = true; Cjk.OversampleH = 1; Cjk.OversampleV = 1;
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 17.0f * Dpi, &Cjk, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    }
    io.FontGlobalScale = 1.0f;

    // Softer, more finished dark theme (rounded frames, roomier padding, a blue accent).
    ImGui::StyleColorsDark();
    ImGuiStyle& St = ImGui::GetStyle();
    St.WindowRounding = 6.0f; St.FrameRounding = 5.0f; St.GrabRounding = 4.0f; St.TabRounding = 5.0f; St.ScrollbarRounding = 6.0f;
    St.WindowPadding = ImVec2(10, 10); St.FramePadding = ImVec2(9, 5); St.ItemSpacing = ImVec2(8, 7); St.CellPadding = ImVec2(7, 4);
    St.Colors[ImGuiCol_WindowBg]        = ImVec4(0.11f, 0.115f, 0.13f, 1.0f);
    St.Colors[ImGuiCol_FrameBg]         = ImVec4(0.17f, 0.18f, 0.20f, 1.0f);
    St.Colors[ImGuiCol_Header]          = ImVec4(0.20f, 0.33f, 0.48f, 1.0f);
    St.Colors[ImGuiCol_HeaderHovered]   = ImVec4(0.26f, 0.42f, 0.60f, 1.0f);
    St.Colors[ImGuiCol_HeaderActive]    = ImVec4(0.30f, 0.48f, 0.68f, 1.0f);
    St.Colors[ImGuiCol_TitleBgActive]   = ImVec4(0.15f, 0.22f, 0.30f, 1.0f);
    St.Colors[ImGuiCol_TableHeaderBg]   = ImVec4(0.17f, 0.19f, 0.23f, 1.0f);
    // Blue-accent buttons + zebra rows + subtle borders for a more finished feel.
    St.Colors[ImGuiCol_Button]          = ImVec4(0.20f, 0.30f, 0.44f, 1.0f);
    St.Colors[ImGuiCol_ButtonHovered]   = ImVec4(0.26f, 0.40f, 0.58f, 1.0f);
    St.Colors[ImGuiCol_ButtonActive]    = ImVec4(0.30f, 0.48f, 0.68f, 1.0f);
    St.Colors[ImGuiCol_CheckMark]       = ImVec4(0.40f, 0.62f, 0.92f, 1.0f);
    St.Colors[ImGuiCol_SliderGrab]      = ImVec4(0.36f, 0.56f, 0.82f, 1.0f);
    St.Colors[ImGuiCol_SliderGrabActive]= ImVec4(0.44f, 0.66f, 0.95f, 1.0f);
    St.Colors[ImGuiCol_Tab]             = ImVec4(0.16f, 0.20f, 0.27f, 1.0f);
    St.Colors[ImGuiCol_TabHovered]      = ImVec4(0.28f, 0.44f, 0.62f, 1.0f);
    St.Colors[ImGuiCol_Border]          = ImVec4(1.0f, 1.0f, 1.0f, 0.07f);
    St.Colors[ImGuiCol_TableBorderLight]= ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    St.Colors[ImGuiCol_TableRowBg]      = ImVec4(1.0f, 1.0f, 1.0f, 0.0f);
    St.Colors[ImGuiCol_TableRowBgAlt]   = ImVec4(1.0f, 1.0f, 1.0f, 0.025f);
    St.ScaleAllSizes(Dpi);

    ImGui_ImplWin32_Init(Hwnd);
    ImGui_ImplDX11_Init(Dev.Get(), Ctx.Get());

    LoadGamePanel LoadGame;

    // Kind: 0 model · 1 image · 2 audio · 3 anim · 4 map.
    // Root: the listed folder the file was found under; Written: its last write time (the newer of two
    // copies of one asset is the one shown)
    struct Item { std::string Path, Name, Format, Root; int Kind=0; uint32_t Bones=0, Meshes=0, Mats=0; int ImgW=0, ImgH=0; long long SizeBytes=0; long long Written=0; bool ModelTexture=false; uint32_t Frames=0; float FrameRate=0.0f; bool Looping=false; uint32_t Verts=0, Tris=0; uint64_t Id=0; };
    std::vector<Item> Models;
    // Bumped whenever Models changes order: the Assets table's filtered, sorted view is built again
    // only then (not every frame: 60,000 rows take a while).
    uint64_t ModelsVersion = 0;
    uint64_t HeaderVersion = 0;   // bumped when header fields come in (ScanWorker)
    std::set<std::string> KnownPaths; // dedup guard for MergeListed: a folder listed again (after a disk export) adds only new files
    // One row per asset: the same kind, format and name under one listed folder (an asset exported
    // twice, into an older layout's zone folder and the current one, or a texture copied beside each
    // model that uses it) is one row, the newest copy's (then the first path). Key -> the row's Id.
    std::unordered_map<std::string, uint64_t> AssetRows;
    const auto AssetKey = [](const Item& M) {
        std::string Key = M.Root + '|' + std::to_string(M.Kind) + '|' + M.Format + '|' + M.Name;
        for (auto& c : Key) c = (char)tolower((unsigned char)c);
        return Key;
    };

    // Async header fill: ListFolder's walk lists a file from its directory entry alone;
    // the scan workers (ScanLoop) open each one for header fields and hands results back via ScanDone, applied to
    // Models by DrainScanResults once per UI frame: the only place a header field is written past
    // the fast pass, and only on the UI thread.
    std::mutex ScanMx;
    std::condition_variable ScanCv;
    struct ScanJob { uint64_t Id; std::string Path; int Kind; std::string Format; long long SizeBytes; };
    struct ScanResult { uint64_t Id=0; std::string Path; uint32_t Bones=0, Meshes=0, Mats=0; int ImgW=0, ImgH=0; uint32_t Frames=0; float FrameRate=0.0f; bool Looping=false; uint32_t Verts=0, Tris=0; };
    // Models, anims and maps are read before images: an export holds ten times more images than
    // models, and a model's counts behind 50,000 image headers came minutes later (the Bones / Meshes
    // / Mats columns and filters stood at 0 meanwhile)
    std::deque<ScanJob> ScanQueue, ScanImages;
    std::vector<ScanResult> ScanDone;
    bool ScanStop = false;
    uint64_t NextItemId = 1;
    std::unordered_map<uint64_t, size_t> IdToIndex; // rebuilt after every sort; kept live on push_back
    auto RebuildIdIndex = [&] { IdToIndex.clear(); for (size_t i = 0; i < Models.size(); i++) IdToIndex[Models[i].Id] = i; };

    // a few readers: a header read is one small file open, bound by the disk's latency, not its rate
    const auto ScanLoop = [&ScanMx, &ScanCv, &ScanQueue, &ScanImages, &ScanDone, &ScanStop] {
        for (;;)
        {
            ScanJob Job;
            {
                std::unique_lock<std::mutex> Lk(ScanMx);
                ScanCv.wait(Lk, [&] { return ScanStop || !ScanQueue.empty() || !ScanImages.empty(); });
                if (ScanStop) return; // shutting down - the app is closing, no need to drain the rest
                auto& From = !ScanQueue.empty() ? ScanQueue : ScanImages;
                if (From.empty()) continue; // spurious wake
                Job = std::move(From.front());
                From.pop_front();
            }
            ScanResult R; R.Id = Job.Id; R.Path = Job.Path;
            if (Job.Format == "semodel")
                ReadSEModelHeaderCounts(Job.Path, R.Bones, R.Meshes, R.Mats);
            else if (Job.Format == "cast" && Job.Kind == 0)
                CastFile::ModelCounts(Job.Path, R.Bones, R.Meshes, R.Mats);
            else if (Job.Format == "cast" && Job.Kind == 3)
            {
                SeAnimClip Clip;   // Cast keeps no header counts: the curves are the anim's length
                if (ReadCastAnim(Job.Path, Clip))
                {
                    R.Looping = Clip.Looping;
                    R.FrameRate = Clip.FrameRate;
                    R.Frames = Clip.FrameCount;
                    R.Bones = (uint32_t)Clip.Bones.size();
                }
            }
            else if (Job.Format == "xmodel_export")
            {
                // Text format: NUMBONES sits at the top (cheap head read); NUMOBJECTS/NUMMATERIALS
                // sit right before the material list, near EOF - a tail read finds them without
                // scanning the whole (often multi-MB) per-vertex/per-face body.
                std::ifstream In(Job.Path, std::ios::binary);
                if (In)
                {
                    char Head[4096] = {};
                    In.read(Head, sizeof(Head) - 1);
                    std::string HeadStr(Head, (size_t)In.gcount());
                    if (auto Pos = HeadStr.find("NUMBONES "); Pos != std::string::npos)
                        R.Bones = (uint32_t)std::atoi(HeadStr.c_str() + Pos + 9);

                    const long long TailLen = std::min<long long>(Job.SizeBytes, 65536);
                    if (TailLen > 0)
                    {
                        std::string Tail((size_t)TailLen, '\0');
                        In.seekg(Job.SizeBytes - TailLen, std::ios::beg);
                        In.read(Tail.data(), TailLen);
                        if (auto Pos = Tail.find("NUMOBJECTS "); Pos != std::string::npos)
                            R.Meshes = (uint32_t)std::atoi(Tail.c_str() + Pos + 11);
                        if (auto Pos = Tail.find("NUMMATERIALS "); Pos != std::string::npos)
                            R.Mats = (uint32_t)std::atoi(Tail.c_str() + Pos + 13);
                    }
                }
            }
            else if (Job.Kind == 1) ReadImageDims(Job.Path, Job.Format, R.ImgW, R.ImgH);
            else if (Job.Format == "seanim")
            {
                // Fixed binary header (see SEAnimExport.cpp): "SEAnim"(6) version(2) headerSize(2)
                // animType(1) flags(1, bit0=looping) dataFlags(1) dataPropFlags(1) reserved(2)
                // frameRate(4) frameCount(4) boneCount(4) ...
                std::ifstream In(Job.Path, std::ios::binary); uint8_t H[28];
                if (In.read((char*)H, sizeof(H)) && std::memcmp(H, "SEAnim", 6) == 0)
                {
                    R.Looping = (H[11] & 1) != 0;
                    std::memcpy(&R.FrameRate, H+16, 4);
                    std::memcpy(&R.Frames,   H+20, 4);
                    std::memcpy(&R.Bones,    H+24, 4);
                }
            }
            else if (Job.Format == "glb") ReadGlbMeta(Job.Path, R.Meshes, R.Verts, R.Tris);

            std::lock_guard<std::mutex> Lk(ScanMx);
            ScanDone.push_back(std::move(R));
        }
    };
    std::vector<std::thread> ScanWorkers;
    for (int i = 0; i < 4; i++) ScanWorkers.emplace_back(ScanLoop);

    auto DrainScanResults = [&] {
        std::vector<ScanResult> Batch;
        { std::lock_guard<std::mutex> Lk(ScanMx); if (ScanDone.empty()) return; Batch.swap(ScanDone); }
        HeaderVersion++;
        for (auto& R : Batch)
        {
            auto It = IdToIndex.find(R.Id);
            if (It == IdToIndex.end() || It->second >= Models.size()) continue;
            Item& M = Models[It->second];
            if (M.Path != R.Path) continue; // a copy the row no longer shows (a newer one replaced it)
            M.Bones = R.Bones; M.Meshes = R.Meshes; M.Mats = R.Mats;
            M.ImgW = R.ImgW; M.ImgH = R.ImgH;
            M.Frames = R.Frames; M.FrameRate = R.FrameRate; M.Looping = R.Looping;
            M.Verts = R.Verts; M.Tris = R.Tris;
        }
    };

    // Folder listing: the recursive directory walk runs on a worker (a 60,000-file export, or any
    // folder on a slow or sleeping drive, takes seconds), its entries handed back in batches and
    // merged by the frame (MergeListed: the only writer of Models). A Replace bumps Generation:
    // a walk of the older folder stops and its batches are dropped.
    struct Listing
    {
        std::mutex M;
        std::vector<Item> Ready;              // under M, for Generation
        std::atomic<uint64_t> Generation{ 0 };
        std::atomic<int> Walks{ 0 };
        std::atomic<size_t> Seen{ 0 };
        std::string Dir;                      // under M: the folder walked last, for the status line
    };
    const auto Lists = std::make_shared<Listing>();
    // Lists each file from its directory entry alone (name/kind/size, no open); the header fields
    // are ScanWorker's, queued when the frame merges the entry.
    auto ListFolder = [Lists](const std::string& Dir) {
        const uint64_t Gen = Lists->Generation.load();
        { std::lock_guard<std::mutex> Lk(Lists->M); Lists->Dir = Dir; }
        Lists->Walks++;
        UiAsync::Run("list folder " + Dir, [Lists, Dir, Gen] {
            struct Ended { std::atomic<int>& N; ~Ended() { N--; } } Ending{ Lists->Walks };
            AppLog::Timer Scan("scan folder " + Dir, spdlog::level::info, 1e9);
            size_t Seen = 0, Other = 0, ByKind[5] = {};
            std::vector<Item> Batch;
            auto LastFlush = std::chrono::steady_clock::now();
            const auto Flush = [&] {
                std::lock_guard<std::mutex> Lk(Lists->M);
                if (Lists->Generation.load() == Gen) for (auto& Md : Batch) Lists->Ready.push_back(std::move(Md));
                Batch.clear();
                LastFlush = std::chrono::steady_clock::now();
            };
            std::error_code Ec;
            if (std::filesystem::is_directory(Dir, Ec))
                for (auto It = std::filesystem::recursive_directory_iterator(Dir, Ec); It != std::filesystem::recursive_directory_iterator(); It.increment(Ec))
                {
                    if (Ec || Lists->Generation.load() != Gen) break;
                    if (!It->is_regular_file(Ec)) continue;
                    Seen++;
                    Lists->Seen++;
                    auto P = It->path();
                    std::string PathText, Stem, E;
                    // a name the ANSI code page cannot spell (path::string() throws) is skipped
                    try { PathText = P.string(); Stem = P.stem().string(); E = P.extension().string(); }
                    catch (const std::exception&) { Other++; continue; }
                    for (auto& c:E) c=(char)tolower((unsigned char)c);

                    Item Md; Md.Path = PathText; Md.Name = Stem;
                    // the folder and its first level under it (a game's folder when the output
                    // folder holding several games is listed): two games' assets of one name stay two rows
                    {
                        const size_t From = PathText.find_first_not_of("\\/", std::min(Dir.size(), PathText.size()));
                        const size_t To = From == std::string::npos ? From : PathText.find_first_of("\\/", From);
                        Md.Root = Dir + '|' + (To == std::string::npos ? std::string() : PathText.substr(From, To - From));
                    }
                    Md.SizeBytes = (long long)It->file_size(Ec); // from the entry the iterator already has, no extra stat
                    Md.Written = (long long)It->last_write_time(Ec).time_since_epoch().count(); // so is this
                    Md.Format = E.empty() ? "" : E.substr(1);
                    { std::string Lp = Md.Path; for (auto& c:Lp) c=(char)tolower((unsigned char)c); Md.ModelTexture = (Lp.find("_images") != std::string::npos); }

                    if (E == ".semodel")            { Md.Kind = 0; Md.Format = "semodel"; }
                    else if (E == ".xmodel_export") { Md.Kind = 0; Md.Format = "xmodel_export"; }
                    else if (E == ".png" || E == ".dds" || E == ".tga" || E == ".iwi") Md.Kind = 1;
                    else if (E == ".wav" || E == ".flac" || E == ".opus") Md.Kind = 2;
                    else if (E == ".seanim")        { Md.Kind = 3; Md.Format = "seanim"; }
                    else if (E == ".cast")          // a model or an anim: its first node says which
                    {
                        const CastFile::Kind K = CastFile::FileKind(Md.Path);
                        if (K == CastFile::Kind::None) { Other++; continue; }
                        Md.Kind = (K == CastFile::Kind::Model) ? 0 : 3; Md.Format = "cast";
                    }
                    else if (E == ".glb")           { Md.Kind = 4; Md.Format = "glb"; }
                    else { Other++; continue; }
                    ByKind[Md.Kind]++;
                    Batch.push_back(std::move(Md));
                    if (std::chrono::steady_clock::now() - LastFlush > std::chrono::milliseconds(250)) Flush();
                }
            Flush();
            Scan.Detail(": " + std::to_string(Seen) + " files seen, models " + std::to_string(ByKind[0]) + ", images " +
                        std::to_string(ByKind[1]) + ", audio " + std::to_string(ByKind[2]) + ", anims " + std::to_string(ByKind[3]) +
                        ", maps " + std::to_string(ByKind[4]) + ", other " + std::to_string(Other) + " (on a worker)");
        });
    };
    if (!ModelsDir.empty()) ListFolder(ModelsDir);
    bool SelectFirstModel = true;   // the startup folder's first model is shown once listed

    char Filter[128] = "";
    int Selected = -1;
    bool ScrollToSelected = false; // set on keyboard nav so the table scrolls the new row into view
    std::set<int> MultiSel; // Ctrl+click picks (into Models), independent of Selected - "Merge" target
    int SortCol = 0; bool SortAsc = true;
    // Numeric filters (0 = unbounded on that side).
    int fMinBones=0, fMaxBones=0, fMinMeshes=0, fMaxMeshes=0, fMinMats=0, fMaxMats=0, fMinKB=0, fMaxKB=0;
    GpuMesh Mesh;
    PreviewLoader Loader;
    float Yaw = 0.9f, Pitch = 0.32f, DistScale = 1.0f;
    DirectX::XMFLOAT3 Pan{};
    PreviewRenderer Renderer(Dev.Get());
    PreviewTarget Vrt;   // the 3D view, shown with ImGui::Image
    ComPtr<ID3D11ShaderResourceView> ImageSRV; int ImageW = 0, ImageH = 0;
    // The shown image's zoom, reset to 100% when the shown SRV changes.
    float ImageZoom = 1.0f;
    void* LastZoomedImage = nullptr;
    AudioPlayer Audio; // real playback (decode -> PCM -> device) with cursor + waveform
    int TypeFilter = 0; // 0 all · 1 models · 2 images · 3 audio · 4 anims · 5 maps (driven by the tabs)
    bool HideModelTextures = false;

    // Anim playback state: an anim (Kind==3) previews against a separately-picked rigged model, since
    // an anim file carries no mesh of its own. AnimClip/AnimClipOk are the currently-selected anim;
    // AnimBind/AnimBindOk are the picked model's bind pose (with skeleton + skin weights); Mesh is
    // reused as the animated GPU mesh (rebuilt fresh from AnimBind on a model pick, vertices updated
    // in place every displayed frame via UpdateGpuMeshVertices).
    SeAnimClip AnimClip; bool AnimClipOk = false;
    int AnimModelPick = -1; PreviewGeometry AnimBind; bool AnimBindOk = false;
    bool AnimPlaying = false; float AnimFrame = 0.0f;
    char AnimModelFilter[128] = "";

    // The folders the Assets tab lists: the one given on the command line, then what "Open folder"
    // or "Add folder" picked and each disk export's output.
    std::vector<std::string> ShownFolders;
    if (!ModelsDir.empty()) ShownFolders.push_back(ModelsDir);
    // Sorted by name, the selection, the picks and the anim's model kept on the same files. The
    // first SortedPrefix entries are sorted already: only the rest is, then merged in.
    auto SortModels = [&](size_t SortedPrefix = 0) {
        const auto PathOf = [&](int i) { return i >= 0 && i < (int)Models.size() ? Models[i].Path : std::string(); };
        const std::string Sel = PathOf(Selected), AnimModel = PathOf(AnimModelPick);
        std::set<std::string> Picked;
        for (int i : MultiSel) Picked.insert(PathOf(i));
        const auto ByName = [](const Item& a, const Item& b){ return a.Name < b.Name; };
        SortedPrefix = std::min(SortedPrefix, Models.size());
        std::sort(Models.begin() + SortedPrefix, Models.end(), ByName);
        if (SortedPrefix > 0) std::inplace_merge(Models.begin(), Models.begin() + SortedPrefix, Models.end(), ByName);
        RebuildIdIndex();
        ModelsVersion++;
        Selected = AnimModelPick = -1;
        MultiSel.clear();
        for (int i = 0; i < (int)Models.size(); i++)
        {
            if (!Sel.empty() && Models[i].Path == Sel) Selected = i;
            if (!AnimModel.empty() && Models[i].Path == AnimModel) AnimModelPick = i;
            if (Picked.count(Models[i].Path)) MultiSel.insert(i);
        }
    };
    // Lists Dir's assets instead of the current ones (Replace) or with them.
    auto OpenFolder = [&](const std::string& Dir, bool Replace) {
        if (Replace)
        {
            { std::lock_guard<std::mutex> Lk(Lists->M); Lists->Generation++; Lists->Ready.clear(); Lists->Seen = 0; }
            Models.clear(); KnownPaths.clear(); AssetRows.clear(); ShownFolders.clear(); ModelsVersion++;
            { std::lock_guard<std::mutex> Lk(ScanMx); ScanQueue.clear(); ScanImages.clear(); ScanDone.clear(); }
            IdToIndex.clear();
            Selected = -1; MultiSel.clear(); ScrollToSelected = false;
            AnimClipOk = false; AnimModelPick = -1; AnimBindOk = false; AnimPlaying = false; AnimFrame = 0.0f;
            Loader.Forget(); Mesh = GpuMesh{}; ImageSRV.Reset(); ImageW = ImageH = 0;
            Audio.Unload();
        }
        if (std::find(ShownFolders.begin(), ShownFolders.end(), Dir) == ShownFolders.end()) ShownFolders.push_back(Dir);
        ListFolder(Dir);
    };
    // Once per frame: the listed entries handed back so far join Models (a path already listed is
    // skipped), their header reads queued, the list sorted again.
    auto MergeListed = [&] {
        std::vector<Item> Batch;
        { std::lock_guard<std::mutex> Lk(Lists->M); if (Lists->Ready.empty()) return; Batch.swap(Lists->Ready); }
        AppLog::Timer Merge("merge " + std::to_string(Batch.size()) + " listed assets");
        const size_t Sorted = Models.size();
        std::vector<ScanJob> NewJobs;
        for (auto& Md : Batch)
        {
            if (!KnownPaths.insert(Md.Path).second) continue;
            const auto [Row, Added] = AssetRows.try_emplace(AssetKey(Md), NextItemId);
            if (!Added)
            {
                // another copy of an asset listed already: the row shows the newer one
                const auto It = IdToIndex.find(Row->second);
                if (It == IdToIndex.end() || It->second >= Models.size()) continue;
                Item& Shown = Models[It->second];
                Shown.ModelTexture = Shown.ModelTexture && Md.ModelTexture;
                if (Md.Written < Shown.Written || (Md.Written == Shown.Written && Md.Path >= Shown.Path)) continue;
                Md.Id = Shown.Id; Md.ModelTexture = Shown.ModelTexture;
                Shown = std::move(Md);
                if (Shown.Kind != 2) NewJobs.push_back(ScanJob{ Shown.Id, Shown.Path, Shown.Kind, Shown.Format, Shown.SizeBytes });
                continue;
            }
            Md.Id = NextItemId++;
            if (Md.Kind != 2) NewJobs.push_back(ScanJob{ Md.Id, Md.Path, Md.Kind, Md.Format, Md.SizeBytes });
            IdToIndex[Md.Id] = Models.size();
            Models.push_back(std::move(Md));
        }
        if (!NewJobs.empty())
        {
            std::lock_guard<std::mutex> Lk(ScanMx);
            for (auto& J : NewJobs) (J.Kind == 1 ? ScanImages : ScanQueue).push_back(std::move(J));
            ScanCv.notify_all();
        }
        SortModels(Sorted);
    };

    // Models and maps are built by Loader off the UI thread: a click moves the selection at once,
    // the previous model stays up, dimmed, until the new one is ready (see the Viewport panel).
    auto ResetCamera = [&] { Yaw = 0.9f; Pitch = 0.32f; DistScale = 1.0f; Pan = {}; };
    auto RequestMesh = [&](const std::string& Key, PreviewLoader::Job Build) {
        GpuMesh Cached;
        if (Loader.Request(Key, std::move(Build), Cached)) { Mesh = std::move(Cached); ResetCamera(); }
    };
    auto RequestModels = [&](const std::vector<std::string>& Paths) {
        std::string Key = "models";
        for (const auto& P : Paths) Key += "|" + P;
        ComPtr<ID3D11Device> D = Dev;
        RequestMesh(Key, [D, Paths](const std::function<bool()>& Cancelled) {
            PreviewGeometry Geo;
            return LoadPreviewGeometry(Paths, Geo, Cancelled) ? BuildGpuMesh(D.Get(), Geo) : GpuMesh{};
        });
    };

    auto LoadSelection = [&](int i) {
        static const char* KindNames[] = { "model", "image", "audio", "anim", "map" };
        AppLog::Timer Preview(std::string("preview ") + KindNames[Models[i].Kind % 5] + " " + Models[i].Path, spdlog::level::info, 300);
        Selected = i;
        ImageSRV.Reset(); ImageW = ImageH = 0;
        Audio.Unload(); // stop / release any audio from the previous selection
        AnimClipOk = false; AnimModelPick = -1; AnimBindOk = false; AnimPlaying = false; AnimFrame = 0.0f;
        const Item& Sel = Models[i];
        if (Sel.Kind != 0 && Sel.Kind != 4) { Loader.Forget(); Mesh = GpuMesh{}; }

        if (Sel.Kind == 3) // anim: no mesh of its own - the model to preview it on is picked separately
        {
            AppLog::Timer Read("read anim");
            AnimClipOk = ReadAnimClip(Sel.Path, AnimClip);
        }
        else if (Sel.Kind == 0) // model: assemble parts sharing the base name
        {
            const std::string Base = ExportNaming::CharacterBaseName(Sel.Name);
            std::vector<std::string> Paths;
            for (int k = 0; k < (int)Models.size(); k++)
                if (Models[k].Kind == 0 && ExportNaming::CharacterBaseName(Models[k].Name) == Base)
                {
                    const bool Body = Models[k].Name.find("_body") != std::string::npos || Models[k].Name.find("_torso") != std::string::npos;
                    if (Body) Paths.insert(Paths.begin(), Models[k].Path); else Paths.push_back(Models[k].Path);
                }
            if (Paths.empty()) Paths.push_back(Sel.Path);
            Preview.Detail("(" + std::to_string(Paths.size()) + " parts, queued)");
            RequestModels(Paths);
        }
        else if (Sel.Kind == 1) // image
        {
            ImageSRV = LoadImageSRV(Dev.Get(), Sel.Path, Sel.Format, ImageW, ImageH);
        }
        else if (Sel.Kind == 2) // audio: decode straight to PCM for playback + waveform
        {
            AppLog::Timer Decode("audio decode");
            Audio.Load(Sel.Path, Sel.Format);
        }
        else if (Sel.Kind == 4) // map: one standalone .glb scene, no multi-part assembly
        {
            ComPtr<ID3D11Device> D = Dev;
            const std::string Path = Sel.Path;
            RequestMesh("map|" + Path, [D, Path](const std::function<bool()>&) {
                PreviewGeometry Geo;
                return LoadPreviewGeometryGlb(Path, Geo) ? BuildGpuMesh(D.Get(), Geo) : GpuMesh{};
            });
        }
    };

    // Merges a user-picked set of model parts into one scene (e.g. a weapon body plus its Ctrl+clicked
    // attachments), seated by LoadPreviewGeometry. The first path is the reference frame every other
    // part aligns onto, so the part with the most bones goes first.
    auto LoadMerged = [&](const std::set<int>& Picks) {
        std::vector<int> Idxs(Picks.begin(), Picks.end());
        std::vector<int> ModelIdxs;
        for (int i : Idxs) if (i >= 0 && i < (int)Models.size() && Models[i].Kind == 0) ModelIdxs.push_back(i);
        if (ModelIdxs.empty()) return;
        std::sort(ModelIdxs.begin(), ModelIdxs.end(), [&](int a, int b) { return Models[a].Bones > Models[b].Bones; });

        Selected = ModelIdxs.front();
        ImageSRV.Reset(); ImageW = ImageH = 0; Audio.Unload();
        std::vector<std::string> Paths;
        for (int i : ModelIdxs) Paths.push_back(Models[i].Path);
        RequestModels(Paths);
    };

    // Ctrl+C on the Assets table and the row context menu's "Copy name"/"Copy path" both land
    // here: every MultiSel pick, one per line, or just Selected when nothing is multi-picked.
    auto CopySelection = [&](bool Paths) {
        std::vector<int> Idxs;
        if (!MultiSel.empty()) Idxs.assign(MultiSel.begin(), MultiSel.end());
        else if (Selected >= 0) Idxs.push_back(Selected);
        std::sort(Idxs.begin(), Idxs.end());
        std::string Text;
        for (int i : Idxs)
            if (i >= 0 && i < (int)Models.size()) Text += (Paths ? Models[i].Path : Models[i].Name) + "\n";
        if (!Text.empty()) { Text.pop_back(); ImGui::SetClipboardText(Text.c_str()); }
    };

    // Merges what the folder walks handed back; once the startup folder is listed whole, shows
    // its first model.
    auto PollListing = [&] {
        const bool Walking = Lists->Walks.load() > 0;   // read first: a walk that ends now has flushed
        MergeListed();
        if (!SelectFirstModel || Walking) return;
        SelectFirstModel = false;
        if (Models.empty() || Selected >= 0) return;
        int First = 0;
        for (int k = 0; k < (int)Models.size(); k++) if (Models[k].Kind == 0) { First = k; break; }
        LoadSelection(First);
    };

    // Filter presets (name query + numeric ranges) persisted as JSON under ./filter_presets.
    namespace fs = std::filesystem;
    const fs::path PresetsDir = "filter_presets";
    std::vector<std::string> Presets;
    auto RefreshPresets = [&] {
        Presets.clear();
        std::error_code Ec;
        if (fs::is_directory(PresetsDir, Ec))
            for (auto& E : fs::directory_iterator(PresetsDir, Ec))
                if (E.path().extension() == ".json") Presets.push_back(E.path().stem().string());
        std::sort(Presets.begin(), Presets.end());
    };
    RefreshPresets();
    auto SavePreset = [&](const std::string& Name) {
        std::error_code Ec; fs::create_directories(PresetsDir, Ec);
        nlohmann::json J;
        J["name"] = std::string(Filter);
        J["minBones"] = fMinBones; J["maxBones"] = fMaxBones;
        J["minMeshes"] = fMinMeshes; J["maxMeshes"] = fMaxMeshes;
        J["minMats"] = fMinMats; J["maxMats"] = fMaxMats;
        J["minKB"] = fMinKB; J["maxKB"] = fMaxKB;
        std::ofstream O((PresetsDir / (Name + ".json")).string(), std::ios::binary | std::ios::trunc);
        if (O) O << J.dump(2);
        RefreshPresets();
    };
    auto LoadPreset = [&](const std::string& Name) {
        std::ifstream I((PresetsDir / (Name + ".json")).string(), std::ios::binary);
        if (!I) return;
        nlohmann::json J; try { I >> J; } catch (...) { return; }
        const std::string Nm = J.value("name", std::string());
        std::snprintf(Filter, sizeof(Filter), "%s", Nm.c_str());
        fMinBones = J.value("minBones", 0); fMaxBones = J.value("maxBones", 0);
        fMinMeshes = J.value("minMeshes", 0); fMaxMeshes = J.value("maxMeshes", 0);
        fMinMats = J.value("minMats", 0); fMaxMats = J.value("maxMats", 0);
        fMinKB = J.value("minKB", 0); fMaxKB = J.value("maxKB", 0);
    };

    // Dims the viewport and spins an arc over it while Loader builds the selected model.
    auto DrawLoadingOverlay = [&](ImVec2 Min, ImVec2 Max) {
        ImDrawList* DL = ImGui::GetWindowDrawList();
        DL->AddRectFilled(Min, Max, IM_COL32(12, 12, 14, 150));
        const ImVec2 C((Min.x + Max.x) * 0.5f, (Min.y + Max.y) * 0.5f);
        const float R = ImGui::GetFontSize() * 1.1f, T = (float)ImGui::GetTime() * 6.0f;
        DL->PathArcTo(C, R, T, T + 4.2f, 32);
        DL->PathStroke(IM_COL32(120, 170, 230, 230), 0, 3.0f);
        const char* Text = "loading\xE2\x80\xA6";
        const ImVec2 Ts = ImGui::CalcTextSize(Text);
        DL->AddText(ImVec2(C.x - Ts.x * 0.5f, C.y + R + 8.0f), IM_COL32(220, 220, 225, 255), Text);
    };

    // Viewport screenshot: the next RenderMeshToRT also renders it, through the same renderer and
    // camera, at ShotScale times the view's size on a transparent background, into memory; a worker
    // writes the PNG, named after the model and the time, in the screenshot folder (ScreenshotDir).
    struct ShotRequest { bool Wanted = false; std::string Path, Name; };   // Path: a fixed file (the dev hook)
    ShotRequest PendingShot;
    int ShotScale = std::clamp(std::atoi(SettingsManager::GetSetting("preview_shot_scale", "1").c_str()), 1, 4);
    // the last save, written by its worker
    struct ShotResult { std::mutex M; std::string Status, File; double Until = 0; };
    const auto LastShot = std::make_shared<ShotResult>();
    // The 3D view's lighting controls, kept in the settings (the defaults are the renderer's own).
    PreviewLighting Lighting;
    Lighting.Exposure = std::clamp((float)std::atof(SettingsManager::GetSetting("preview_exposure", "0").c_str()), -4.0f, 4.0f);
    Lighting.Lights = std::clamp((float)std::atof(SettingsManager::GetSetting("preview_lights", "1").c_str()), 0.0f, 3.0f);
    Lighting.Specular = SettingsManager::GetSetting("preview_specular", "true") != "false";

    auto RenderMeshToRT = [&](int W, int H)
    {
        if (!Vrt.Resize(Dev.Get(), W, H)) return;
        Renderer.SetLighting(Lighting);
        const float Bg[4] = { 0.086f, 0.086f, 0.098f, 1.0f };
        const PreviewCamera Cam = OrbitPreviewCamera(Mesh, Yaw, Pitch, DistScale, Pan);
        Renderer.Render(Ctx.Get(), Mesh.Valid ? &Mesh : nullptr, Cam, Vrt, Bg);
        if (!PendingShot.Wanted || !Mesh.Valid) return;
        const ShotRequest Req = std::exchange(PendingShot, ShotRequest{});
        const int SW = W * ShotScale, SH = H * ShotScale;
        const std::string Size = std::to_string(SW) + "x" + std::to_string(SH);
        auto Image = std::make_shared<DirectX::ScratchImage>();
        if (!Renderer.Capture(Dev.Get(), Ctx.Get(), Mesh, Cam, SW, SH, *Image))
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
    };

    // The row above a 3D view: Screenshot (or Ctrl+Shift+S), its scale, and the last save's path.
    // ModelName names the file.
    auto DrawScreenshotBar = [&](std::string ModelName)
    {
        for (char& c : ModelName) if (std::strchr("<>:\"/\\|?*", c) || (unsigned char)c < 32) c = '_';
        const bool Shortcut = !io.WantTextInput && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S);
        if ((ImGui::SmallButton("Screenshot") || Shortcut) && Mesh.Valid) PendingShot = { true, "", ModelName };
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
        int ScaleIdx = ShotScale == 4 ? 2 : ShotScale - 1;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 3.5f);
        if (ImGui::Combo("##shotscale", &ScaleIdx, Scales, 3))
        {
            ShotScale = 1 << ScaleIdx;
            SettingsManager::SetSetting("preview_shot_scale", std::to_string(ShotScale));
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
            std::lock_guard<std::mutex> Lk(LastShot->M);
            if (UiAsync::Detail::Now() < LastShot->Until) { Status = LastShot->Status; File = LastShot->File; }
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
    };

    // Transport + waveform for whatever is loaded in Audio.
    // FormatLabel is only the display string (e.g. "flac").
    auto DrawAudioPlayer = [&](const std::string& FormatLabel)
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
    };

    // The Assets table's rows: Models filtered and sorted, built again only when a filter, the sort
    // or Models changed (that last at most a few times a second while headers stream in).
    std::vector<int> View;
    std::string ViewKey;
    uint64_t ViewVersion = ~0ull, ViewHeaders = ~0ull;
    double ViewBuiltAt = -1e9;

    while (!State.Quit)
    {
        MSG Msg;
        while (PeekMessage(&Msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&Msg); DispatchMessage(&Msg);
            if (Msg.message == WM_QUIT) State.Quit = true;
        }
        if (State.Quit) break;
        AppLog::FrameBegin();
        DrainScanResults();
        PollListing();
        { GpuMesh Ready; double LoadMs = 0; if (Loader.Poll(Ready, LoadMs)) { Mesh = std::move(Ready); ResetCamera(); } }

        if (State.Resized) { BackRTV.Reset(); Swap->ResizeBuffers(0, State.W, State.H, DXGI_FORMAT_UNKNOWN, 0); CreateBackRTV(); State.Resized = false; }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        bool OpenHelp = false;

        if (ImGui::BeginMainMenuBar())
        {
            if (ImGui::BeginMenu("File")) { if (ImGui::MenuItem("Exit")) State.Quit = true; ImGui::EndMenu(); }
            DrawUpdatesMenu();
            DrawBlenderSettingsMenu();
            DrawScreenshotSettingsMenu();
            DrawSoundSettingsMenu();
            if (ImGui::BeginMenu("Help"))
            {
                if (ImGui::MenuItem("Filter syntax…")) OpenHelp = true;
                ImGui::Separator();
                if (ImGui::MenuItem("Open session log")) UiAsync::OpenInShell(AppLog::SessionPath());
                bool Verbose = AppLog::Verbose();
                if (ImGui::MenuItem("Verbose log (every step timed)", nullptr, &Verbose)) AppLog::SetVerbose(Verbose);
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }
        // a newer GrayWolf: Download / Later, then Restart now (the helper replaces the files)
        if (DrawUpdateBanner()) State.Quit = true;

        // Bottom status bar (trims the work area before the dockspace claims the rest). Uses the
        // menu-bar idiom so the text is vertically centred in the frame height and never clipped.
        ImGuiViewport* MainVp = ImGui::GetMainViewport();
        if (ImGui::BeginViewportSideBar("##status", MainVp, ImGuiDir_Down, ImGui::GetFrameHeight(),
                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar))
        {
            if (ImGui::BeginMenuBar())
            {
                ImGui::Text("%d assets", (int)Models.size());
                ImGui::TextDisabled("|");
                if (Selected >= 0 && Selected < (int)Models.size()) ImGui::Text("Selected: %s", Models[Selected].Name.c_str());
                else ImGui::TextDisabled("no selection");
                const char* Hint = "drag: orbit   \xC2\xB7   middle drag: pan   \xC2\xB7   wheel: zoom";
                ImGui::SameLine(std::max(0.0f, ImGui::GetWindowWidth() - ImGui::CalcTextSize(Hint).x - 14.0f));
                ImGui::TextDisabled("%s", Hint);
                ImGui::EndMenuBar();
            }
        }
        ImGui::End();

        const ImGuiID DockId = ImGui::DockSpaceOverViewport();

        // First frame: split the dockspace so Assets fills the left and Viewport the rest, both
        // filling the window (otherwise ImGui leaves them as small floating windows in the corner).
        static bool LayoutBuilt = false;
        if (!LayoutBuilt)
        {
            LayoutBuilt = true;
            ImGui::DockBuilderRemoveNode(DockId);
            ImGui::DockBuilderAddNode(DockId, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(DockId, ImGui::GetMainViewport()->Size);
            ImGuiID Left = 0, Right = 0;
            ImGui::DockBuilderSplitNode(DockId, ImGuiDir_Left, 0.38f, &Left, &Right);
            ImGui::DockBuilderDockWindow("Assets", Left);
            // Tabbed with Assets: browsing exported files and loading a game are the same job seen
            // from two ends, and they never need to be visible at once.
            ImGui::DockBuilderDockWindow("Load Game", Left);
            ImGui::DockBuilderDockWindow("Viewport", Right);
            ImGui::DockBuilderFinish(DockId);
        }

        // opened on a folder (ui <dir>): its assets tab comes first, not Load Game's
        static int FramesShown = 0;
        if (FramesShown++ == 1 && !ModelsDir.empty()) ImGui::SetNextWindowFocus();
        ImGui::Begin("Assets");
        const float Em = ImGui::GetFontSize();
        const float BtnW = ImGui::GetFrameHeight();
        const float Sp   = ImGui::GetStyle().ItemSpacing.x;
        // Any folder of exported assets, in place of the current list or added to it.
        // the dialog runs on its own thread: its answer is collected on the frame it closes
        const std::string StartIn = ShownFolders.empty() ? std::string() : ShownFolders.back();
        if (ImGui::Button("Open folder...")) UiAsync::PickFolder("assets.open", "Select a folder of exported assets", StartIn);
        if (std::string Dir; UiAsync::Picked("assets.open", Dir)) OpenFolder(Dir, true);
        ImGui::SameLine();
        if (ImGui::Button("Add folder...")) UiAsync::PickFolder("assets.add", "Select a folder of exported assets to add", StartIn);
        if (std::string Dir; UiAsync::Picked("assets.add", Dir)) OpenFolder(Dir, false);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keep the assets listed and add this folder's");
        ImGui::SameLine();
        if (Lists->Walks.load() > 0)
        {
            std::string Dir;
            { std::lock_guard<std::mutex> Lk(Lists->M); Dir = Lists->Dir; }
            ImGui::TextDisabled("scanning %s\xE2\x80\xA6  %zu files", Dir.c_str(), Lists->Seen.load());
        }
        else if (ShownFolders.empty()) ImGui::TextDisabled("no folder open");
        else if (ShownFolders.size() == 1) ImGui::TextDisabled("%s", ShownFolders[0].c_str());
        else ImGui::TextDisabled("%s (+%d more)", ShownFolders.back().c_str(), (int)ShownFolders.size() - 1);
        if (ShownFolders.size() > 1 && ImGui::IsItemHovered())
        {
            std::string All;
            for (const auto& F : ShownFolders) All += F + "\n";
            ImGui::SetTooltip("%s", All.c_str());
        }
        const bool  HasText = Filter[0] != '\0';
        // Search box: full-row hint field, an inline clear (x) shown only when there's text, then the help (?).
        float Reserve = BtnW + Sp;                      // "?" button
        if (HasText) Reserve += BtnW + Sp;              // "x" clear button
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - Reserve);
        ImGui::InputTextWithHint("##filter", "Search assets by name\xE2\x80\xA6  (Ctrl+F, Esc clears)",
                                 Filter, sizeof(Filter), ImGuiInputTextFlags_EscapeClearsAll);
        if (HasText)
        {
            ImGui::SameLine();
            if (ImGui::Button("\xC3\x97", ImVec2(BtnW, 0))) Filter[0] = '\0';
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear filter");
        }
        ImGui::SameLine();
        if (ImGui::Button("?", ImVec2(BtnW, 0))) OpenHelp = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Filter syntax help");

        // Type selector — segmented "pills" (cleaner than default tabs). Active = accent fill,
        // inactive = flat + muted, all equal width.
        {
            // Indices 4/5 (Anims/Maps) come after 0-3 to keep TypeFilter's meanings (1=Models, 2=Images, 3=Audio).
            static const char* Segs[] = { "All", "Models", "Images", "Audio", "Anims", "Maps" };
            const ImVec4 AccOn(0.24f, 0.40f, 0.60f, 1.0f), AccOnHov(0.30f, 0.48f, 0.70f, 1.0f);
            const ImVec4 Flat(1,1,1,0.04f), FlatHov(1,1,1,0.10f);
            const ImVec4 Muted = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            const float SegW = Em * 4.4f;
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Em * 0.9f);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, ImGui::GetStyle().ItemSpacing.y));
            for (int i = 0; i < 6; i++)
            {
                const bool On = (TypeFilter == i);
                if (i > 0) ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button,        On ? AccOn    : Flat);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, On ? AccOnHov : FlatHov);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  On ? AccOnHov : FlatHov);
                if (!On) ImGui::PushStyleColor(ImGuiCol_Text, Muted);
                if (ImGui::Button(Segs[i], ImVec2(SegW, 0))) TypeFilter = i;
                ImGui::PopStyleColor(On ? 3 : 4);
            }
            ImGui::PopStyleVar(2);
        }
        if (TypeFilter == 0 || TypeFilter == 2)
            ImGui::Checkbox("Hide model textures (the _images of 3D models)", &HideModelTextures);

        // Numeric filters (min-max ranges; 0 = no limit). Each value is a segmented stepper:
        // [ - ][ number ][ + ] — minus left, the (editable) value in the middle, plus right,
        // joined in one bordered box.
        if (ImGui::CollapsingHeader("Numeric filters", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::TextDisabled("0 = no limit");
            const ImVec4 Cell   = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
            const ImVec4 CellHi(Cell.x + 0.06f, Cell.y + 0.06f, Cell.z + 0.07f, 1.0f);
            auto Stepper = [&](const char* Id, int* V, int Step) {
                const float H = ImGui::GetFrameHeight();
                ImGui::PushID(Id);
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_Button,        Cell);   // -/+ match the value cell -> one unit
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, CellHi);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  CellHi);
                ImGui::PushButtonRepeat(true);
                ImGui::BeginGroup();
                if (ImGui::Button("\xE2\x88\x92", ImVec2(H, H))) { *V -= Step; if (*V < 0) *V = 0; } // U+2212 minus
                ImGui::SameLine();
                ImGui::SetNextItemWidth(Em * 3.2f);
                if (ImGui::InputInt("##n", V, 0, 0) && *V < 0) *V = 0;
                ImGui::SameLine();
                if (ImGui::Button("+", ImVec2(H, H))) *V += Step;
                ImGui::EndGroup();
                ImGui::PopButtonRepeat();
                ImGui::PopStyleColor(3);
                ImGui::PopStyleVar(3);
                ImGui::PopID();
            };
            auto RangeRow = [&](const char* Label, int* Lo, int* Hi, int Step) {
                ImGui::PushID(Label);
                ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(Label);
                ImGui::SameLine(Em * 4.4f);
                Stepper("lo", Lo, Step);
                ImGui::SameLine(0, 7); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("\xE2\x80\x93"); ImGui::SameLine(0, 7);
                Stepper("hi", Hi, Step);
                ImGui::PopID();
            };
            RangeRow("Bones",  &fMinBones,  &fMaxBones,  1);
            RangeRow("Meshes", &fMinMeshes, &fMaxMeshes, 1);
            RangeRow("Mats",   &fMinMats,   &fMaxMats,   1);
            RangeRow("Size KB",&fMinKB,     &fMaxKB,     64);
            ImGui::Spacing();
            if (ImGui::SmallButton("Reset numeric")) { fMinBones=fMaxBones=fMinMeshes=fMaxMeshes=fMinMats=fMaxMats=fMinKB=fMaxKB=0; }
        }

        if (ImGui::CollapsingHeader("Filter presets"))
        {
            static char PresetName[64] = "";
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 70.0f);
            ImGui::InputTextWithHint("##presetname", "preset name", PresetName, sizeof(PresetName));
            ImGui::SameLine();
            if (ImGui::Button("Save", ImVec2(64, 0)) && PresetName[0]) SavePreset(PresetName);
            if (Presets.empty()) ImGui::TextDisabled("no saved presets");
            for (const auto& P : Presets)
            {
                ImGui::PushID(P.c_str());
                if (ImGui::SmallButton("Load")) LoadPreset(P);
                ImGui::SameLine(); ImGui::TextUnformatted(P.c_str());
                ImGui::PopID();
            }
        }

        auto BuildView = [&] {
        const std::string Key = std::to_string(TypeFilter) + "|" + std::to_string(HideModelTextures) + "|" + std::to_string(SortCol) + "|" +
            std::to_string(SortAsc) + "|" + std::to_string(fMinBones) + "," + std::to_string(fMaxBones) + "," + std::to_string(fMinMeshes) + "," +
            std::to_string(fMaxMeshes) + "," + std::to_string(fMinMats) + "," + std::to_string(fMaxMats) + "," + std::to_string(fMinKB) + "," +
            std::to_string(fMaxKB) + "|" + Filter;
        // Models in another order (a merge, a replace) invalidates View's indices: built at once;
        // header fields filled in only move rows: a few times a second
        const bool HeadersMatter = (SortCol >= 2 && SortCol <= 5) || fMinBones || fMaxBones || fMinMeshes || fMaxMeshes || fMinMats || fMaxMats;
        if (Key == ViewKey && ViewVersion == ModelsVersion &&
            (!HeadersMatter || ViewHeaders == HeaderVersion || ImGui::GetTime() - ViewBuiltAt < 0.3)) return;
        AppLog::Timer Build("build the asset view");
        ViewKey = Key;
        ViewVersion = ModelsVersion;
        ViewHeaders = HeaderVersion;
        ViewBuiltAt = ImGui::GetTime();
        View.clear(); View.reserve(Models.size());
        for (int i = 0; i < (int)Models.size(); i++)
        {
            const Item& M = Models[i];
            if (TypeFilter == 1 && M.Kind != 0) continue;
            if (TypeFilter == 2 && M.Kind != 1) continue;
            if (TypeFilter == 3 && M.Kind != 2) continue;
            if (TypeFilter == 4 && M.Kind != 3) continue;
            if (TypeFilter == 5 && M.Kind != 4) continue;
            if (HideModelTextures && M.Kind == 1 && M.ModelTexture) continue;
            if (Filter[0] && !NameMatches(Filter, M.Name)) continue;
            if ((fMinBones  > 0 && (int)M.Bones  < fMinBones)  || (fMaxBones  > 0 && (int)M.Bones  > fMaxBones))  continue;
            if ((fMinMeshes > 0 && (int)M.Meshes < fMinMeshes) || (fMaxMeshes > 0 && (int)M.Meshes > fMaxMeshes)) continue;
            if ((fMinMats   > 0 && (int)M.Mats   < fMinMats)   || (fMaxMats   > 0 && (int)M.Mats   > fMaxMats))   continue;
            { const long long KB = M.SizeBytes / 1024;
              if ((fMinKB > 0 && KB < fMinKB) || (fMaxKB > 0 && KB > fMaxKB)) continue; }
            View.push_back(i);
        }
            // Models is kept sorted by name: that order needs no sort
            if (SortCol == 0) { if (!SortAsc) std::reverse(View.begin(), View.end()); return; }
            std::sort(View.begin(), View.end(), [&](int a, int b) {
                const Item& A = Models[a]; const Item& B = Models[b]; bool lt;
                switch (SortCol) {
                    case 1: lt = A.Kind   < B.Kind;   break;
                    case 2: lt = A.Bones  < B.Bones;  break;
                    case 3: lt = A.Meshes < B.Meshes; break;
                    case 4: lt = A.Mats   < B.Mats;   break;
                    case 5: lt = (long long)A.ImgW * A.ImgH < (long long)B.ImgW * B.ImgH; break;
                    case 6: lt = A.Format < B.Format; break;
                    case 7: lt = A.SizeBytes < B.SizeBytes; break;
                    default: lt = A.Name  < B.Name;   break;
                }
                return SortAsc ? lt : !lt;
            });
        };
        // Built with the last known sort before keyboard nav reads it, so Up/Down walk what is on
        // screen; again once BeginTable has this frame's sort (only if a header was just clicked).
        BuildView();
        ImGui::TextDisabled("%d / %d assets  (click a header to sort)", (int)View.size(), (int)Models.size());
        if (!MultiSel.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("\xC2\xB7  %d picked (Ctrl+click to build a set)", (int)MultiSel.size());
            ImGui::SameLine();
            if (ImGui::SmallButton("Merge")) LoadMerged(MultiSel);
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear picks")) MultiSel.clear();
        }
        ImGui::Separator();

        // Keyboard row navigation: Up/Down walks the filtered/sorted View, not raw Models,
        // so it always lands on what's actually visible. Skipped while a widget (e.g. the
        // filter box) has active input capture, so arrow keys still move a text cursor there.
        if (!View.empty() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive())
        {
            int CurPos = -1;
            for (int k = 0; k < (int)View.size(); k++) if (View[k] == Selected) { CurPos = k; break; }
            bool Moved = false;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) { CurPos = (CurPos < 0) ? 0 : std::min(CurPos + 1, (int)View.size() - 1); Moved = true; }
            else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) { CurPos = (CurPos < 0) ? 0 : std::max(CurPos - 1, 0); Moved = true; }
            if (Moved) { LoadSelection(View[CurPos]); ScrollToSelected = true; }
        }
        // Ctrl+C copies the selection's name(s) - same focus/active-item gate as the nav above, but
        // not gated on View: a multi-pick can still include rows the current type tab is hiding.
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive()
            && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C))
            CopySelection(false);

        // ScrollX + SizingFixedFit: every column keeps its own width, so widening one grows the
        // table (horizontal scrollbar) instead of stealing from its neighbours. No Stretch column
        // here (Stretch + ScrollX don't mix). Name column is pinned while scrolling.
        const ImGuiTableFlags TFlags = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg
            | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit
            | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Hideable | ImGuiTableFlags_Reorderable;
        if (ImGui::BeginTable("assets", 8, TFlags, ImVec2(0.0f, 0.0f), 0.0f))
        {
            ImGui::TableSetupColumn("Name",   ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, Em * 12.0f);
            ImGui::TableSetupColumn("Type",   ImGuiTableColumnFlags_WidthFixed, Em * 3.4f);
            ImGui::TableSetupColumn("Bones",  ImGuiTableColumnFlags_WidthFixed, Em * 3.6f);
            ImGui::TableSetupColumn("Meshes", ImGuiTableColumnFlags_WidthFixed, Em * 3.6f);
            ImGui::TableSetupColumn("Mats",   ImGuiTableColumnFlags_WidthFixed, Em * 3.6f);
            ImGui::TableSetupColumn("Dim",    ImGuiTableColumnFlags_WidthFixed, Em * 6.4f);
            ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, Em * 4.5f);
            ImGui::TableSetupColumn("Size",   ImGuiTableColumnFlags_WidthFixed, Em * 4.5f);
            ImGui::TableSetupScrollFreeze(1, 1);
            // Per-kind columns don't make sense outside their tab: Bones applies to models AND anims
            // (bone count an anim drives), Meshes to models AND maps (mesh count either way), Mats
            // only to models, Dim shows image WxH or map verts/tris. On the mixed "All" tab, leave
            // everything visible.
            ImGui::TableSetColumnEnabled(2, TypeFilter == 0 || TypeFilter == 1 || TypeFilter == 4); // Bones
            ImGui::TableSetColumnEnabled(3, TypeFilter == 0 || TypeFilter == 1 || TypeFilter == 5); // Meshes
            ImGui::TableSetColumnEnabled(4, TypeFilter == 0 || TypeFilter == 1); // Mats
            ImGui::TableSetColumnEnabled(5, TypeFilter == 0 || TypeFilter == 2 || TypeFilter == 5); // Dim
            ImGui::TableHeadersRow();

            if (ImGuiTableSortSpecs* Ss = ImGui::TableGetSortSpecs())
                if (Ss->SpecsCount > 0) { SortCol = Ss->Specs[0].ColumnIndex; SortAsc = (Ss->Specs[0].SortDirection == ImGuiSortDirection_Ascending); }
            BuildView();

            // only the rows on screen are submitted (and the selected one, to scroll it into view)
            ImGuiListClipper Clipper;
            Clipper.Begin((int)View.size());
            if (ScrollToSelected)
                for (int k = 0; k < (int)View.size(); k++)
                    if (View[k] == Selected) { Clipper.IncludeItemByIndex(k); break; }
            while (Clipper.Step())
            for (int Row = Clipper.DisplayStart; Row < Clipper.DisplayEnd; Row++)
            {
                const int idx = View[Row];
                const Item& M = Models[idx];
                const char* Ty = (M.Kind == 0) ? "model" : (M.Kind == 1) ? "image" : (M.Kind == 2) ? "audio" : (M.Kind == 3) ? "anim" : "map";
                ImGui::PushID(idx); // names can repeat (e.g. shared texture stems) - disambiguate the row
                ImGui::TableNextRow();
                if (ScrollToSelected && Selected == idx) { ImGui::SetScrollHereY(); ScrollToSelected = false; }
                ImGui::TableSetColumnIndex(0);
                if (ImGui::Selectable(M.Name.c_str(), Selected == idx || MultiSel.count(idx) != 0, ImGuiSelectableFlags_SpanAllColumns))
                {
                    // Ctrl+click builds a pick set for "Merge" without disturbing the live preview;
                    // a plain click previews normally and starts a fresh pick set.
                    if (ImGui::GetIO().KeyCtrl)
                    {
                        if (MultiSel.count(idx)) MultiSel.erase(idx); else MultiSel.insert(idx);
                    }
                    else
                    {
                        MultiSel.clear();
                        LoadSelection(idx);
                    }
                }
                // Right-click on a row outside the current selection selects it first (Explorer-
                // style), so "Copy name" on the menu always acts on what it looks like it acts on.
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && Selected != idx && !MultiSel.count(idx))
                {
                    MultiSel.clear();
                    LoadSelection(idx);
                }
                if (ImGui::BeginPopupContextItem("row_context"))
                {
                    if (ImGui::MenuItem("Copy name")) CopySelection(false);
                    if (ImGui::MenuItem("Copy path")) CopySelection(true);
                    ImGui::EndPopup();
                }
                ImGui::TableSetColumnIndex(1); ImGui::TextDisabled("%s", Ty);
                ImGui::TableSetColumnIndex(2); if (M.Kind == 0 || M.Kind == 3) ImGui::Text("%u", M.Bones);
                ImGui::TableSetColumnIndex(3); if (M.Kind == 0 || M.Kind == 4) ImGui::Text("%u", M.Meshes);
                ImGui::TableSetColumnIndex(4); if (M.Kind == 0) ImGui::Text("%u", M.Mats);
                ImGui::TableSetColumnIndex(5);
                if (M.Kind == 1 && M.ImgW > 0) ImGui::Text("%dx%d", M.ImgW, M.ImgH);
                else if (M.Kind == 4) ImGui::Text("%uv / %ut", M.Verts, M.Tris);
                ImGui::TableSetColumnIndex(6); ImGui::TextDisabled("%s", M.Format.c_str());
                ImGui::TableSetColumnIndex(7);
                { std::string Sz = FormatSize(M.SizeBytes);
                  float Tw = ImGui::CalcTextSize(Sz.c_str()).x;
                  float Av = ImGui::GetContentRegionAvail().x;
                  if (Av > Tw) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (Av - Tw)); // right-align sizes
                  ImGui::TextUnformatted(Sz.c_str()); }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::End();

        // Viewport panel (dispatches on the selected asset kind)
        ImGui::Begin("Viewport");

        const int SelKind = (Selected >= 0 && Selected < (int)Models.size()) ? Models[Selected].Kind : -1;
        if (SelKind >= 0)
        {
            const Item& S = Models[Selected];
            ImGui::Text("%s", S.Name.c_str());
            if ((S.Kind == 0 || S.Kind == 4) && Loader.Busy())
                ImGui::TextDisabled("loading\xE2\x80\xA6  \xC2\xB7  %s", FormatSize(S.SizeBytes).c_str());
            else if ((S.Kind == 0 || S.Kind == 4) && Mesh.Valid)
                ImGui::TextDisabled("verts %u  \xC2\xB7  tris %u  \xC2\xB7  textures %u  \xC2\xB7  %s    (drag: orbit   middle drag: pan   wheel: zoom)",
                    Mesh.VertexCount, Mesh.TriCount, Mesh.TexCount, FormatSize(S.SizeBytes).c_str());
            else if (S.Kind == 1 && ImageSRV) ImGui::TextDisabled("%d x %d  \xC2\xB7  %s", ImageW, ImageH, FormatSize(S.SizeBytes).c_str());
            else if (S.Kind == 2)             ImGui::TextDisabled("audio  \xC2\xB7  %s", FormatSize(S.SizeBytes).c_str());
            else if (S.Kind == 3)             ImGui::TextDisabled("anim  \xC2\xB7  %s", FormatSize(S.SizeBytes).c_str());
            if (S.Kind == 4) DrawOpenInBlender(S.Path);   // a map export: its .blend, or its import

            // Full field dump for the selection, collapsed by default - the raw numbers behind the
            // one-line summary above, for whichever fields this asset kind actually has.
            if (ImGui::CollapsingHeader("Details"))
            {
                ImGui::TextDisabled("Path");     ImGui::SameLine(Em * 7.0f); ImGui::TextWrapped("%s", S.Path.c_str());
                ImGui::TextDisabled("Format");   ImGui::SameLine(Em * 7.0f); ImGui::Text("%s", S.Format.c_str());
                ImGui::TextDisabled("Size");     ImGui::SameLine(Em * 7.0f); ImGui::Text("%s (%lld bytes)", FormatSize(S.SizeBytes).c_str(), S.SizeBytes);
                if (S.Kind == 0) // model
                {
                    ImGui::TextDisabled("Bones");  ImGui::SameLine(Em * 7.0f); ImGui::Text("%u", S.Bones);
                    ImGui::TextDisabled("Meshes"); ImGui::SameLine(Em * 7.0f); ImGui::Text("%u", S.Meshes);
                    ImGui::TextDisabled("Mats");   ImGui::SameLine(Em * 7.0f); ImGui::Text("%u", S.Mats);
                    if (Mesh.Valid)
                    {
                        ImGui::TextDisabled("GPU verts");    ImGui::SameLine(Em * 7.0f); ImGui::Text("%u", Mesh.VertexCount);
                        ImGui::TextDisabled("GPU tris");     ImGui::SameLine(Em * 7.0f); ImGui::Text("%u", Mesh.TriCount);
                        ImGui::TextDisabled("GPU textures"); ImGui::SameLine(Em * 7.0f); ImGui::Text("%u", Mesh.TexCount);
                    }
                }
                else if (S.Kind == 1) // image
                {
                    ImGui::TextDisabled("Dimensions"); ImGui::SameLine(Em * 7.0f); ImGui::Text("%d x %d", S.ImgW, S.ImgH);
                    ImGui::TextDisabled("Decoded");    ImGui::SameLine(Em * 7.0f); ImGui::TextUnformatted(ImageSRV ? "yes" : "no");
                }
                else if (S.Kind == 2) // audio
                {
                    if (Audio.IsLoaded())
                    {
                        ImGui::TextDisabled("Sample rate"); ImGui::SameLine(Em * 7.0f); ImGui::Text("%u Hz", Audio.SampleRate());
                        ImGui::TextDisabled("Channels");    ImGui::SameLine(Em * 7.0f); ImGui::Text("%d", Audio.Channels());
                        ImGui::TextDisabled("Duration");    ImGui::SameLine(Em * 7.0f); ImGui::Text("%.2fs", Audio.Duration());
                    }
                    else ImGui::TextDisabled("(could not decode)");
                }
                else if (S.Kind == 3) // anim
                {
                    ImGui::TextDisabled("Frames");     ImGui::SameLine(Em * 7.0f); ImGui::Text("%u", S.Frames);
                    ImGui::TextDisabled("Frame rate"); ImGui::SameLine(Em * 7.0f); ImGui::Text("%.2f fps", S.FrameRate);
                    ImGui::TextDisabled("Duration");   ImGui::SameLine(Em * 7.0f); ImGui::Text("%.2fs", S.FrameRate > 0.0f ? (float)S.Frames / S.FrameRate : 0.0f);
                    ImGui::TextDisabled("Bones");       ImGui::SameLine(Em * 7.0f); ImGui::Text("%u", S.Bones);
                    ImGui::TextDisabled("Looping");     ImGui::SameLine(Em * 7.0f); ImGui::TextUnformatted(S.Looping ? "yes" : "no");
                }
                else if (S.Kind == 4) // map
                {
                    ImGui::TextDisabled("Meshes");  ImGui::SameLine(Em * 7.0f); ImGui::Text("%u", S.Meshes);
                    ImGui::TextDisabled("Verts");   ImGui::SameLine(Em * 7.0f); ImGui::Text("%u", S.Verts);
                    ImGui::TextDisabled("Tris");    ImGui::SameLine(Em * 7.0f); ImGui::Text("%u", S.Tris);
                    if (Mesh.Valid)
                    {
                        ImGui::TextDisabled("GPU textures"); ImGui::SameLine(Em * 7.0f); ImGui::Text("%u", Mesh.TexCount);
                    }
                }
            }
            ImGui::Separator();
        }

        if (SelKind == 3) // anim: pick a model to preview it on, then play/pause/scrub
        {
            const Item& A = Models[Selected];
            ImGui::Text("%u frames  \xC2\xB7  %.1f fps  \xC2\xB7  %.2fs  \xC2\xB7  %u bones  \xC2\xB7  %s",
                A.Frames, A.FrameRate, A.FrameRate > 0.0f ? (float)A.Frames / A.FrameRate : 0.0f, A.Bones,
                A.Looping ? "looping" : "one-shot");
            if (!AnimClipOk) ImGui::TextDisabled("could not parse this animation");
            else
            {
                ImGui::Spacing();
                ImGui::TextUnformatted("Preview on model:");
                ImGui::SetNextItemWidth(-1);
                ImGui::InputTextWithHint("##animmodelfilter", "filter by name\xE2\x80\xA6", AnimModelFilter, sizeof(AnimModelFilter));
                if (ImGui::BeginListBox("##animmodels", ImVec2(0, 110)))
                {
                    for (int k = 0; k < (int)Models.size(); k++)
                    {
                        if (Models[k].Kind != 0) continue;
                        if (AnimModelFilter[0] && !NameMatches(AnimModelFilter, Models[k].Name)) continue;
                        const bool IsPicked = (k == AnimModelPick);
                        ImGui::PushID(k);
                        if (ImGui::Selectable(Models[k].Name.c_str(), IsPicked) && !IsPicked)
                        {
                            AnimModelPick = k;
                            AppLog::Timer Bind("anim preview model " + Models[k].Path, spdlog::level::info, 300);
                            PreviewGeometry Geo;
                            AnimBindOk = LoadPreviewGeometry({ Models[k].Path }, Geo);
                            AnimBind = std::move(Geo);
                            Loader.Forget();
                            Mesh = AnimBindOk ? BuildGpuMesh(Dev.Get(), AnimBind) : GpuMesh{};
                            AnimFrame = 0.0f; AnimPlaying = AnimBindOk;
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndListBox();
                }

                if (AnimModelPick >= 0 && !AnimBindOk) ImGui::TextDisabled("could not load that model");
                else if (AnimModelPick >= 0 && AnimBind.Bones.empty()) ImGui::TextDisabled("that model has no skeleton - nothing to animate");
                else if (AnimModelPick >= 0 && AnimBind.VtxBoneIdx.empty()) ImGui::TextDisabled("that model has no skin weights - it will show but won't move");

                if (AnimModelPick >= 0 && AnimBindOk && A.Frames > 0)
                {
                    ImGui::Spacing();
                    if (ImGui::Button(AnimPlaying ? "Pause" : "Play", ImVec2(90, 0))) AnimPlaying = !AnimPlaying;
                    ImGui::SameLine();
                    const float MaxFrame = std::max(0.0f, (float)A.Frames - 1.0f);
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::SliderFloat("##animframe", &AnimFrame, 0.0f, MaxFrame, "frame %.1f")) AnimPlaying = false;

                    if (AnimPlaying && A.FrameRate > 0.0f)
                    {
                        AnimFrame += io.DeltaTime * A.FrameRate;
                        if (AnimFrame > MaxFrame)
                        {
                            if (A.Looping) AnimFrame = std::fmod(AnimFrame, MaxFrame + 1.0f);
                            else { AnimFrame = MaxFrame; AnimPlaying = false; }
                        }
                    }

                    // Re-evaluated every frame (not just while Playing) so scrubbing the slider above
                    // updates the mesh too.
                    std::vector<SkeletonPoseBone> Pose;
                    EvaluateSkeletonPose(AnimBind, AnimClip, AnimFrame, Pose);
                    std::vector<float> SkPos, SkNrm;
                    SkinPreviewGeometry(AnimBind, Pose, SkPos, SkNrm);
                    UpdateGpuMeshVertices(Dev.Get(), Mesh, SkPos, SkNrm, AnimBind.UV);
                }
            }
            ImGui::Separator();
        }

        if (SelKind == 0 || SelKind == 4 || (SelKind == 3 && AnimBindOk)) // 3D model, map, or an animated model - same GPU mesh, same orbit viewport
        {
            DrawScreenshotBar(SelKind == 3 && AnimModelPick >= 0 ? Models[AnimModelPick].Name + "_" + Models[Selected].Name : Models[Selected].Name);
            ImVec2 Avail = ImGui::GetContentRegionAvail();
            const int VW = (int)std::max(16.0f, Avail.x), VH = (int)std::max(16.0f, Avail.y);
            RenderMeshToRT(VW, VH);
            if (Vrt.OutputSRV)
            {
                ImGui::Image((ImTextureID)(intptr_t)Vrt.OutputSRV.Get(), ImVec2((float)VW, (float)VH));
                if (Loader.Busy()) DrawLoadingOverlay(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                if (ImGui::IsItemHovered())
                {
                    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                    {
                        ImVec2 D = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
                        Yaw += D.x * 0.01f; Pitch += D.y * 0.01f;
                        Pitch = std::max(-1.5f, std::min(1.5f, Pitch));
                        ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
                    }
                    if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f))
                    {
                        ImVec2 D = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle, 0.0f);
                        PanPreviewCamera(OrbitPreviewCamera(Mesh, Yaw, Pitch, DistScale, Pan), D.x, D.y, (float)VH, Pan);
                        ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
                    }
                    if (io.MouseWheel != 0.0f)
                    {
                        DistScale *= (io.MouseWheel > 0) ? 0.88f : 1.136f;
                        DistScale = std::max(0.1f, std::min(8.0f, DistScale));
                    }
                }
            }
        }
        else if (SelKind == 1) // image
        {
            if (ImageSRV)
            {
                if (LastZoomedImage != ImageSRV.Get()) { ImageZoom = 1.0f; LastZoomedImage = ImageSRV.Get(); }
                DrawZoomableImage(ImageSRV.Get(), ImageW, ImageH, ImageZoom);
            }
            else ImGui::TextDisabled("could not decode this image");
        }
        else if (SelKind == 2) // audio
        {
            DrawAudioPlayer(Models[Selected].Format);
        }
        else if (SelKind == 3) {} // anim with no model picked yet (or it failed to load) - the picker
                                   // UI above already covers this; nothing more to show here.
        else ImGui::TextUnformatted("Select an asset on the left.");
        ImGui::End();

        // Filter help dialog (opened from the '?' button or the Help menu)
        if (OpenHelp) ImGui::OpenPopup("Filter syntax");
        ImGui::SetNextWindowPos(MainVp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Filter syntax", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted("Filter — matches the model NAME");
            ImGui::Separator();
            ImGui::BulletText("space   =   AND  (every term must appear)");
            ImGui::BulletText("!term   =   the term must NOT appear");
            ImGui::BulletText("case-insensitive");
            ImGui::Spacing(); ImGui::TextDisabled("Examples");
            ImGui::BulletText("richtofen              names containing \"richtofen\"");
            ImGui::BulletText("zombie !head           \"zombie\" and not \"head\"");
            ImGui::BulletText("moon militarypolice    both terms");
            ImGui::Spacing();
            ImGui::TextDisabled("Tip: sort by any column by clicking its header.");
            ImGui::TextDisabled("Numeric filters (bones / meshes / mats / size) are in the toolbar above the table.");
            ImGui::Spacing();
            if (ImGui::Button("Got it", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        LoadGame.Draw(Dev.Get());

        if (LoadGame.ConsumeExportJustFinished())
        {
            // Merge the freshly-exported game into the Assets tab's list; MergeListed dedups by path, so a
            // re-run is safe.
            OpenFolder(LoadGame.ExportOutputDir(), false);
        }

        UiAsync::DrawOverlay();   // a dialog's dimmed layer, the notices
        ImGui::Render();
        const float Clear[4] = { 0.10f, 0.10f, 0.11f, 1.0f };
        ID3D11RenderTargetView* Bb[] = { BackRTV.Get() };
        Ctx->OMSetRenderTargets(1, Bb, nullptr);
        Ctx->ClearRenderTargetView(BackRTV.Get(), Clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // Self-screenshot on demand: when the trigger file exists, save the freshly rendered backbuffer
        // (before Present, since DISCARD invalidates it after) to a PNG - lets a headless caller grab the
        // exact UI without stealing focus. Dev aid. The triggers are looked for by a worker, never by
        // the frame (a probe of a sleeping drive would stall it).
        {
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
                ComPtr<ID3D11Texture2D> Back;
                auto Shot = std::make_shared<ScratchImage>();
                if (SUCCEEDED(Swap->GetBuffer(0, __uuidof(ID3D11Texture2D), &Back)) && SUCCEEDED(CaptureTexture(Dev.Get(), Ctx.Get(), Back.Get(), *Shot)))
                    UiAsync::Run("ui shot", [Shot] {
                        const HRESULT Com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                        SaveToWICFile(*Shot->GetImage(0, 0, 0), WIC_FLAGS_NONE, GetWICCodec(WIC_CODEC_PNG),
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
            else if (ViewportScale != 0 && !PendingShot.Wanted && !Loader.Busy())
            {
                if (ViewportScale > 0) ShotScale = ViewportScale;
                PendingShot = { true, ShotBase + ".viewport.png", "" };
                Consuming = true;
                UiAsync::Run("ui shot trigger", [] {
                    std::error_code Ec;
                    std::filesystem::remove(ShotBase + ".viewport.trigger", Ec);
                    Triggers.Forget();
                    Consuming = false;
                });
            }
        }

        Swap->Present(1, 0);
        AppLog::FrameEnd();
    }

    { std::lock_guard<std::mutex> Lk(ScanMx); ScanStop = true; }
    ScanCv.notify_all();
    for (auto& W : ScanWorkers) W.join();
    Lists->Generation++;   // a folder walk still running stops
    UiAsync::Shutdown();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    DestroyWindow(Hwnd);
    return true;
}
