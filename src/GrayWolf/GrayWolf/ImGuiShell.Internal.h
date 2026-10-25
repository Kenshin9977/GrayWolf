#pragma once

// The asset browser's pieces, behind RunImGuiShell (ImGuiShell.cpp: the window, the device, the frame
// loop): the listed assets (ImGuiShell.Catalog.cpp), the Assets tab (ImGuiShell.Assets.cpp), the
// Viewport (ImGuiShell.Viewport.cpp; its image and audio views ImGuiShell.Media.cpp, its screenshots
// ImGuiShell.Screenshots.cpp) and the menus, status bar and dock layout (ImGuiShell.Menus.cpp).

#include "ModelPreview.h"      // PreviewGeometry
#include "ModelPreviewGPU.h"   // PreviewGpuMesh, PreviewRenderer, PreviewTarget, PreviewLighting
#include "PreviewLoader.h"     // models built off the UI thread
#include "SeAnimReader.h"      // SeAnimClip
#include "AudioPlayer.h"       // real audio playback (miniaudio) with cursor + waveform
#include "FxPreviewPanel.h"    // an effect (fx JSON) playing in 3D
#include "AssetGrid/AssetGrid.h" // the Assets tab's grid view
#include "AssetGrid/GridShot.h" // ui-grid-shot
#include "AssetGrid/AnimRigs.h" // the model each anim plays on

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <DirectXMath.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include <thread>
#include <unordered_map>
#include <vector>

namespace ShellUi
{
    // GPU resources for one loaded model; copies share them.
    using GpuMesh = PreviewGpuMesh;

    // Kind: 0 model · 1 image · 2 audio · 3 anim · 4 map · 5 fx.
    // Root: the listed folder the file was found under; Written: its last write time (the newer of two
    // copies of one asset is the one shown)
    struct Item { std::string Path, Name, Format, Root; int Kind=0; uint32_t Bones=0, Meshes=0, Mats=0; int ImgW=0, ImgH=0; long long SizeBytes=0; long long Written=0; bool ModelTexture=false; uint32_t Frames=0; float FrameRate=0.0f; bool Looping=false; uint32_t Verts=0, Tris=0; uint64_t Id=0; };

    // The listed assets: the folders' walks, one row per asset, the header reads behind them.
    class AssetCatalog
    {
    public:
        AssetCatalog();    // starts the header readers
        ~AssetCatalog();   // Stop()
        AssetCatalog(const AssetCatalog&) = delete;
        AssetCatalog& operator=(const AssetCatalog&) = delete;

        std::vector<Item> Models;
        // Bumped whenever Models changes order: the Assets table's filtered, sorted view is built again
        // only then (not every frame: 60,000 rows take a while).
        uint64_t ModelsVersion = 0;
        uint64_t HeaderVersion = 0;   // bumped when header fields come in (ScanWorker)
        // The folders the Assets tab lists: the one given on the command line, then what "Open folder"
        // or "Add folder" picked and each disk export's output.
        std::vector<std::string> ShownFolders;

        // Lists each file from its directory entry alone (name/kind/size, no open); the header fields
        // are ScanWorker's, queued when the frame merges the entry.
        void ListFolder(const std::string& Dir);
        // Dir among the shown folders (once), and listed.
        void AddFolder(const std::string& Dir);
        // Forgets every row, the walks under way and the header reads queued (OpenFolder's Replace).
        void Clear();
        bool Walking() const { return Lists->Walks.load() > 0; }
        std::string WalkedDir() const;   // the folder walked last, for the status line
        size_t Seen() const { return Lists->Seen.load(); }

        // Once per UI frame: the header fields read so far written into Models.
        void DrainScanResults();
        // The listed entries handed back so far; false when there are none.
        bool TakeListed(std::vector<Item>& Batch);
        // Batch joins Models (a path already listed is skipped), its header reads queued. Returns
        // how many rows were sorted before (SortByName's SortedPrefix).
        size_t Merge(std::vector<Item>& Batch);
        // Sorted by name; the first SortedPrefix entries are sorted already: only the rest is, then merged in.
        void SortByName(size_t SortedPrefix);
        // The header readers and the folder walks stop (the app is closing).
        void Stop();

    private:
        struct ScanJob { uint64_t Id; std::string Path; int Kind; std::string Format; long long SizeBytes; };
        struct ScanResult { uint64_t Id=0; std::string Path; uint32_t Bones=0, Meshes=0, Mats=0; int ImgW=0, ImgH=0; uint32_t Frames=0; float FrameRate=0.0f; bool Looping=false; uint32_t Verts=0, Tris=0; };
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

        void ScanLoop();
        void RebuildIdIndex();

        std::set<std::string> KnownPaths; // dedup guard for MergeListed: a folder listed again (after a disk export) adds only new files

