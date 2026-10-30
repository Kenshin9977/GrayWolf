#pragma once

#include <string>

struct ID3D11Device;
struct ID3D11ShaderResourceView;
struct ImDrawList;
struct ImVec2;

// A game as the Library shows it, from the folder its assets were exported under ("coldwar"): its
// title (the disk reader's) and its icon (the exe of an install of it found on this machine; the
// installs are looked for once, on a worker).
namespace GameBadges
{
    // "Call of Duty: Black Ops Cold War"; the id itself when no reader has it
    std::string Title(const std::string& Id);
    // the title without "Call of Duty: " ("Black Ops Cold War"), for a narrow column or a list
    std::string ShortTitle(const std::string& Id);
    // the game's exe icon; nullptr while the installs are looked for, or when none of it is installed
    ID3D11ShaderResourceView* Icon(ID3D11Device* Dev, const std::string& Id);
    // the icon in a Side x Side square at Min, else a rounded badge of the id's first letters
    void Draw(ImDrawList* DL, ID3D11Device* Dev, const std::string& Id, const ImVec2& Min, float Side);
    // the same for one install (GameInstalls::Install): its own exe's icon, else its title's initials
    void DrawInstall(ImDrawList* DL, ID3D11Device* Dev, const std::string& Folder, const std::string& Title, const ImVec2& Min, float Side);
}
