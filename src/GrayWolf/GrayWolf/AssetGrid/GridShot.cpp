#include "stdafx.h"

#include "GridShot.h"
#include "ModelPreview.h"   // SavePreviewPng

#include "DirectXTex.h"     // CaptureTexture

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numeric>

#include <mmsystem.h>        // timeBeginPeriod
#pragma comment(lib, "winmm.lib")

using Microsoft::WRL::ComPtr;

namespace
{
    double NowMs()
    {
        static const double Freq = [] { LARGE_INTEGER F; QueryPerformanceFrequency(&F); return (double)F.QuadPart; }();
        LARGE_INTEGER T; QueryPerformanceCounter(&T);
        return (double)T.QuadPart * 1000.0 / Freq;
    }

    std::string Describe(const char* What, std::vector<double> V)
    {
        if (V.empty()) return std::string(What) + ": none\n";
        std::sort(V.begin(), V.end());
        const double Avg = std::accumulate(V.begin(), V.end(), 0.0) / V.size();
        const auto At = [&](double Q) { return V[std::min(V.size() - 1, (size_t)(Q * (V.size() - 1) + 0.5))]; };
        char B[256];
        snprintf(B, sizeof(B), "%s: avg %.2f ms, p50 %.2f, p95 %.2f, max %.2f (%zu frames)\n", What, Avg, At(0.5), At(0.95), V.back(), V.size());
        return B;
    }
}

GridShot::GridShot(const GridShotOptions& Options, ID3D11Device* D, ID3D11DeviceContext* C) : O(Options), Dev(D), Ctx(C)
{
    D3D11_QUERY_DESC Q{};
    Q.Query = D3D11_QUERY_TIMESTAMP_DISJOINT; Dev->CreateQuery(&Q, &Disjoint);
    Q.Query = D3D11_QUERY_TIMESTAMP; Dev->CreateQuery(&Q, &T0); Dev->CreateQuery(&Q, &T1);
    Q.Query = D3D11_QUERY_EVENT; Dev->CreateQuery(&Q, &Fence);
    Start = NowMs();
}

void GridShot::FrameBegin()
{
    FrameStartMs = NowMs();
    if (LastEnd == 0) LastEnd = FrameStartMs;
    if (Measuring_ && Disjoint && T0) { Ctx->Begin(Disjoint.Get()); Ctx->End(T0.Get()); }
}

void GridShot::Pace()
{
    // the rest of a 60 Hz frame: slept, then spun for the last millisecond
    static const bool Fine = timeBeginPeriod(1) == TIMERR_NOERROR;
    (void)Fine;
    const double Until = FrameStartMs + 1000.0 / 60.0;
    while (Until - NowMs() > 1.5) Sleep(1);
    while (NowMs() < Until) {}
}

bool GridShot::FrameEnd(IDXGISwapChain* Swap, bool Ready, bool Settled, const std::string& GridStats)
{
    const double Cpu = NowMs() - FrameStartMs;
    if (!Measuring_)
    {
        const double Waited = (NowMs() - Start) / 1000.0;
        // a few frames settled in a row: the last results drained, the cells drawn from them
        WarmFrames = Ready && Settled ? WarmFrames + 1 : 0;
        if (WarmFrames >= 3 || Waited > O.SettleTimeout)
        {
            TimedOut = WarmFrames < 3;
            SettledAt = Waited;
            Measuring_ = true;
        }
        // the GPU drained before timing starts
        Ctx->End(Fence.Get());
        while (Ctx->GetData(Fence.Get(), nullptr, 0, 0) == S_FALSE) {}
        Pace();
        LastEnd = NowMs();
        return false;
    }

    if (Disjoint && T1) { Ctx->End(T1.Get()); Ctx->End(Disjoint.Get()); }
    Ctx->End(Fence.Get());
    while (Ctx->GetData(Fence.Get(), nullptr, 0, 0) == S_FALSE) {}
    const double End = NowMs();
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT Dj{}; UINT64 A = 0, B = 0;
    if (Disjoint && Ctx->GetData(Disjoint.Get(), &Dj, sizeof(Dj), 0) == S_OK && !Dj.Disjoint &&
        Ctx->GetData(T0.Get(), &A, sizeof(A), 0) == S_OK && Ctx->GetData(T1.Get(), &B, sizeof(B), 0) == S_OK && Dj.Frequency)
        GpuMs.push_back((double)(B - A) * 1000.0 / (double)Dj.Frequency);
    CpuMs.push_back(Cpu);
    WallMs.push_back(End - FrameStartMs);
    if ((int)WallMs.size() < std::max(1, O.Frames)) { Pace(); LastEnd = NowMs(); return false; }

    // the last frame, as drawn
    ComPtr<ID3D11Texture2D> Back;
    DirectX::ScratchImage Shot;
    if (SUCCEEDED(Swap->GetBuffer(0, __uuidof(ID3D11Texture2D), &Back)) && SUCCEEDED(DirectX::CaptureTexture(Dev.Get(), Ctx.Get(), Back.Get(), Shot)))
        Saved = SavePreviewPng(*Shot.GetImage(0, 0, 0), O.OutPng);

    const double Avg = std::accumulate(WallMs.begin(), WallMs.end(), 0.0) / WallMs.size();
    char Head[512];
    snprintf(Head, sizeof(Head), "png=%s\nsize=%dx%d\nview=%s\nsettled_after_s=%.2f%s\nfps_serialized=%.1f\n",
             O.OutPng.c_str(), O.Width, O.Height, O.List ? "list" : "grid", SettledAt, TimedOut ? " (timed out, not settled)" : "", Avg > 0 ? 1000.0 / Avg : 0.0);
    Text = Head;
    Text += Describe("frame (cpu + gpu, gpu waited each frame)", WallMs);
    Text += Describe("cpu", CpuMs);
    Text += Describe("gpu", GpuMs);
    Text += GridStats + "\n";
    std::filesystem::path Txt(O.OutPng);
    Txt.replace_extension(".txt");
    std::ofstream(Txt) << Text;
    return true;
}
