#include "stdafx.h"

#include "FxPreviewCells.h"
#include "FxPreviewRender.h"
#include "AssetGrid/GridServices.h"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr double FrameInterval = 1.0 / 30.0;   // a cell's effect is drawn at most this often
    constexpr size_t MaxScenes = 128;              // effects kept loaded, the least recently seen go first
    constexpr size_t MaxCells = 4096;
    constexpr int TextureLimit = 256;              // images start at their first mip within this
    constexpr int TimeSamples = 48;                // times sampled over a run: where it starts, its peak
    constexpr double LoopingPeriodMsec = 60000;    // a looping effect plays on this long, then starts again

    void DrawSpinner(ImDrawList* DL, const ImVec2& Min, const ImVec2& Max, double Time)
    {
        const float R = std::max(4.0f, (Max.x - Min.x) * 0.06f);
        const ImVec2 C(Max.x - R * 2.0f, Max.y - R * 2.0f);
        const float T = (float)Time * 6.0f;
        DL->PathArcTo(C, R, T, T + 4.2f, 16);
        DL->PathStroke(IM_COL32(120, 170, 230, 220), 0, 2.0f);
    }

    // something drawn (a sound, a runner: nothing of its own)
    size_t Drawn(const FxPreview::Frame& F)
    {
        size_t N = F.Trails.size();
        for (const auto& P : F.Particles)
            N += P.Type != FxPreview::ElemType::Sound && P.Type != FxPreview::ElemType::Runner && P.Type != FxPreview::ElemType::Other;
        return N;
    }

    // An effect loaded for a cell, and when it plays: from Begin, for Period, again and again; its
    // still at Peak (the time it draws the most).
    struct Loaded
    {
        std::shared_ptr<FxPreview::Scene> Scene;
        double Begin = 0, Period = 1000, Peak = 0;
        bool Loops = false;
    };
}

class FxCellRenderer : public IAssetCellRenderer
{
public:
    void BeginFrame(AssetCellContext& X) override
    {
        Visible.clear();
        if (!R && !RFailed)
        {
            R = std::make_unique<FxPreview::Renderer>(X.Services.Dev);
            RFailed = !R->Ok();
            R->SetTextureLimit(TextureLimit);
        }
        Evict(X.Services, X.Frame, X.Time);
    }

    void Draw(AssetCellContext& X, const AssetGridItem& A, ImDrawList* DL, const ImVec2& Min, const ImVec2& Max, bool) override
    {
        Cell& C = CellFor(A);
        C.SeenFrame = X.Frame;
        C.LastSeen = X.Time;
        Visible.push_back(&C);
        WantStill(X, C);
        if (!RFailed) WantScene(X, C);
        ID3D11ShaderResourceView* Show = C.LiveValid ? C.LiveSRV.Get() : C.Still.Get();
        if (Show) DL->AddImage((ImTextureID)(intptr_t)Show, Min, Max);
        else DrawAssetKindIcon(DL, Min, Max, A.Kind);
        if (!RFailed && !C.Failed && !C.LiveValid) DrawSpinner(DL, Min, Max, X.Time);
    }

    void EndFrame(AssetCellContext& X) override
    {
        GridServices& S = X.Services;
        const int Pixels = X.ThumbPixels;
        std::vector<Cell*> Order = Visible;
        // the stalest first: over budget, every cell still plays, only less often
        std::sort(Order.begin(), Order.end(), [](const Cell* A, const Cell* B) { return A->LastRender < B->LastRender; });
        bool Settled = true;
        size_t Live = 0;
        for (Cell* C : Order)
        {
            if (C->Fx.Scene && !RFailed)
            {
                if (!C->Still && C->StillLooked && !C->StillSaving && S.CanRender()) RenderStill(S, *C, Pixels);
                if (X.Time - C->LastRender >= FrameInterval && S.CanRender()) RenderLive(S, *C, X.Time, Pixels);
            }
            Settled &= C->LiveValid || C->Failed || RFailed;
            Live += C->LiveValid;
        }
        WasSettled = Settled;
        LastVisible = Order.size();
        LastLive = Live;
    }

