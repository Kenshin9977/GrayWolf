#include "stdafx.h"

// The original 32-bit Modern Warfare 3's live readers (Games/IW4/OriginalLive.h); its materials are
// the rebuild's (Games/IW5/MaterialAdapter.cpp).

#include "Games/IW4/OriginalLive.h"

namespace
{
    const Iw64::OriginalLive Game("IW5x86", "modern_warfare_3", Iw64::Mw3x86);
    const LiveGameRegistration Registered(Game);
}
