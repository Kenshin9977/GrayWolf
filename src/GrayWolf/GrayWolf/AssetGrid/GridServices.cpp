#include "stdafx.h"

#include "GridServices.h"
#include "ModelPreview.h"      // DecodeImageFileToRGBA8, SavePreviewPng, ScratchImageToRGBA8
#include "ModelPreviewGPU.h"   // PreviewRenderer, PreviewTarget
#include "CoDIWITranslator.h"  // TranslateIWI (raw CoD images)
#include "CoDAssetType.h"      // XImageDDS

#include "DirectXTex.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>

using Microsoft::WRL::ComPtr;

namespace
{
    // Frames a job may go unwanted before it is dropped (queued) or told it is stale (running).
    constexpr uint64_t StaleFrames = 3;

    int64_t Ticks()
    {
        LARGE_INTEGER T; QueryPerformanceCounter(&T); return T.QuadPart;
    }
    double TicksToMs(int64_t T)
    {
        static const double Freq = [] { LARGE_INTEGER F; QueryPerformanceFrequency(&F); return (double)F.QuadPart; }();
        return (double)T * 1000.0 / Freq;
    }
}

GridJobs::GridJobs(int Count)
{
    for (int i = 0; i < Count; i++) Threads.emplace_back([this, i] { Loop(i == 0); });
}

GridJobs::~GridJobs()
{
    {
        std::lock_guard<std::mutex> Lk(M);
        Stop = true;
        Now = ~0ull >> 1;   // every running job is stale now
    }
    Cv.notify_all();
    for (auto& T : Threads) T.join();
}

void GridJobs::Want(const std::string& Key, int Priority, bool Heavy, const std::function<Work()>& Make)
{
    std::lock_guard<std::mutex> Lk(M);
    if (const auto It = Jobs.find(Key); It != Jobs.end()) { It->second->Stamp = Now.load(); return; }
    auto J = std::make_shared<Job>();
    J->Key = Key; J->Priority = Priority; J->Heavy = Heavy; J->Stamp = Now.load(); J->Fn = Make();
    Jobs.emplace(Key, std::move(J));
    Cv.notify_one();
}

void GridJobs::Always(const std::string& Key, int Priority, Work Fn)
{
    std::lock_guard<std::mutex> Lk(M);
    if (Jobs.count(Key)) return;
    auto J = std::make_shared<Job>();
    J->Key = Key; J->Priority = Priority; J->Sticky = true; J->Stamp = Now.load(); J->Fn = std::move(Fn);
    Jobs.emplace(Key, std::move(J));
    Cv.notify_one();
}

void GridJobs::SetFrame(uint64_t Frame)
{
    Now = Frame;
    Cv.notify_all();   // the idle workers drop what went stale
}

void GridJobs::PostUi(std::function<void()> Fn)
{
    std::lock_guard<std::mutex> Lk(M);
    Done.push_back(std::move(Fn));
}

void GridJobs::DrainUi()
{
    std::vector<std::function<void()>> Batch;
    { std::lock_guard<std::mutex> Lk(M); Batch.swap(Done); }
    for (auto& Fn : Batch) Fn();
}

size_t GridJobs::Pending() const
{
    std::lock_guard<std::mutex> Lk(M);
    return Jobs.size();
}

void GridJobs::Loop(bool FastLane)
{
    const HRESULT Com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // WIC: the PNG cache, the textures
    for (;;)
    {
        std::shared_ptr<Job> J;
        {
            std::unique_lock<std::mutex> Lk(M);
            for (;;)
            {
                if (Stop) break;
                const uint64_t T = Now.load();
                std::shared_ptr<Job> Best;
                for (auto It = Jobs.begin(); It != Jobs.end();)
                {
                    const auto& C = It->second;
                    if (!C->Running && C->Fn && !C->Sticky && C->Stamp + StaleFrames < T) { It = Jobs.erase(It); continue; }   // scrolled past
                    if (!C->Running && C->Fn && (!C->Heavy || HeavyRunning == 0) && (!FastLane || C->Priority == 0) &&
                        (!Best || C->Priority < Best->Priority || (C->Priority == Best->Priority && C->Stamp > Best->Stamp)))
                        Best = C;
                    ++It;
                }
                if (Best) { J = std::move(Best); J->Running = true; if (J->Heavy) HeavyRunning++; break; }
                Cv.wait(Lk);
            }
            if (!J) break;
        }
        const Job* Raw = J.get();
        const StaleFn Stale = [this, Raw] { return !Raw->Sticky && Raw->Stamp.load() + StaleFrames < Now.load(); };
        try { J->Fn(Stale); } catch (...) {}
        {
            std::lock_guard<std::mutex> Lk(M);
            J->Fn = nullptr;
            if (J->Heavy) HeavyRunning--;
            // the key is forgotten once its results ran on the UI thread (posted before this)
            const std::string Key = J->Key;
            Done.push_back([this, Key] { std::lock_guard<std::mutex> L(M); Jobs.erase(Key); });
        }
        Cv.notify_all();
    }
    if (SUCCEEDED(Com)) CoUninitialize();
}

