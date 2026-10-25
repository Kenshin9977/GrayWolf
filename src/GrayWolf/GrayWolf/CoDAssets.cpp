#include "stdafx.h"

#include "CoDAssets.h"

#include "WraithNameIndex.h"

#include <format>

WraithNameIndex CoDAssets::AssetNameCache;
WraithNameIndex CoDAssets::StringCache;

std::unique_ptr<ProcessReader> CoDAssets::GameInstance = nullptr;
std::shared_ptr<spdlog::logger> CoDAssets::Log = nullptr;

const ICoDGame* CoDAssets::Game = nullptr;
SupportedGameFlags CoDAssets::GameFlags = SupportedGameFlags::None;

std::unique_ptr<AssetPool> CoDAssets::GameAssets = nullptr;
std::unique_ptr<CoDPackageCache> CoDAssets::GamePackageCache = nullptr;
std::unique_ptr<CoDPackageCache> CoDAssets::OnDemandCache = nullptr;
std::unique_ptr<CoDCDNDownloader> CoDAssets::CDNDownloader = nullptr;

LoadXImageHandler CoDAssets::GameXImageHandler = nullptr;
LoadStringHandler CoDAssets::GameStringHandler = nullptr;

std::string CoDAssets::GetHashedName(const std::string& type, const uint64_t hash)
{
    auto found = AssetNameCache.NameDatabase.find(hash & 0xFFFFFFFFFFFFFFF);

    if (found != AssetNameCache.NameDatabase.end())
    {
        return found->second;
    }

    return std::format("{}_{:x}", type.c_str(), hash & 0xFFFFFFFFFFFFFFF);
}

std::string CoDAssets::GetHashedString(const std::string& type, const uint64_t hash)
{
    auto found = StringCache.NameDatabase.find(hash);

    if (found != StringCache.NameDatabase.end())
    {
        return found->second;
    }

    return std::format("{}_{:x}", type.c_str(), hash);
}

void CoDAssets::CleanUpGame()
{
    // The caches finish loading before they go
    for (auto* Cache : { &GamePackageCache, &OnDemandCache })
    {
        if (*Cache != nullptr)
        {
            (*Cache)->WaitForPackageCacheLoad();
            Cache->reset();
        }
    }

    GameAssets.reset();

    AssetNameCache.NameDatabase.clear();
    StringCache.NameDatabase.clear();

    GameXImageHandler = nullptr;

    GameFlags = SupportedGameFlags::None;
    Game = nullptr;

    GameInstance.reset();
}
