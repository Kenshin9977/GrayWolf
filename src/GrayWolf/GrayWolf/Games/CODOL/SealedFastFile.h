#pragma once

#include "ZoneMemory.h"

#include "ByteView.h"

#include <cstdint>
#include <string>
#include <vector>

// Call of Duty Online's fast files ("IWff0100", version 461): IW4's authenticated stream, sealed.
// After the 21-byte header, 0x100 bytes RSA-2048 (PKCS#1 v1.5) seal 40 bytes: an AES-192 key and
// a counter. Then 0x2000-byte blocks in AES-192 counter mode (tomcrypt's little-endian counter):
// block 0 is the IWffs100 header (counter from the seal), then groups of one hash block (counter:
// the header's hash g at +0x130 + 32 g) and up to 256 data blocks (counter: the hash block's entry
// k). A counter is the first 16 bytes of its block's SHA-256, checked here. The data blocks are one
// zlib stream: the zone. The game's code: COL_DB_LoadXFile 0x483db0, its block read 0x4834f0.
namespace Codol
{
    bool DecryptFastFile(ByteView File, ZoneBytes& Zone, std::string& Error);
}
