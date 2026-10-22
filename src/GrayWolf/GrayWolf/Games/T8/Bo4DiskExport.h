#pragma once

// Bo4DiskExport - BO4 assets straight from zone files, no game running: the zone is walked with
// the zonekit tables (ZoneTableWalker, profile bo4), and each asset is read at the place the walk
// found it.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "VirtualGameSession.h"
#include "ZoneTableWalker.h"

class CoDPackageCache;
namespace DiskGame { struct SoundHeader; }

namespace Bo4DiskExport
{
    struct ImageInfo
    {
        int64_t Pos = 0;            // where the image struct is in the zone
        uint64_t Hash = 0;          // 60-bit name hash, u64@0x20
        std::string Name;           // from the name index, or ximage_<hash>
        uint32_t Format = 0;        // DXGI, u32@0x64
        uint16_t Width = 0, Height = 0, Depth = 0;
        uint8_t MapType = 0, Mips = 0;
        bool Streamed = false;      // u8@0x7c != 0: the pixels are in a package, not the zone
        uint32_t PayloadSize = 0;   // u32@0x60
        int64_t PayloadPos = -1;    // zone offset of the in-zone pixels, -1 when absent
        // the streamed mips (the 0x28-byte records at +0x30): the largest one's package key
        uint64_t MipHash = 0;
        uint32_t MipSize = 0;
        uint16_t MipWidth = 0, MipHeight = 0;
        std::vector<uint64_t> MipKeys;   // every mip record's package key, widest or not
    };

    struct ZoneAssets
    {
        std::string Why;                       // how the walk ended
        bool Complete = false;                 // walked to the zone's last byte
        std::vector<std::pair<std::string, int64_t>> Assets;   // zone-level (type, offset)
        std::vector<ImageInfo> Images;         // every image instance, nested ones included
        std::vector<std::pair<uint64_t, int64_t>> Models;      // xmodel (name hash, offset)
        std::vector<std::pair<uint64_t, int64_t>> Anims;       // xanim (name hash, offset)
        std::vector<std::pair<uint64_t, int64_t>> Materials;   // material (name hash, offset)
        std::vector<std::pair<std::string, int64_t>> Roots;    // every asset instance: (type, offset)
        std::vector<std::pair<int64_t, int64_t>> Links;        // pointer field -> its data
        size_t BackRefs = 0, BackRefsResolved = 0;             // back references met / placed
    };

    // The zone: a decompressed zone (.bin) or a fast file (.ff, decompressed here, with the
    // update patch of the same name applied when an .fd sits next to it - the zone the game loads).
    bool LoadZone(const std::string& Path, std::vector<uint8_t>& Zone, std::string& Error);

    // The 1080_ and 4k_ zones (also behind a locale prefix: arasafe_4k_core_ui) hold the same UI
    // images, the 4K ones at twice the width and height. UiRes "4k" or "1080" keeps that set only,
    // "all" both; every other zone is always kept. Takes a zone name, path or file name.
    bool KeepZoneForUiRes(const std::string& Zone, const std::string& UiRes);

    // The zones an input names: a zone (.ff/.bin), a folder of them, a .txt list, or casc:<pattern>
    // pulled from Packages into CacheDir; then only the UiRes set of the 1080_/4k_ zones.
    // Skipped counts the zones the UI resolution left out.
    bool Zones(const std::string& Input, CoDPackageCache* Packages, const std::string& CacheDir,
               const std::string& UiRes, std::vector<std::string>& Out, size_t& Skipped, std::string& Error);

    // Walks the zone with the profile's tables and lists what it holds.
    bool ReadZone(const std::vector<uint8_t>& Zone, const std::string& Profile, ZoneAssets& Out,
                  std::string& Error);
    // What an existing layout walk holds (ReadZone without the walk): assets, images, models...
    void DescribeZone(const std::vector<uint8_t>& Zone, const ZoneTables::ZoneResult& R, ZoneAssets& Out);

    // BO4's live readers on a zone served as memory: its string table and the disk image handler.
    // ServeZone points them at a zone (per thread) until EndServe; Assets must outlive the serving.
    VirtualGameSession::Handlers LiveHandlers();
    void ServeZone(const std::vector<uint8_t>& Zone, const ZoneAssets& Assets);
    void EndServe();
    // An asset's name from BO4's name index, or <prefix>_<hash>.
    std::string LiveName(uint64_t Hash, const char* Prefix);

