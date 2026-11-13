#include "stdafx.h"

// The original 32-bit Modern Warfare 2's live readers (Games/IW4/OriginalLive.h); its materials are
// the rebuild's (Games/IW4/MaterialAdapter.cpp).

#include "Games/IW4/OriginalLive.h"

namespace
{
    const Iw64::OriginalLive Game("IW4x86", "modern_warfare_2", Iw64::Mw2x86);
    const LiveGameRegistration Registered(Game);
}
