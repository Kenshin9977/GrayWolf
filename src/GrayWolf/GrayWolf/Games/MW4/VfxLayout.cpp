#include "stdafx.h"

#include "VfxLayout.h"

#include "Games/BO7/VfxLayout.h"

#include <cstring>

// Modern Warfare 4's particle system: Black Ops 7's (Games/BO7/VfxLayout.cpp). Its exe's module name
// table (mw4_flat 0xc460980, char*[81]) is Black Ops 7's, name for name in the same order; the module
// descriptors after it (0x20 each, the data's size at +0x18) give Black Ops 7's sizes but for two:
// InitDecal 0x40 (0x30) and InitFXCompute 0x70 (0x60), whose data the mw4 tables read as such.
namespace MW4
{
const Vfx::Layout& VfxLayout()
{
    static const Vfx::Layout L = [] {
        Vfx::Layout V = BO7::VfxLayout();
        V.Game = "mw4";
        for (Vfx::Module& M : V.Modules)
        {
            // the decal's material list {pointer, i32 count} at 0x20 (Black Ops 7: 0x10; table 0x2089320)
            if (std::strcmp(M.Name, "InitDecal") == 0) M.Size = 0x40, M.AssetList = 0x20;
            else if (std::strcmp(M.Name, "InitFXCompute") == 0) M.Size = 0x70;
        }
        return V;
    }();
    return L;
}
}