    struct ExportStats
    {
        size_t Images = 0, Written = 0, FromPackages = 0, References = 0, Missing = 0, Failed = 0;
        // Linked: written as a hard link to an identical image (same content key) already exported
        size_t Linked = 0;
        // Sounds: entries without samples in any bank of their language (a line that language does not
        // record, its key a placeholder), not counted in Images
        size_t Empty = 0;
        // References: name-only headers (no size, no mips) of an image another zone defines
        std::vector<std::string> MissingSamples;   // why, for the first few without pixels
    };

    // BO4 sounds are not in the zones: the install's banks named after the zone hold them
    // (zm_red.all.sabl, zm_red.fr.sabs...: Bo4SoundBanks, read in place). Writes each sound matching
    // the filter under OutDir in the export settings' format, a language's under OutDir\<language>.
    // Step(done, count, asset) as it goes (DiskGame::Context::Advance), when given; false stops there.
    // Wants(name): whether to write that asset (DiskGame::Context::Wants), all when not given.
    // Headers, when given: a dry run (DiskGame::Context::OnSoundHeader), each sound's header, no sample.
    using StepFn = std::function<bool(size_t Done, size_t Count, const std::string& Asset)>;
    using NameTest = std::function<bool(const std::string&)>;
    using HeaderFn = std::function<void(const DiskGame::SoundHeader&)>;
    ExportStats ExportSounds(CoDPackageCache* Packages, const std::string& Zone, const std::string& OutDir,
                             const NameTest& Wants, const StepFn& Step = nullptr, const HeaderFn& Headers = nullptr);

    // Every material's texture slots: (material hash, slot, semantic hash, image hash), the
    // image found through the resolved pointer (inline or back reference); 0 when unresolved.
    // Every resolved pointer that lands on an asset instance's root: which zone-level asset holds
    // the pointer, and which asset it names. (owner type, owner offset, target type, target offset)
    struct AssetRef { std::string OwnerType; int64_t Owner = 0; std::string TargetType; int64_t Target = 0; };
    std::vector<AssetRef> AssetRefs(const ZoneAssets& Assets);

    struct MaterialImage { uint64_t Material = 0; uint32_t Slot = 0, Semantic = 0; uint64_t Image = 0; };
    std::vector<MaterialImage> MaterialImages(const std::vector<uint8_t>& Zone, const ZoneAssets& Assets);

    // A zone's materials: name hash -> its images (image name hash, semantic hash) in slot order,
    // its technique set's name hash and its constant blob (what the material adapter reads).
    struct BookMaterial
    {
        std::vector<std::pair<uint64_t, uint32_t>> Images;
        uint64_t Techset = 0;
        std::vector<uint8_t> Constants;
    };
    using MaterialBook = std::unordered_map<uint64_t, BookMaterial>;
    // What a level's name-only material headers stand for: the game loads core_common and the
    // mode's common zone (zm_common...) first, which define them. Their books, read once from
    // each zone (beside Zone, else from Packages into FfCache), kept in BookDir.
    std::vector<std::shared_ptr<const MaterialBook>> CommonMaterials(const std::string& Zone, CoDPackageCache* Packages,
                                                                     const std::string& FfCache, const std::string& BookDir);

    // The zone's string table, the encrypted entries decrypted as the game's interning does.
    const std::vector<std::string>& ZoneStringTable(const std::vector<uint8_t>& Zone);
    // The zone string at Start, up to its NUL (End: the NUL's offset): the game's encrypted form
    // ([10xxxxxx cipher id][length][ciphertext]) decrypted as its interning does, else the bytes.
    std::string ZoneString(const std::vector<uint8_t>& Zone, size_t Start, size_t& End);

    // Writes every image Wants, in the export format: in-zone ones from the zone, streamed ones
    // from Packages when given. A wanted name-only header is counted and given to OnReference.
    ExportStats ExportImages(const std::vector<uint8_t>& Zone, const ZoneAssets& Assets,
                             const std::string& OutDir, const NameTest& Wants,
                             CoDPackageCache* Packages = nullptr, const StepFn& Step = nullptr,
                             const std::function<void(const std::string&)>& OnReference = nullptr);
    // a name-only header: no size and no mips (another zone defines the image)
    inline bool IsReference(const ImageInfo& I) { return I.Width == 0 && I.PayloadSize == 0 && I.MipHash == 0; }
}
