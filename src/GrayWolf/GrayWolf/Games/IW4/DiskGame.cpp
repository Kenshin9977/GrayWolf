#include "stdafx.h"

// Modern Warfare 2 on the disk template: the 64-bit rebuild (the iw4 zonekit tables) and the
// original 32-bit build (iw4x86), each zone read as the build its content is (Iw64::IsOriginal),
// by the family's readers (Games/IW4/IwDiskZone.h).

#include "Games/IW4/IwDiskZone.h"

namespace
{
    // the campaign by MENU_SP_SP_ (airport "Pas de russe"), Special Ops by SPECIAL_OPS_, the
    // multiplayer maps by MPUI_ (mp_rust "Rust") from the signed localized_code_post_gfx_mp, else
    // by the longname the map list mp/basemaps.arena gives them (mp_brecourt MPUI_WASTELAND)
    const DiskGame::IDiskGame::LevelNaming Levels{ { "code_post_gfx", "common", "code_post_gfx_mp", "localized_code_post_gfx_mp" },
                                                   { { "so_*", { "SPECIAL_OPS_{NAME}" } },
                                                     { "mp_*", { "MPUI_{REST}" } },
                                                     { "*", { "MENU_SP_SP_{NAME}" } } },
                                                   { "mp/basemaps.arena" } };
    const Iw64::GameInfo Original{ "mw2", "Call of Duty: Modern Warfare 2", "IW4", "iw4x86",
                                   "IW4x86", &Iw64::Mw2x86, Scripts::Vm{}, &Levels };
    const Iw64::GameInfo Info{ "mw2", "Call of Duty: Modern Warfare 2", "IW4", "iw4",
                               "IW4", &Iw64::Mw2, Scripts::Vm{}, &Levels, &Original };
    const Iw64::IwDiskGame Game(Info);
    const DiskGame::Registration Registered(Game);
}
