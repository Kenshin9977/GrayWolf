#include "stdafx.h"

// Modern Warfare 3 on the disk template: the 64-bit rebuild (the iw5 zonekit tables) and the
// original 32-bit build (iw5x86), each zone read as the build its content is (Iw64::IsOriginal),
// by MW2's readers with MW3's layouts (Games/IW4/IwDiskZone.h).

#include "Games/IW4/IwDiskZone.h"

namespace
{
    // the rich presence strings: PRESENCE_MP_SEATOWN, PRESENCE_SP_DUBAI; Survival by its map's
    // SPECIAL_OPS_SURVIVAL_
    const DiskGame::IDiskGame::LevelNaming Levels{ { "code_post_gfx", "common" },
                                                   { { "so_survival_mp_*", { "SPECIAL_OPS_SURVIVAL_{REST}" } },
                                                     { "*", { "PRESENCE_{NAME}", "PRESENCE_SP_{NAME}", "MENU_SP_{NAME}" } } } };
    // the original's scriptfiles: the IW5 VM's 32-bit PC bytecode
    const Iw64::GameInfo Original{ "mw3", "Call of Duty: Modern Warfare 3", "IW5", "iw5x86",
                                   "IW5x86", &Iw64::Mw3x86, Scripts::GscToolBin("iw5", "pc"), &Levels };
    const Iw64::GameInfo Info{ "mw3", "Call of Duty: Modern Warfare 3", "IW5", "iw5",
                               "IW5", &Iw64::Mw3,
                               Scripts::GscToolBin("iw5", "pc64"), &Levels, &Original };
    const Iw64::IwDiskGame Game(Info);
    const DiskGame::Registration Registered(Game);
}
