#include "stdafx.h"

// render-fx <fx json> [--time s] [--frames N --fps F] [--size px] [--seed n] [--forward-up]
// [--fx-images "dir;dir"] [--out png]: the effects preview offscreen, the way the GUI's FX
// viewport draws it (FxPreviewRender.h), on a hardware device or WARP.

#include "CliCommands.h"

#include <CLI/CLI.hpp>
#include "CliOptions.h"
#include "CliSupport.h"
#include "FxPreviewRender.h"
#include "ModelPreview.h"   // SavePreviewPng
#include "WraithX.h"

#include "DirectXTex.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <map>
#include <sstream>

using Microsoft::WRL::ComPtr;

int RunRenderFxCli(const CliOptions& Options)
{
    WraithX::InitializeAPI();   // WIC: the PNG encode, the images' decode
    std::vector<std::string> Extra;
    {
        std::stringstream Ss(Options.FxImages);
        std::string P;
        while (std::getline(Ss, P, ';'))
            if (!P.empty()) Extra.push_back(P);
    }
    FxPreview::SimOptions Sim;
    Sim.Seed = (uint32_t)Options.FxSeed;
    Sim.ForwardUp = Options.FxForwardUp;
    FxPreview::Scene Scene;
    std::string Error;
    if (!Scene.Load(Options.RenderFx, Extra, Error, Sim))
    {
        CliReport(Options, std::format("result=error\nerror={}\n", Error));
        return 5;
    }

    ComPtr<ID3D11Device> Dev;
    ComPtr<ID3D11DeviceContext> Ctx;
    const D3D_FEATURE_LEVEL Want[] = { D3D_FEATURE_LEVEL_11_0 };
    HRESULT Hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, Want, 1, D3D11_SDK_VERSION, &Dev, nullptr, &Ctx);
    if (FAILED(Hr)) Hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, Want, 1, D3D11_SDK_VERSION, &Dev, nullptr, &Ctx);
    if (FAILED(Hr))
    {
        CliReport(Options, "result=error\nerror=no D3D11 device\n");
        return 5;
    }
    FxPreview::Renderer R(Dev.Get());
    FxPreview::Target T;
    const int Size = std::clamp(Options.FxSize, 32, 4096);
    std::string Out = Options.Out.empty() ? Options.RenderFx + ".png" : Options.Out;
    const double StartMs = Options.FxTime >= 0 ? Options.FxTime * 1000.0 : Scene.DurationMsec / 3.0;
    const int Frames = std::max(1, Options.FxFrames);
    const double StepMs = 1000.0 / std::max(0.1, Options.FxFps);
    const std::string Stem = Out.size() > 4 && Out.compare(Out.size() - 4, 4, ".png") == 0 ? Out.substr(0, Out.size() - 4) : Out;

    // the sheet: the frames tiled, at most 6 a row
    const int Cols = std::min(Frames, 6), Rows = (Frames + Cols - 1) / Cols;
    DirectX::ScratchImage Sheet;
    if (Frames > 1 && FAILED(Sheet.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, Size * Cols, Size * Rows, 1, 1))) return 5;
    if (Frames > 1) std::memset(Sheet.GetPixels(), 0, Sheet.GetPixelsSize());

    size_t Particles = 0, Effects = 0;
    bool Ok = true;
    std::map<std::string, int> Types;
    for (int K = 0; K < Frames && Ok; K++)
    {
        const double Ms = StartMs + K * StepMs;
        FxPreview::Frame F;
        Scene.Sim->Evaluate(Ms, F);
        Particles = std::max(Particles, F.Particles.size() + F.Trails.size());
        Effects = std::max(Effects, F.Effects);
        for (const auto& P : F.Particles) Types[P.El ? P.El->TypeName : "?"]++;
        Types["trail"] += (int)F.Trails.size();
        Ok = R.Ok() && FxPreview::RenderAt(Ctx.Get(), R, Scene, Ms, T, Size, Size);
        DirectX::ScratchImage Shot;
        Ok = Ok && FxPreview::CaptureTarget(Dev.Get(), Ctx.Get(), T, Shot);
        if (!Ok) break;
        const std::string File = Frames > 1 ? std::format("{}_{:03}.png", Stem, K) : Out;
        Ok = SavePreviewPng(*Shot.GetImage(0, 0, 0), File);
        if (Frames > 1 && Ok)
        {
            const DirectX::Image* Src = Shot.GetImage(0, 0, 0);
            const DirectX::Image* Dst = Sheet.GetImage(0, 0, 0);
            const int X0 = (K % Cols) * Size, Y0 = (K / Cols) * Size;
            for (int Y = 0; Y < Size; Y++)
                std::memcpy(Dst->pixels + (size_t)(Y0 + Y) * Dst->rowPitch + (size_t)X0 * 4, Src->pixels + (size_t)Y * Src->rowPitch, (size_t)Size * 4);
        }
    }
    if (Ok && Frames > 1) Ok = SavePreviewPng(*Sheet.GetImage(0, 0, 0), Stem + "_sheet.png");

    std::string Drawn;
    for (const auto& [Name, N] : Types)
        if (N) Drawn += (Drawn.empty() ? "" : ",") + Name + ":" + std::to_string(N);
    CliReport(Options, std::format("result={}\nout={}\neffect={}\nelements={}\nduration_ms={:.0f}\ntime_ms={:.0f}\nframes={}\n"
                                   "particles_max={}\neffects_max={}\ndrawn={}\nradius={:.1f}\nimages_indexed={}\ntextures_missing={}\n",
                                   Ok ? "ok" : "error", Out, Scene.Fx->Name, Scene.Fx->Elements.size(), Scene.DurationMsec, StartMs,
                                   Frames, Particles, Effects, Drawn, Scene.Radius, Scene.Textures->Size(), R.TexturesMissing()));
    return Ok ? 0 : 5;
}

// ---- the subcommand (CliCommands.h)

void AddFxPreviewCommands(CliCommandBuilders& B)
{
    B["render-fx"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("render-fx", "Render an exported effect (--type fx JSON) at a time, or a frame sequence, to PNG");
        Sub->add_option("file", Options.RenderFx, "The effect's JSON (<out>/<game>/fx/...json)")->required()->check(CLI::ExistingFile);
        Sub->add_option("--time", Options.FxTime, "Seconds after the effect starts (default: a third of a run)");
        Sub->add_option("--frames", Options.FxFrames, "Render N frames from --time at --fps: <out>_NNN.png and <out>_sheet.png");
        Sub->add_option("--fps", Options.FxFps, "Frames per second of a sequence (10)");
        Sub->add_option("--size", Options.FxSize, "Image size in pixels (512)");
        Sub->add_option("--seed", Options.FxSeed, "The particles' random seed (1)");
        Sub->add_flag("--forward-up", Options.FxForwardUp, "The effect's forward (X) axis up, as an impact on the ground");
        Sub->add_option("--fx-images", Options.FxImages, "More export folders to find images and effects in, ';'-separated");
    };
}
