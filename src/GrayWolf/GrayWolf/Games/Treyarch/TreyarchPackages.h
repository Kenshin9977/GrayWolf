#pragma once

#include <memory>
#include <string>
#include <typeinfo>
#include <vector>

class CoDPackageCache;

// The Treyarch titles' streamed-data packages in a Battle.net CASC install, opened as CoDAssets'
// package cache, which the live model, image and sound readers take streamed data from. Each game
// names its own package format: Black Ops 4 its .xpak (XPAKCache), Cold War its .xsub (XSUBCache).
namespace TreyarchPackages
{
    // The fast file names (".ff") in the install's packages' file system (CASC), sorted.
    std::vector<std::string> ListFastFiles(CoDPackageCache* Packages);

    // Pulls the fast files matching Pattern (e.g. "*.ff", "zm_*.ff", or one file name) out of the
    // packages' file system into CacheDir, each with its update patch (PatchExtension: .fd, Modern
    // Warfare 2019's .fp) when there is one; returns the .ff paths. A file already there at the
    // same size is not extracted again.
    std::vector<std::string> ExtractFastFiles(CoDPackageCache* Packages, const std::string& Pattern,
                                              const std::string& CacheDir, std::string& Error,
                                              const char* PatchExtension = ".fd");
    // IDiskGame::ResolveZones through ExtractFastFiles: each of Names pulled into CacheDir and its
    // path added to Out; false with Error when none is (Install names the install there)
    bool ExtractZones(CoDPackageCache* Packages, const std::string& Install, const std::vector<std::string>& Names,
                      const std::string& CacheDir, std::vector<std::string>& Out, std::string& Error,
                      const char* PatchExtension = ".fd");

    using Factory = std::unique_ptr<CoDPackageCache> (*)();

    // nullptr if the install cannot be opened. Loaded synchronously, once per install and format.
    CoDPackageCache* Open(const std::string& Install, const char* Format, Factory Make);

    template <class Cache>
    CoDPackageCache* Open(const std::string& Install)
    {
        return Open(Install, typeid(Cache).name(), []() -> std::unique_ptr<CoDPackageCache> { return std::make_unique<Cache>(); });
    }
}