    bool Settled() const override { return WasSettled; }

    std::string Stats() const override
    {
        char Ms[32];
        std::snprintf(Ms, sizeof(Ms), "%.3f", Renders ? RenderMs / Renders : 0.0);
        return "fx cells visible " + std::to_string(LastVisible) + ", live " + std::to_string(LastLive) + ", effects loaded " +
               std::to_string(Scenes) + ", cells known " + std::to_string(Cells.size()) + ", fx renders " + std::to_string(Renders) +
               " (" + Ms + " ms each on the CPU: simulation and draw calls)";
    }

private:
    struct Cell
    {
        AssetGridItem Item;
        std::string Path, Name, Format;
        uint64_t SeenFrame = 0;
        double LastSeen = 0;

        ComPtr<ID3D11ShaderResourceView> Still;
        bool StillLooked = false, StillFromDisk = false, StillSaving = false;

        Loaded Fx;
        bool Failed = false;

        ComPtr<ID3D11Texture2D> Live;
        ComPtr<ID3D11ShaderResourceView> LiveSRV;
        int LivePixels = 0;
        bool LiveValid = false;
        double Start = -1, LastRender = -1;
    };

    std::unordered_map<std::string, Cell> Cells;
    std::vector<Cell*> Visible;
    std::unique_ptr<FxPreview::Renderer> R;
    bool RFailed = false, WasSettled = true;
    FxPreview::Target Scratch;
    size_t LastVisible = 0, LastLive = 0, Scenes = 0, Renders = 0;
    double RenderMs = 0;

    // one image index per export (the scenes' own are remade a minute apart: the renderer would
    // forget its images each time two cells' differ)
    std::mutex IndexLock;
    std::map<std::string, std::shared_ptr<const FxPreview::TextureIndex>> Indexes;

    Cell& CellFor(const AssetGridItem& A)
    {
        Cell& C = Cells[*A.Path];
        if (C.Path.empty())
        {
            C.Path = *A.Path; C.Name = A.Name ? *A.Name : std::string(); C.Format = A.Format ? *A.Format : std::string();
            C.Item = A; C.Item.Path = &C.Path; C.Item.Name = &C.Name; C.Item.Format = &C.Format;
        }
        return C;
    }

    Cell* Find(const std::string& Path)
    {
        const auto It = Cells.find(Path);
        return It == Cells.end() ? nullptr : &It->second;
    }

    void WantStill(AssetCellContext& X, Cell& C)
    {
        if (C.StillLooked) return;
        GridServices* S = &X.Services;
        const std::string File = ThumbCache::FileFor(C.Item, "fx"), Path = C.Path;
        S->Jobs.Want("still|" + Path, 0, false, [this, S, File, Path] {
            return [this, S, File, Path](const GridJobs::StaleFn&) {
                int W = 0, H = 0; std::vector<uint8_t> Px;
                ComPtr<ID3D11ShaderResourceView> Srv;
                if (ThumbCache::Load(File, W, H, Px)) Srv = MakeRgbaSRV(S->Dev, W, H, Px);
                S->Jobs.PostUi([this, Path, Srv] {
                    if (Cell* C = Find(Path)) { C->StillLooked = true; C->Still = Srv; C->StillFromDisk = Srv != nullptr; }
                });
            };
        });
    }

