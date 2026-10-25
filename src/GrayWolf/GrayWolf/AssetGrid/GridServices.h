#pragma once

#include "AssetGrid.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class PreviewRenderer;
struct PreviewTarget;

// Keyed background jobs a renderer wants while its cell is on screen: never queued twice, dropped
// (or told it is stale) once not wanted for a few frames, newest first by priority, Heavy ones one
// at a time. Results come back through PostUi; the key stays known until DrainUi ran them.
class GridJobs
{
public:
    using StaleFn = std::function<bool()>;
    using Work = std::function<void(const StaleFn& Stale)>;

    explicit GridJobs(int Threads);
    ~GridJobs();
    GridJobs(const GridJobs&) = delete;
    GridJobs& operator=(const GridJobs&) = delete;

    void Want(const std::string& Key, int Priority, bool Heavy, const std::function<Work()>& Make);
    // Queued until done, wanted or not (a still written to the disk cache).
    void Always(const std::string& Key, int Priority, Work Fn);
    void SetFrame(uint64_t Frame);
    // From a worker: Fn runs on the UI thread at the next DrainUi.
    void PostUi(std::function<void()> Fn);
    void DrainUi();
    // Queued, running or with a result not drained yet.
    size_t Pending() const;

private:
    struct Job
    {
        std::string Key;
        int Priority = 0;
        bool Heavy = false;
        std::atomic<uint64_t> Stamp{ 0 };   // the frame it was last wanted
        bool Running = false, Sticky = false;
        Work Fn;
    };
    void Loop(bool FastLane);   // the fast lane runs only priority 0 (disk stills, thumbnails)

    mutable std::mutex M;
    std::condition_variable Cv;
    std::unordered_map<std::string, std::shared_ptr<Job>> Jobs;
    std::vector<std::function<void()>> Done;
    std::atomic<uint64_t> Now{ 0 };
    int HeavyRunning = 0;
    bool Stop = false;
    std::vector<std::thread> Threads;
};

// Thumbnails on disk: %TEMP%\graywolf_thumbs\<hash of path, write time, size, variant>.png.
namespace ThumbCache
{
    std::string FileFor(const AssetGridItem& A, const char* Variant);
    bool Load(const std::string& File, int& W, int& H, std::vector<uint8_t>& Rgba);
    // Needs COM (WIC) on the calling thread: the grid's workers have it.
    bool Save(const std::string& File, int W, int H, const std::vector<uint8_t>& Rgba);
}

// An immutable RGBA8 texture's view (device only: any thread).
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> MakeRgbaSRV(ID3D11Device* Dev, int W, int H, const std::vector<uint8_t>& Rgba);
// An image file (dds, png, tga, iwi) from its first mip within MaxSize, scaled to fit, made opaque.
bool DecodeImageThumb(const std::string& Path, const std::string& Ext, int MaxSize, int& W, int& H, std::vector<uint8_t>& Rgba);

// What every renderer shares: the device, the jobs, the grid's own 3D renderer and offscreen target,
// the frame's render budget, readbacks for the disk cache, and replaced resources kept until drawn.
class GridServices
{
public:
    GridServices(ID3D11Device* Dev, ID3D11DeviceContext* Ctx);
    ~GridServices();

    ID3D11Device* Dev;
    ID3D11DeviceContext* Ctx;
    GridJobs Jobs;

    void BeginFrame(uint64_t Frame);
    PreviewRenderer* Renderer();               // null when it could not be made
    PreviewTarget* Scratch(int Pixels);        // the 3D render target, Pixels square

    // The frame's 3D render budget: a render may start while this holds (time and count).
    bool CanRender() const;
    void Rendered() { Renders++; }
    double BudgetMs = 7.0;
    int MaxRenders = 160;
    int RendersThisFrame() const { return Renders; }

    // Tex's pixels, copied now and read back once the GPU is done (no stall): Done gets them (RGBA8)
    // on the UI thread at a later BeginFrame. False when too many wait (ask again later).
    bool ReadBack(ID3D11Texture2D* Tex, std::function<void(int W, int H, std::vector<uint8_t>&& Rgba)> Done);
    size_t ReadBacksPending() const { return Backs.size(); }

    void Retire(Microsoft::WRL::ComPtr<IUnknown> Resource) { Graveyard.push_back(std::move(Resource)); }

private:
    struct Back { Microsoft::WRL::ComPtr<ID3D11Texture2D> Staging; int W = 0, H = 0; std::function<void(int, int, std::vector<uint8_t>&&)> Done; };
    std::unique_ptr<PreviewRenderer> R;
    bool RendererTried = false;
    std::unique_ptr<PreviewTarget> Target;
    std::vector<Back> Backs;
    std::vector<Microsoft::WRL::ComPtr<IUnknown>> Graveyard;
    int64_t FrameStart = 0;
    int Renders = 0;
};
