#pragma once

#include <string>

struct GridShotOptions;

// Opens the modern Dear ImGui + Direct3D 11 shell: a dockable asset browser (filter + list) beside a
// 3D viewport, DPI-crisp and MFC-free. ModelsDir is scanned recursively for .cast/.semodel files. Blocks
// until the window is closed. Returns false if the window/device can't be created.
// Requires WraithX::InitializeAPI first (WIC, for texture decode).
// ShotMode (ui-grid-shot): hidden, timed, the last frame saved, then closed.
bool RunImGuiShell(const std::string& ModelsDir, const GridShotOptions* ShotMode = nullptr);