namespace ThumbCache
{
    std::string FileFor(const AssetGridItem& A, const char* Variant)
    {
        static const std::filesystem::path Dir = [] {
            std::error_code Ec;
            const auto Temp = std::filesystem::temp_directory_path(Ec);
            return Ec ? std::filesystem::path() : Temp / "graywolf_thumbs";
        }();
        if (Dir.empty() || !A.Path) return {};
        uint64_t H = 0xcbf29ce484222325ull;
        const auto Mix = [&H](const std::string& S) { for (unsigned char c : S) { H ^= (uint8_t)tolower(c); H *= 0x100000001b3ull; } H ^= '|'; H *= 0x100000001b3ull; };
        Mix(*A.Path); Mix(std::to_string(A.Written)); Mix(std::to_string(A.SizeBytes)); Mix(Variant); Mix("v1");
        char Name[32];
        snprintf(Name, sizeof(Name), "%016llx.png", (unsigned long long)H);
        return (Dir / Name).string();
    }

    bool Load(const std::string& File, int& W, int& H, std::vector<uint8_t>& Rgba)
    {
        std::error_code Ec;
        if (File.empty() || !std::filesystem::exists(File, Ec)) return false;
        return DecodeImageFileToRGBA8(File, W, H, Rgba) && W > 0 && H > 0;
    }

    bool Save(const std::string& File, int W, int H, const std::vector<uint8_t>& Rgba)
    {
        if (File.empty() || W <= 0 || H <= 0 || Rgba.size() != (size_t)W * H * 4) return false;
        DirectX::Image Img{};
        Img.width = (size_t)W; Img.height = (size_t)H; Img.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        Img.rowPitch = (size_t)W * 4; Img.slicePitch = Img.rowPitch * H;
        Img.pixels = const_cast<uint8_t*>(Rgba.data());
        // written beside, then renamed: a reader never sees half a file
        const std::string Part = File + ".part" + std::to_string(GetCurrentThreadId());
        if (!SavePreviewPng(Img, Part)) return false;
        std::error_code Ec;
        std::filesystem::rename(Part, File, Ec);
        if (Ec) std::filesystem::remove(Part, Ec);
        return true;
    }
}

ComPtr<ID3D11ShaderResourceView> MakeRgbaSRV(ID3D11Device* Dev, int W, int H, const std::vector<uint8_t>& Rgba)
{
    if (W <= 0 || H <= 0 || Rgba.size() != (size_t)W * H * 4) return nullptr;
    D3D11_TEXTURE2D_DESC Td{}; Td.Width = W; Td.Height = H; Td.MipLevels = 1; Td.ArraySize = 1;
    Td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; Td.SampleDesc.Count = 1; Td.Usage = D3D11_USAGE_IMMUTABLE; Td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA Sd{}; Sd.pSysMem = Rgba.data(); Sd.SysMemPitch = (UINT)W * 4;
    ComPtr<ID3D11Texture2D> T;
    if (FAILED(Dev->CreateTexture2D(&Td, &Sd, &T))) return nullptr;
    ComPtr<ID3D11ShaderResourceView> S;
    Dev->CreateShaderResourceView(T.Get(), nullptr, &S);
    return S;
}

