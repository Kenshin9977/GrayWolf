#pragma once

#include "AssetGrid.h"

#include "ModelPreviewGPU.h"   // PreviewLighting

#include <functional>
#include <memory>
#include <string>
#include <vector>

// The grid's built-in thumbnails, one renderer per kind (AssetGrid::SetRenderer replaces one).
std::shared_ptr<IAssetCellRenderer> MakeImageCells();   // the image decoded small
std::shared_ptr<IAssetCellRenderer> MakeAudioCells();   // the sound's waveform
std::shared_ptr<IAssetCellRenderer> MakeIconCells();    // the kind's icon (FX until its renderer)

// Models turning live (each visible cell rendered into its own texture, within the frame's budget),
// anims played on the rig model, maps as one rendered still. The stills are kept on disk.
class ModelCellRenderer : public IAssetCellRenderer
{
public:
    ModelCellRenderer();
    ~ModelCellRenderer() override;
    // An anim's rig (AnimRigs.h): its models' paths, false while not known; empty Paths: nothing fits
    using RigResolver = std::function<bool(const std::string& AnimPath, std::vector<std::string>& Paths)>;
    void SetRigResolver(RigResolver Resolve);
    // A model every anim plays on instead (ui-grid-shot --rig), empty for none
    void SetRig(const std::string& ModelPath);
    void SetTurning(bool On);   // false: a model holds its first angle
    // The viewport's lighting controls, so a cell looks like the viewport (a change redraws every cell).
    void SetLighting(const PreviewLighting& L);

    void BeginFrame(AssetCellContext& C) override;
    void Draw(AssetCellContext& C, const AssetGridItem& A, ImDrawList* DL, const ImVec2& Min, const ImVec2& Max, bool Hovered) override;
    void EndFrame(AssetCellContext& C) override;
    bool Settled() const override;
    std::string Stats() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> P;
};
