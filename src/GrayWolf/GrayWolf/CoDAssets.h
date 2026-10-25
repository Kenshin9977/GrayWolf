#pragma once

// The process-wide state the live readers (Game*.cpp, the translators) read: the game being read,
// its memory, its asset pool, package caches, name indices and handlers. VirtualGameSession swaps
// it in for a served zone. Writing an asset is LiveExport.h; the `file` mode is PackageFileMode.h.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ProcessReader.h"

#include "CoDAssetType.h"
#include "CoDPackageCache.h"
#include "CoDCDNDownloader.h"

#include <spdlog/spdlog.h>

class WraithNameIndex;

class ICoDGame;   // ICoDGame.h

enum class SupportedGameFlags
{
    None,
    Files,
    SP,
    MP,
    ZM
};

typedef std::unique_ptr<XImageDDS>(*LoadXImageHandler)(const XImage_t& Image);
typedef std::string(*LoadStringHandler)(uint64_t Index);

class CoDAssets
{
public:
    // The game's memory as its readers see it: a relocated zone (VirtualGameSession)
    static std::unique_ptr<ProcessReader> GameInstance;
    static std::shared_ptr<spdlog::logger> Log;
    // The live readers of the game being read, if any (ICoDGame.h)
    static const ICoDGame* Game;
    // That game's flags, if any
    static SupportedGameFlags GameFlags;

    static std::unique_ptr<AssetPool> GameAssets;
    // The game's package cache, if any
    static std::unique_ptr<CoDPackageCache> GamePackageCache;
    static std::unique_ptr<CoDPackageCache> OnDemandCache;
    // The game's cdn downloader, if any
    static std::unique_ptr<CoDCDNDownloader> CDNDownloader;
    static LoadXImageHandler GameXImageHandler;
    static LoadStringHandler GameStringHandler;
    static WraithNameIndex AssetNameCache;
    static WraithNameIndex StringCache;

    // Releases the loaded assets, caches and game state
    static void CleanUpGame();

    // Gets an asset hash. If one is not found, a default value is used.
    static std::string GetHashedName(const std::string& type, const uint64_t hash);
    // Gets a string hash. If one is not found, a default value is used.
    static std::string GetHashedString(const std::string& type, const uint64_t hash);
};
