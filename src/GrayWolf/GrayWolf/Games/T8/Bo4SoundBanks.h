#pragma once

// Black Ops 4's sound banks (zone\snd\<language>\<zone>.<language>[.p].sabl / .sabs) read in place
// from the packages' file system: headers and entry tables, the samples when written. Format, the
// .sabl heads of streamed sounds, the keys' hash and the language paths: docs/games/T8.md, "Sounds".

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class CoDFileSystem;
class ICoDGame;

namespace Bo4SoundBanks
{
    struct Entry
    {
        uint64_t Key = 0;          // 63 bits
        uint64_t Offset = 0;       // in its bank
        uint32_t Size = 0, Frames = 0, Rate = 0;
        uint8_t Channels = 0;
        size_t Bank = 0;           // index in Zone::Banks
        uint32_t Index = 0;        // in its bank
    };

    struct Bank
    {
        std::string Path;          // in the file system (zone\snd\fr\zm_red.fr.sabs)
        std::string File;          // zm_red.fr.sabs
        std::string Language;      // "all", "en", "fr"... (the name's second part)
    };

    struct Zone
    {
        std::vector<Bank> Banks;   // sorted by file name
        // One per key and language: the entry with the most bytes (a streamed sound's whole file
        // rather than its .sabl head, a sound rather than its placeholder); bank, offset order.
        std::vector<Entry> Entries;
        size_t Read = 0;           // entries in the banks
        size_t Duplicates = 0;     // left out: the key again in another bank of that language
        std::vector<std::string> Failures;   // banks not read, why
    };

    // The zone's banks (<zone>.*.sabl and .sabs) in FS.
    Zone Read(CoDFileSystem& FS, const std::string& ZoneName);

    // The path a key hashes, from the name index (bo4_sab.wni, fnv1a_xsounds.wni), else through
    // another language's path in it ("en\x" names "fr\x" in a fr bank); empty if none.
    std::string NameOf(uint64_t Key, const std::string& Language);
    // FNV-1a 64 of the path, lower case, backslashes, bit 63 cleared: an entry's key
    uint64_t Hash(const std::string& Path);
    // A sound's folder under the sound folder: Name's folder for "all"; for a language,
    // <language>\ then Name's folder without its leading <language> (fr\vox\x\y -> fr\vox\x)
    std::string Folder(const std::string& Name, const std::string& Language);

    // CoDAssets::Game while a zone's sounds are written: a CoDSound_t's BankFile is its bank's path
    // in FS, AssetPointer and AssetSize its FLAC file.
    std::unique_ptr<ICoDGame> Reader(CoDFileSystem& FS);
}
