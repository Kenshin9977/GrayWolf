#pragma once

#include "ZoneMemory.h"

#include "ByteView.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class XPAKCache;

// Reads a Black Ops 3 (T7) fastfile's images OFFLINE and writes them as PNG, since OAT's
// in-process zone loader stops at T6 and BO3 pixels live in zone/*.xpak under an opaque hash
// keyed with no name->hash algorithm.
struct Bo3ZoneImage
{
    std::string Name;
    uint64_t Hash = 0;      // the XPAK key of the largest mip that the packages actually carry
    uint32_t Width = 0;
    uint32_t Height = 0;
    uint32_t Size = 0;      // the mip's size as the zone declares it
    uint8_t Format = 0;     // the engine's image-format byte, at +240 in the struct
    uint8_t Levels = 1;     // how many mip levels the inline payload holds
    uint64_t PixelOffset = 0; // non-zero when the pixels are INLINE in the zone, at this offset
    int64_t Pos = -1;         // its instance in a walked zone (T7Zone), -1 from a scan
};

// Finds every image in the fastfile: streamed ones (pixels in the packages under XpakDir) AND
// ones carrying pixels inline right after the name string (a game's 2D interface - cursors,
// badges, HUD); ZoneOut exists so the caller has the buffer to read inline pixels
// out of. Pass an empty XpakDir to look beside the fastfile. Returns them in file order.
std::vector<Bo3ZoneImage> Bo3ScanZoneImages(const std::string& FastFilePath,
                                            const std::string& XpakDir,
                                            std::string& FailureReason,
                                            ZoneBytes* ZoneOut = nullptr);

// Lists what the zone declares, one "<name>	<width>x<height>	format <n>	size <n>" per line.
bool Bo3ListZoneImages(const std::string& FastFilePath, const std::string& XpakDir,
                       std::string& Report);

// Scans, then writes each match as <OutDir>/<name>.png. NameFilter, when non-empty, keeps only
// the images whose name contains it (case-insensitive). Returns how many were written.
// Requires WraithX::InitializeAPI (WIC) first.
size_t Bo3ExportZoneImages(const std::string& FastFilePath, const std::string& XpakDir,
                           const std::string& OutDir, const std::string& NameFilter,
                           std::string& Report);

// One GfxImage at zone offset Start (its 0x108-byte struct, then its name): the largest mip the
// packages carry, the inline pixels after the name, or (ZoneStem given: the zone's file stem)
// the object its own packages' index names. False when it is not one. The scan above
// and the zonekit t7 walk (Games/T7/DiskGame.cpp, which knows where every image is) share it.
bool Bo3DescribeZoneImage(ByteView Zone, size_t Start, XPAKCache& Cache,
                          Bo3ZoneImage& Image, const std::string& ZoneStem = std::string());

// Writes described images (Bo3ExportZoneImages' second half). Returns how many were written.
// Step(done, count) as it goes, when given (DiskGame::Context::Advance); false stops there.
size_t Bo3WriteZoneImages(ByteView Zone, const std::vector<Bo3ZoneImage>& Images,
                          XPAKCache& Cache, const std::string& OutDir, const std::string& NameFilter,
                          std::string& Report, const std::function<bool(size_t, size_t)>& Step = nullptr);
