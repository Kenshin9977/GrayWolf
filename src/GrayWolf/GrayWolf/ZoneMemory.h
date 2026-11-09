#pragma once

// A zone's bytes, and the copy-on-write copy a session serves (VirtualGameSession): Wolff's
// ZoneBytes and ZoneMemory::Copy.
#include <wolff/ZoneBytes.h>

#include "ByteView.h"

using wolff::ZoneBytes;
namespace ZoneMemory = wolff::ZoneMemory;
