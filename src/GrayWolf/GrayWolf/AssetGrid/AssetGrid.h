#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct ImDrawList;
struct ImVec2;

// The Assets tab's grid view: the same filtered, sorted entries as the list, one cell each (a square
// thumbnail, the name, the kind), drawn row by row through a clipper so only the cells on screen
// cost anything. What a cell's thumbnail shows is its kind's IAssetCellRenderer: models turning
// live on the GPU, images decoded small, sounds as a waveform, anims on a rig, maps as a still,
// effects playing (FxPreviewCells.h, plugged in by the shell), anything else as an icon.

// The asset kinds of the shell's list (ImGuiShell.cpp's Item::Kind).
namespace AssetKind { enum : int { Model = 0, Image = 1, Audio = 2, Anim = 3, Map = 4, Fx = 5, Count }; }

// One cell's asset, as the shell describes it. Written (the file's last write time) and SizeBytes key
// the thumbnails cached on disk.
struct AssetGridItem
{
    const std::string* Path = nullptr;
    const std::string* Name = nullptr;
    const std::string* Format = nullptr;
    int Kind = 0;
    long long SizeBytes = 0, Written = 0;
    int Id = 0;   // the shell's index: the cell's ImGui id
};

class GridServices;

// What a renderer gets each frame: the device, the services (jobs, disk cache, the 3D renderer), and
// where its thumbnail goes.
struct AssetCellContext
{
    GridServices& Services;
    double Time = 0;          // ImGui's clock, seconds
    uint64_t Frame = 0;
    int ThumbPixels = 0;      // the thumbnail's side in pixels
};

// A kind's thumbnails. The grid calls BeginFrame, then Draw for each visible cell of the kind, then
// EndFrame (still before ImGui renders: a texture drawn into there shows this frame). A renderer
// keeps its own per-asset state, asks the services' jobs for slow work, and frees what has not been
// seen for a while. Resources a cell drew this frame must live until the next BeginFrame.
class IAssetCellRenderer
{
public:
    virtual ~IAssetCellRenderer() = default;
    virtual void BeginFrame(AssetCellContext&) {}
    // Draws A's thumbnail into [Min, Max] (a square). Hovered: the pointer is over the cell.
    virtual void Draw(AssetCellContext& C, const AssetGridItem& A, ImDrawList* DL, const ImVec2& Min, const ImVec2& Max,
                      bool Hovered) = 0;
    virtual void EndFrame(AssetCellContext&) {}
    // Every visible cell of the kind shows its final picture (nothing loading): the grid is settled.
    virtual bool Settled() const { return true; }
    // A line for the frame statistics (ui-grid-shot), empty for none.
    virtual std::string Stats() const { return {}; }
};

// A plain icon for Kind (a vector drawing, no texture): the fallback thumbnail.
void DrawAssetKindIcon(ImDrawList* DL, const ImVec2& Min, const ImVec2& Max, int Kind);
const char* AssetKindName(int Kind);

class AssetGrid
{
public:
    AssetGrid(ID3D11Device* Dev, ID3D11DeviceContext* Ctx);
    ~AssetGrid();
    AssetGrid(const AssetGrid&) = delete;
    AssetGrid& operator=(const AssetGrid&) = delete;

    // Replaces Kind's renderer (the FX preview plugs in here).
    void SetRenderer(int Kind, std::shared_ptr<IAssetCellRenderer> Renderer);

    // Fills the rest of the window with Count cells. Item(i) describes cell i, Selected(i) highlights
    // it, OnItem(i) runs right after cell i's item is submitted (clicks, its context menu: the
    // ImGui "last item" is the cell). ScrollTo >= 0 brings that cell into view.
    void Draw(int Count, const std::function<AssetGridItem(int)>& Item, const std::function<bool(int)>& Selected,
              const std::function<void(int)>& OnItem, int ScrollTo = -1);
    // The cell size slider (the shell puts it on its toolbar).
    void DrawSizeSlider();
    // Cells per row at the last Draw (Up/Down move by this much).
    int Columns() const { return LastColumns; }
    float CellSize() const { return Size; }
    void SetCellSize(float Pixels);
    // Scrolls this many pixels a second (ui-grid-shot's scrolling test).
    void SetAutoScroll(float PixelsPerSecond) { AutoScroll = PixelsPerSecond; }

    // false: model cells hold their first angle, the viewport's (ui-grid-shot --still).
    void SetTurning(bool On);
    // The viewport's exposure, lights and specular switch, which the 3D cells render with.
    void SetLighting(const struct PreviewLighting& L);

    // What an anim's cell plays on: its own rig (Resolve: its models, false while not known, AnimRigs.h),
    // or ModelPath for every anim when set; with neither, anims show an icon.
    void SetAnimRigResolver(std::function<bool(const std::string& AnimPath, std::vector<std::string>& Paths)> Resolve);
    void SetAnimRig(const std::string& ModelPath);

    // Every visible cell shows its final picture and no job is queued.
    bool Settled() const;
    std::string Stats() const;
    int VisibleCells() const { return LastVisible; }

private:
    std::unique_ptr<GridServices> Services;
    std::shared_ptr<IAssetCellRenderer> Renderers[AssetKind::Count];
    std::shared_ptr<class ModelCellRenderer> Models;
    float Size = 128.0f, AutoScroll = 0;
    int LastColumns = 1, LastVisible = 0;
    uint64_t Frame = 0;
};
