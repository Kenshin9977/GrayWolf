#pragma once

// A CoD1/CoD2 install's files as the game sees them: every .pk3/.iwd zip of its main folder, the
// later archive (by name) winning a path several hold, as Quake 3's file system orders them; the
// localized archives (localized_<language>_*) of the chosen language after the base ones (their
// files replace the base's, as the game does in that language), the others' before (theirs only
// fill what the base lacks). English, the default, puts every localized archive first. A zone
// of the disk template is one archive; an asset in it may need files of others (a model's parts,
// surfaces and textures), which the install gives. Loaded once per process per folder.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace IW12
{
    class Install
    {
    public:
        // The install at Folder (the game's folder, or its main folder), with archives of Extension
        // (".pk3", ".iwd"); nullptr and Error when it holds none.
        static std::shared_ptr<Install> Open(const std::string& Folder, const std::string& Extension,
                                             const std::string& Language, std::string& Error);
        // The archives a folder holds, in the order above (the main folder looked in).
        static std::vector<std::string> Archives(const std::string& Folder, const std::string& Extension,
                                                 const std::string& Language);

        ~Install();
        const std::string& Folder() const { return MainFolder; }
        const std::vector<std::string>& ArchivePaths() const { return Paths; }
        // The archive holding the copy of Path (lower case, forward slashes) the game reads, -1 if none.
        int Owner(const std::string& Path) const;
        // The files whose winning copy is in archive A (lower-case paths).
        const std::vector<std::string>& FilesOf(int A) const { return Owned[A]; }
        // Every file the game sees, lower case, sorted.
        const std::map<std::string, int>& Files() const { return Winner; }
        // A file's bytes, as the game reads it; false when no archive holds it or it does not inflate.
        bool Read(const std::string& Path, std::vector<uint8_t>& Out) const;
        // ... and its size inflated, 0 when no archive holds it
        uint64_t Size(const std::string& Path) const;
        int ArchiveOf(const std::string& ArchivePath) const;   // by file name, -1 if not this install's

    private:
        struct Archive;
        std::string MainFolder;
        std::vector<std::string> Paths;
        std::vector<std::unique_ptr<Archive>> Zips;
        std::map<std::string, int> Winner;                // path -> archive
        std::map<std::string, uint32_t> WinnerIndex;      // path -> file index in that archive
        std::vector<std::vector<std::string>> Owned;
    };

    // Lower case, forward slashes.
    std::string NormalPath(std::string Path);
}