    // The effect read, its images and models made, when it starts drawing and for how long.
    void WantScene(AssetCellContext& X, Cell& C)
    {
        if (C.Fx.Scene || C.Failed) return;
        GridServices* S = &X.Services;
        FxPreview::Renderer* Rd = R.get();
        const std::string Path = C.Path;
        S->Jobs.Want("fx|" + Path, 1, false, [this, S, Rd, Path] {
            return [this, S, Rd, Path](const GridJobs::StaleFn& Stale) {
                Loaded L;
                L.Scene = std::make_shared<FxPreview::Scene>();
                std::string Error;
                bool Ok = L.Scene->Load(Path, {}, Error);
                if (Stale()) return;   // scrolled past: asked again when seen
                if (Ok && L.Scene->Textures)
                {
                    std::string Key;
                    for (const auto& F : FxPreview::GameFolders(Path)) Key += F + "|";
                    std::lock_guard<std::mutex> Guard(IndexLock);
                    auto& Kept = Indexes[Key];
                    if (Kept) L.Scene->Textures = Kept; else Kept = L.Scene->Textures;
                }
                if (Ok) Ok = Time(L, *Rd, Stale);
                if (Stale()) return;
                S->Jobs.PostUi([this, Path, L, Ok] {
                    Cell* C = Find(Path);
                    if (!C) return;
                    if (Ok) C->Fx = L; else C->Failed = true;
                });
            };
        });
    }

    // Samples a run: the first time something is drawn (where the cell starts), the time the most
    // is (its still), each sample's images and models read now (not on the UI thread).
    static bool Time(Loaded& L, FxPreview::Renderer& Rd, const GridJobs::StaleFn& Stale)
    {
        FxPreview::Scene& Sc = *L.Scene;
        const double Duration = std::max(1.0, Sc.DurationMsec);
        L.Loops = Sc.Fx->Loops();
        double First = -1, Last = 0;
        size_t Most = 0;
        FxPreview::Frame F;
        for (int K = 0; K <= TimeSamples; K++)
        {
            if (Stale()) return false;
            const double At = Duration * K / TimeSamples;
            Sc.Sim->Evaluate(At, F);
            const size_t N = Drawn(F);
            if (N && First < 0) First = At;
            if (N) Last = At;
            if (N > Most) { Most = N; L.Peak = At; }
            if (Sc.Textures) Rd.Prepare(F, *Sc.Textures);
        }
        if (First < 0) First = 0;
        // a looping effect from where it is whole (its longest life on), a one-shot while it draws
        // something (and a sample more: the last particles fade out)
        L.Begin = L.Loops ? std::max(First, Duration) : First;
        L.Period = L.Loops ? LoopingPeriodMsec : std::max(250.0, std::min(Duration, Last + Duration / TimeSamples) - First);
        if (L.Loops) L.Peak = L.Begin;
        return true;
    }

    bool EnsureLive(GridServices& S, Cell& C, int Pixels)
    {
        if (C.Live && C.LivePixels == Pixels) return true;
        if (C.Live) { S.Retire(C.Live); S.Retire(C.LiveSRV); }
        C.Live.Reset(); C.LiveSRV.Reset(); C.LiveValid = false;
        if (!MakeTexture(S, Pixels, C.Live, C.LiveSRV)) return false;
        C.LivePixels = Pixels;
        return true;
    }

    static bool MakeTexture(GridServices& S, int Pixels, ComPtr<ID3D11Texture2D>& Tex, ComPtr<ID3D11ShaderResourceView>& Srv)
    {
        D3D11_TEXTURE2D_DESC Td{}; Td.Width = Pixels; Td.Height = Pixels; Td.MipLevels = 1; Td.ArraySize = 1;
        Td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; Td.SampleDesc.Count = 1; Td.Usage = D3D11_USAGE_DEFAULT; Td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (SUCCEEDED(S.Dev->CreateTexture2D(&Td, nullptr, &Tex)) && SUCCEEDED(S.Dev->CreateShaderResourceView(Tex.Get(), nullptr, &Srv))) return true;
        Tex.Reset(); Srv.Reset();
        return false;
    }

    // The effect at Msec through the scratch target, copied into Into.
    bool RenderInto(GridServices& S, Cell& C, double Msec, int Pixels, ID3D11Texture2D* Into)
    {
        FxPreview::DrawOptions O;
        O.Grid = false;
        const auto T0 = std::chrono::steady_clock::now();
        const bool Ok = FxPreview::RenderAt(S.Ctx, *R, *C.Fx.Scene, Msec, Scratch, Pixels, Pixels, O);
        RenderMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count();
        Renders++;
        S.Rendered();
        if (!Ok || !Scratch.Output) return false;
        S.Ctx->CopyResource(Into, Scratch.Output.Get());
        return true;
    }