        // One row per asset: the same kind, format and name under one listed folder (an asset exported
        // twice, into an older layout's zone folder and the current one, or a texture copied beside each
        // model that uses it) is one row, the newest copy's (then the first path). Key -> the row's Id.
        std::unordered_map<std::string, uint64_t> AssetRows;

        // Async header fill: ListFolder's walk lists a file from its directory entry alone;
        // the scan workers (ScanLoop) open each one for header fields and hands results back via ScanDone, applied to
        // Models by DrainScanResults once per UI frame: the only place a header field is written past
        // the fast pass, and only on the UI thread.
        std::mutex ScanMx;
        std::condition_variable ScanCv;
        // Models, anims and maps are read before images: an export holds ten times more images than
        // models, and a model's counts behind 50,000 image headers came minutes later (the Bones / Meshes
        // / Mats columns and filters stood at 0 meanwhile)
        std::deque<ScanJob> ScanQueue, ScanImages;
        std::vector<ScanResult> ScanDone;
        bool ScanStop = false;
        uint64_t NextItemId = 1;
        std::unordered_map<uint64_t, size_t> IdToIndex; // rebuilt after every sort; kept live on push_back
        std::vector<std::thread> ScanWorkers;
        const std::shared_ptr<Listing> Lists = std::make_shared<Listing>();
    };

    // The Assets tab's own state: the filters, the presets, the rows shown, the list or the grid.
    struct AssetsTab
    {
        char Filter[128] = "";
        int SortCol = 0; bool SortAsc = true;
        // Numeric filters (0 = unbounded on that side).
        int fMinBones=0, fMaxBones=0, fMinMeshes=0, fMaxMeshes=0, fMinMats=0, fMaxMats=0, fMinKB=0, fMaxKB=0;
        int TypeFilter = 0; // 0 all · 1 models · 2 images · 3 audio · 4 anims · 5 maps · 6 fx (driven by the tabs)
        bool HideModelTextures = false;
        // The list or the grid (AssetGrid, made on first use); anims' cells play on their rig (ShellState::Rigs),
        // or on GridRig when set (ui-grid-shot --rig).
        bool GridView = false;
        std::unique_ptr<AssetGrid> Grid;
        std::string GridRig;
        // Filter presets (name query + numeric ranges) persisted as JSON under ./filter_presets.
        std::vector<std::string> Presets;
        // The Assets table's rows: Models filtered and sorted, built again only when a filter, the sort
        // or Models changed (that last at most a few times a second while headers stream in).
        std::vector<int> View;
        std::string ViewKey;
        uint64_t ViewVersion = ~0ull, ViewHeaders = ~0ull;
        double ViewBuiltAt = -1e9;
    };

    // Viewport screenshot: the next RenderMeshToRT also renders it, through the same renderer and
    // camera, at ShotScale times the view's size on a transparent background, into memory; a worker
    // writes the PNG, named after the model and the time, in the screenshot folder (ScreenshotDir).
    struct ShotRequest { bool Wanted = false; std::string Path, Name; };   // Path: a fixed file (the dev hook)
    // the last save, written by its worker
    struct ShotResult { std::mutex M; std::string Status, File; double Until = 0; };

    // The Viewport's state: the 3D view, the image, the audio, the anim's playback, the effect.
    struct ViewportState
    {
        ViewportState(ID3D11Device* Dev, ID3D11DeviceContext* Ctx);

        GpuMesh Mesh;
        PreviewLoader Loader;
        float Yaw = 0.9f, Pitch = 0.32f, DistScale = 1.0f;
        DirectX::XMFLOAT3 Pan{};
        PreviewRenderer Renderer;
        PreviewTarget Vrt;   // the 3D view, shown with ImGui::Image
        FxPreviewPanel FxPanel;   // the selected effect's view
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> ImageSRV; int ImageW = 0, ImageH = 0;
        // The shown image's zoom, reset to 100% when the shown SRV changes.
        float ImageZoom = 1.0f;
        void* LastZoomedImage = nullptr;
        AudioPlayer Audio; // real playback (decode -> PCM -> device) with cursor + waveform

