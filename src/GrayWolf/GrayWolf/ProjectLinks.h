#pragma once

struct ID3D11Device;

// The project's links (GitHub, Discord, Ko-fi) as icons at the right end of the main menu bar.
// Their marks are simple-icons' (CC0), drawn at the bar's height in the text's colour.
namespace ProjectLinks
{
    // Inside BeginMainMenuBar, after the last menu
    void DrawMenuBarIcons(ID3D11Device* Dev);
}