bool DecodeImageThumb(const std::string& Path, const std::string& Ext, int MaxSize, int& W, int& H, std::vector<uint8_t>& Rgba)
{
    using namespace DirectX;
    TexMetadata Meta{}; ScratchImage Img;
    const std::wstring Wide = std::filesystem::path(Path).wstring();
    HRESULT Hr = E_FAIL;
    if (Ext == "dds") Hr = LoadFromDDSFile(Wide.c_str(), DDS_FLAGS_NONE, &Meta, Img);
    else if (Ext == "tga") Hr = LoadFromTGAFile(Wide.c_str(), &Meta, Img);
    else if (Ext == "iwi")
    {
        std::ifstream In(Wide, std::ios::binary);
        std::vector<uint8_t> Raw((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
        if (Raw.empty()) return false;
        auto Buf = std::make_unique<uint8_t[]>(Raw.size());
        std::memcpy(Buf.get(), Raw.data(), Raw.size());
        auto Dds = CoDIWITranslator::TranslateIWI(Buf, (uint32_t)Raw.size());
        if (!Dds || Dds->DataBuffer == nullptr || Dds->DataSize == 0) return false;
        Hr = LoadFromDDSMemory((const uint8_t*)Dds->DataBuffer, (size_t)Dds->DataSize, DDS_FLAGS_NONE, &Meta, Img);
    }
    else Hr = LoadFromWICFile(Wide.c_str(), WIC_FLAGS_NONE, &Meta, Img);
    if (FAILED(Hr)) return false;

    // the first mip that fits (a DDS carries them): only that one is decompressed
    size_t Mip = 0;
    while (Mip + 1 < Meta.mipLevels && std::max(Meta.width >> Mip, Meta.height >> Mip) > (size_t)MaxSize) Mip++;
    ScratchImage One;
    if (FAILED(One.InitializeFromImage(*Img.GetImage(Mip, 0, 0)))) return false;
    int SW = 0, SH = 0; std::vector<uint8_t> Px;
    if (!ScratchImageToRGBA8(One, One.GetMetadata(), SW, SH, Px)) return false;

    if (std::max(SW, SH) > MaxSize)
    {
        const float K = (float)MaxSize / (float)std::max(SW, SH);
        const size_t DW = std::max<size_t>(1, (size_t)(SW * K)), DH = std::max<size_t>(1, (size_t)(SH * K));
        DirectX::Image Src{}; Src.width = SW; Src.height = SH; Src.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        Src.rowPitch = (size_t)SW * 4; Src.slicePitch = Src.rowPitch * SH; Src.pixels = Px.data();
        ScratchImage Small;
        if (FAILED(Resize(Src, DW, DH, TEX_FILTER_BOX, Small))) return false;
        const DirectX::Image* S = Small.GetImage(0, 0, 0);
        SW = (int)DW; SH = (int)DH;
        std::vector<uint8_t> Out((size_t)SW * SH * 4);
        for (int y = 0; y < SH; y++) std::memcpy(Out.data() + (size_t)y * SW * 4, S->pixels + y * S->rowPitch, (size_t)SW * 4);
        Px.swap(Out);
    }
    for (size_t i = 3; i < Px.size(); i += 4) Px[i] = 255;   // alpha often holds data (gloss, height): shown opaque
    W = SW; H = SH; Rgba = std::move(Px);
    return true;
}

GridServices::GridServices(ID3D11Device* D, ID3D11DeviceContext* C)
    : Dev(D), Ctx(C), Jobs(std::clamp((int)std::thread::hardware_concurrency() / 4, 2, 4)), Target(std::make_unique<PreviewTarget>())
{
}

GridServices::~GridServices() = default;

void GridServices::BeginFrame(uint64_t Frame)
{
    Graveyard.clear();   // retired last frame: ImGui has drawn them
    Jobs.SetFrame(Frame);
    FrameStart = Ticks();
    Renders = 0;
    for (size_t i = 0; i < Backs.size();)
    {
        D3D11_MAPPED_SUBRESOURCE Map{};
        const HRESULT Hr = Ctx->Map(Backs[i].Staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &Map);
        if (Hr == DXGI_ERROR_WAS_STILL_DRAWING) { i++; continue; }
        Back B = std::move(Backs[i]);
        Backs.erase(Backs.begin() + i);
        if (FAILED(Hr)) continue;
        std::vector<uint8_t> Px((size_t)B.W * B.H * 4);
        for (int y = 0; y < B.H; y++) std::memcpy(Px.data() + (size_t)y * B.W * 4, (const uint8_t*)Map.pData + (size_t)y * Map.RowPitch, (size_t)B.W * 4);
        Ctx->Unmap(B.Staging.Get(), 0);
        B.Done(B.W, B.H, std::move(Px));
    }
}

PreviewRenderer* GridServices::Renderer()
{
    if (!RendererTried)
    {
        RendererTried = true;
        R = std::make_unique<PreviewRenderer>(Dev);
        if (!R->Ok()) R.reset();
    }
    return R.get();
}

PreviewTarget* GridServices::Scratch(int Pixels)
{
    return Target->Resize(Dev, Pixels, Pixels) ? Target.get() : nullptr;
}

bool GridServices::CanRender() const
{
    return Renders < MaxRenders && TicksToMs(Ticks() - FrameStart) < BudgetMs;
}

bool GridServices::ReadBack(ID3D11Texture2D* Tex, std::function<void(int, int, std::vector<uint8_t>&&)> Done)
{
    if (!Tex || Backs.size() >= 16) return false;
    D3D11_TEXTURE2D_DESC Td{};
    Tex->GetDesc(&Td);
    if (Td.Format != DXGI_FORMAT_R8G8B8A8_UNORM || Td.SampleDesc.Count != 1) return false;
    Td.Usage = D3D11_USAGE_STAGING; Td.BindFlags = 0; Td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; Td.MiscFlags = 0; Td.MipLevels = 1;
    Back B;
    if (FAILED(Dev->CreateTexture2D(&Td, nullptr, &B.Staging))) return false;
    Ctx->CopySubresourceRegion(B.Staging.Get(), 0, 0, 0, 0, Tex, 0, nullptr);
    B.W = (int)Td.Width; B.H = (int)Td.Height; B.Done = std::move(Done);
    Backs.push_back(std::move(B));
    return true;
}
