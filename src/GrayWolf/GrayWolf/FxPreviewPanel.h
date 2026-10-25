#pragma once

// The asset browser's view of an effect (an fx kind JSON): it plays in 3D over a ground grid with
// Pause/Play, Restart, a time slider, speed, Loop and a new seed, and orbits like the model
// viewport (drag: orbit, middle drag: pan, wheel: zoom). Everything it draws is FxPreview's
// (FxPreviewRender.h); the shell only hands it the selection and the space to draw in.

#include <d3d11.h>
#include <wrl/client.h>

#include <memory>
#include <string>

class FxPreviewPanel
{
public:
    FxPreviewPanel(ID3D11Device* Dev, ID3D11DeviceContext* Ctx);
    ~FxPreviewPanel();
    FxPreviewPanel(const FxPreviewPanel&) = delete;
    FxPreviewPanel& operator=(const FxPreviewPanel&) = delete;

    // An effect file as the fx kind writes it: a .json under a folder named fx
    static bool IsEffectFile(const std::string& Path);

    // Shows the effect at JsonPath, from its start, playing. False when it does not read (Draw
    // then says why).
    bool Load(const std::string& JsonPath);
    void Unload();
    bool Loaded() const;
    // One line about the effect (elements, length, what it draws), for the viewport's header
    std::string Summary() const;
    // The transport row and the 3D view in the space left, once per frame
    void Draw();

private:
    struct Impl;
    std::unique_ptr<Impl> P;
};
