#pragma once

#include <string>

struct ID3D11Device;
struct ID3D11ShaderResourceView;

// The icon of the launcher a game was installed from, as Windows shows it: Steam's and Battle.net's
// own exe icons (their paths from the registry), the Xbox app's as the Start menu draws it (Game
// Pass), the shell's folder icon for anything else. Read
// once per source and kept as a texture; nothing of the logos is stored in this program.
namespace LauncherIcons
{
    // Source: GameInstalls::Install::Source ("Steam", "Battle.net", ...). nullptr when Windows
    // gives no icon.
    ID3D11ShaderResourceView* For(ID3D11Device* Dev, const std::string& Source);
}