        // Anim playback state: an anim (Kind==3) previews against a separately-picked rigged model, since
        // an anim file carries no mesh of its own. AnimClip/AnimClipOk are the currently-selected anim;
        // AnimBind/AnimBindOk are the picked model's bind pose (with skeleton + skin weights); Mesh is
        // reused as the animated GPU mesh (rebuilt fresh from AnimBind on a model pick, vertices updated
        // in place every displayed frame via UpdateGpuMeshVertices).
        SeAnimClip AnimClip; bool AnimClipOk = false;
        int AnimModelPick = -1; PreviewGeometry AnimBind; bool AnimBindOk = false;
        // the rig shown (AnimRigs: picked for the anim, or the user's), its name and score
        std::vector<std::string> AnimRigPaths; std::string AnimRigLabel; float AnimRigScore = 0; bool AnimRigManual = false;
        // the rig being read off the UI thread (ImGuiShell.Viewport.cpp), the one shown kept until it is in
        std::shared_ptr<struct AnimRigLoad> AnimRigLoading;
        // the rig's skinned bones and their bind centroid: the view follows them (an anim's root motion stays in view)
        std::vector<int> AnimSkinBones; float AnimBindCenter[3] = {}; float AnimFirstZ = 0; bool AnimFramed = false;
        bool AnimPlaying = false; float AnimFrame = 0.0f;
        char AnimModelFilter[128] = "";

        ShotRequest PendingShot;
        int ShotScale = 1;
        const std::shared_ptr<ShotResult> LastShot = std::make_shared<ShotResult>();
        // The 3D view's lighting controls, kept in the settings (the defaults are the renderer's own).
        PreviewLighting Lighting;
    };

    // Everything the shell's panels share.
    struct ShellState
    {
        ShellState(ID3D11Device* Dev, ID3D11DeviceContext* Ctx, const std::string& ModelsDir, const GridShotOptions* ShotMode);

        Microsoft::WRL::ComPtr<ID3D11Device> Dev;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> Ctx;
        const std::string ModelsDir;
        const GridShotOptions* const ShotMode;   // ui-grid-shot, else null

        AssetCatalog Catalog;
        bool SelectFirstModel = true;   // the startup folder's first model is shown once listed
        int Selected = -1;
        bool ScrollToSelected = false; // set on keyboard nav so the table scrolls the new row into view
        std::set<int> MultiSel; // Ctrl+click picks (into Models), independent of Selected - "Merge" target
        AssetsTab Assets;
        ViewportState Viewport;
        std::unique_ptr<GridShot> Shot;
        // the model each anim plays on (grid cells and Viewport), given the listed models once a walk ends
        std::shared_ptr<AnimRigs::Index> Rigs = std::make_shared<AnimRigs::Index>();
        uint64_t RigModelsVersion = 0;
    };

    // ImGuiShell.Catalog.cpp
    // Sorted by name, the selection, the picks and the anim's model kept on the same files.
    void SortModels(ShellState& S, size_t SortedPrefix = 0);
    // Lists Dir's assets instead of the current ones (Replace) or with them.
    void OpenFolder(ShellState& S, const std::string& Dir, bool Replace);
    // Merges what the folder walks handed back; once the startup folder is listed whole, shows
    // its first model.
    void PollListing(ShellState& S);

    // ImGuiShell.Assets.cpp
    bool NameMatches(const std::string& Query, const std::string& Name);
    std::string FormatSize(long long Bytes);
    void RefreshPresets(AssetsTab& A);
    // The Assets window; OpenHelp is set by its "?" button.
    void DrawAssetsPanel(ShellState& S, bool& OpenHelp);

    // ImGuiShell.Viewport.cpp
    void ResetCamera(ViewportState& V);
    // The listed models handed to S.Rigs when they changed and no walk is under way.
    void SyncAnimRigs(ShellState& S, bool Walking);
    void LoadSelection(ShellState& S, int i);
    // Merges a user-picked set of model parts into one scene.
    void LoadMerged(ShellState& S, const std::set<int>& Picks);
    // The Viewport window (dispatches on the selected asset kind).
    void DrawViewportPanel(ShellState& S);

    // ImGuiShell.Media.cpp
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> LoadImageSRV(ID3D11Device* Dev, const std::string& Path, const std::string& Ext, int& OutW, int& OutH);
    void DrawZoomableImage(ID3D11ShaderResourceView* SRV, int W, int H, float& Zoom);
    void DrawAudioPlayer(AudioPlayer& Audio, const std::string& FormatLabel);

    // ImGuiShell.Screenshots.cpp
    std::string ScreenshotDir();
    void OpenScreenshotDir();
    void DrawScreenshotSettingsMenu();
    void RenderMeshToRT(ViewportState& V, ID3D11Device* Dev, ID3D11DeviceContext* Ctx, int W, int H);
    void DrawScreenshotBar(ViewportState& V, std::string ModelName);
    // GH_UI_SHOT's triggers, after the frame is drawn and before Present.
    void PollUiShotTriggers(ShellState& S, IDXGISwapChain* Swap);

    // ImGuiShell.Menus.cpp
    // The main menu bar and the update banner; true when the app is to quit.
    bool DrawMainMenuBar(bool& OpenHelp);
    void DrawStatusBar(const ShellState& S);
    void BuildDockLayout(unsigned int DockId);
    void DrawFilterHelp(bool OpenHelp);
}
