#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <string>
#include <vector>

// ui-grid-shot: the shell opened offscreen on a folder, its Assets tab in the grid (or the list),
// rendered until every visible cell shows its picture, then Frames frames timed and the last one
// saved as a PNG beside a text file of the numbers.
struct GridShotOptions
{
    std::string OutPng;
    int Frames = 120;
    int Width = 1920, Height = 1080;
    float Cell = 0;              // cell size in pixels (0: the setting)
    int Type = 1;                // the type tab: 0 all, 1 models, 2 images, 3 audio, 4 anims, 5 maps, 6 fx
    bool List = false;           // the list instead of the grid
    std::string Search;          // the search box's text (--name-filter)
    std::string AnimRig;         // the model anims play on
    float Scroll = 0;            // pixels a second the grid scrolls while timed (loading while scrolling)
    bool Still = false;          // model cells hold their first angle (the viewport's) instead of turning
    double SettleTimeout = 120;  // seconds to wait for the cells before timing anyway
    std::vector<std::string> Select;   // assets selected one after the other by name (the viewport's own state carried over), the last kept
};

// Times frames honestly: each frame waits for the GPU at its end (CPU and GPU then add up), with
// GPU timestamps for the GPU's share; frames are then paced to 60 Hz, as a screen would.
class GridShot
{
public:
    GridShot(const GridShotOptions& Options, ID3D11Device* Dev, ID3D11DeviceContext* Ctx);
    void FrameBegin();
    // After the frame is drawn, before Present. Ready: the folder is listed; Settled: the grid's
    // cells are all drawn. True once the PNG and the numbers are written: quit.
    bool FrameEnd(IDXGISwapChain* Swap, bool Ready, bool Settled, const std::string& GridStats);
    bool Ok() const { return Saved; }
    bool Measuring() const { return Measuring_; }
    std::string Summary() const { return Text; }

private:
    void Pace();
    GridShotOptions O;
    Microsoft::WRL::ComPtr<ID3D11Device> Dev;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> Ctx;
    Microsoft::WRL::ComPtr<ID3D11Query> Disjoint, T0, T1, Fence;
    double Start = 0, FrameStartMs = 0, LastEnd = 0, SettledAt = -1;
    bool Measuring_ = false, TimedOut = false, Saved = false;
    int WarmFrames = 0;
    std::vector<double> CpuMs, GpuMs, WallMs;
    std::string Text;
};
