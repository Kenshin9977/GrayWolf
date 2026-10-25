#pragma once

// The Assets grid's FX cells (AssetGrid::SetRenderer(AssetKind::Fx, ...)): each visible effect
// plays in its own cell as the effects preview draws it (FxPreview::RenderAt), from the first time
// it draws something, framed on its particles over its first run, looped. Effects load on the
// grid's workers with their images; cells render within the frame's budget like the model cells;
// effects not seen for a while are let go; a still of each is kept on disk for the next time.

#include "AssetGrid/AssetGrid.h"

#include <memory>

std::shared_ptr<IAssetCellRenderer> MakeFxCells();