    void RenderLive(GridServices& S, Cell& C, double Now, int Pixels)
    {
        if (!EnsureLive(S, C, Pixels)) return;
        if (C.Start < 0) C.Start = Now;
        const double Msec = C.Fx.Begin + std::fmod((Now - C.Start) * 1000.0, C.Fx.Period);
        if (!RenderInto(S, C, Msec, Pixels, C.Live.Get())) return;
        C.LiveValid = true;
        C.LastRender = Now;
    }

    // The effect at its peak, kept as the cell's still: in memory, and on disk for the next time.
    void RenderStill(GridServices& S, Cell& C, int Pixels)
    {
        ComPtr<ID3D11Texture2D> Tex;
        ComPtr<ID3D11ShaderResourceView> Srv;
        if (!MakeTexture(S, Pixels, Tex, Srv) || !RenderInto(S, C, C.Fx.Peak, Pixels, Tex.Get())) return;
        C.Still = Srv;
        const std::string File = ThumbCache::FileFor(C.Item, "fx");
        GridServices* SP = &S;
        // shown from memory now; on disk once read back (too many waiting: not saved this time)
        S.ReadBack(Tex.Get(), [SP, File](int W, int H, std::vector<uint8_t>&& Px) {
            auto Shared = std::make_shared<std::vector<uint8_t>>(std::move(Px));
            SP->Jobs.Always("save|" + File, 0, [File, W, H, Shared](const GridJobs::StaleFn&) { ThumbCache::Save(File, W, H, *Shared); });
        });
        C.StillSaving = true;
    }

    void Evict(GridServices& S, uint64_t Frame, double Now)
    {
        std::vector<Cell*> WithScene;
        size_t Loaded = 0;
        for (auto& [Key, C] : Cells)
        {
            const bool Gone = C.SeenFrame + 1 < Frame;   // not drawn last frame: nothing of it is in flight
            if (Gone && Now - C.LastSeen > 1.5 && C.Live)
            {
                S.Retire(C.Live); S.Retire(C.LiveSRV);
                C.Live.Reset(); C.LiveSRV.Reset(); C.LivePixels = 0; C.LiveValid = false; C.Start = -1;
            }
            if (C.Fx.Scene)
            {
                Loaded++;
                if (Gone) WithScene.push_back(&C);
            }
        }
        bool Freed = false;
        if (Loaded > MaxScenes)
        {
            std::sort(WithScene.begin(), WithScene.end(), [](const Cell* A, const Cell* B) { return A->LastSeen < B->LastSeen; });
            for (Cell* C : WithScene)
            {
                if (Loaded <= MaxScenes) break;
                C->Fx = {}; C->LiveValid = false; C->Start = -1;
                Loaded--;
                Freed = true;
            }
        }
        if (Cells.size() > MaxCells)
        {
            std::vector<std::pair<double, std::string>> Old;
            for (const auto& [Key, C] : Cells) if (C.SeenFrame + 1 < Frame) Old.emplace_back(C.LastSeen, Key);
            std::sort(Old.begin(), Old.end());
            for (size_t i = 0; i < Old.size() && Cells.size() > MaxCells * 3 / 4; i++)
            {
                Freed |= Cells[Old[i].second].Fx.Scene != nullptr;
                Cells.erase(Old[i].second);
            }
        }
        // the renderer kept which image each material of the freed effects drew with, by address
        if (Freed && R) R->ForgetMaterials();
        Scenes = 0;
        for (const auto& [Key, C] : Cells) Scenes += C.Fx.Scene != nullptr;
    }
};

std::shared_ptr<IAssetCellRenderer> MakeFxCells() { return std::make_shared<FxCellRenderer>(); }
