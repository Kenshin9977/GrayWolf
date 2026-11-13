#pragma once

// Modern Warfare 2 (IW4) and Modern Warfare 3 (IW5): the live readers, run over a zone served by
// VirtualGameSession (Games/IW4/DiskGame.cpp), for both builds of each game: the 64-bit rebuild
// (2026, the zonekit iw4/iw5 tables: XModel 0x198 / 0x1a0, XSurface 0x58, XAnimParts 0x88, Material
// 0x78 / 0x80, GfxImage 0x28) and the original 32-bit one (the iw4x86/iw5x86 tables, read off the
// Steam iw4mp.exe / iw5mp.exe). Every offset is one the game's own loaders read, not a community
// header. A build differs from another only where Layout says; its defaults are the 64-bit
// rebuild's.

#include "Games/IW/IWSoundAliases.h"
#include "Games/IW4/Iw4Readers.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class CoDAnim_t;
class CoDModel_t;
class CoDSound_t;
struct XMaterial_t;
struct XModel_t;
struct XAnim_t;
struct XImageDDS;

namespace Iw64
{
    struct Layout
    {
        const char* Title;
        // Material: the texture count (u8) and the MaterialTextureDef table pointer (16-byte
        // records, 12 in the 32-bit builds)
        int64_t MaterialTextureCount, MaterialTextureTable;
        // snd_alias_t: its size in an alias list
        int64_t AliasSize;
        // The level (Games/IW4/IwMap.cpp), where the builds' structs differ: GfxWorld's
        // GfxWorldDraw (vertex count and buffer, index count and buffer, from the world), brush
        // models (count, array) and GfxWorldDpvsStatic; ComPrimaryLight's size and the offsets of
        // its origin, radius and cosHalfFovOuter; clipMap_t's two dynamic entity lists (u16
        // counts, arrays) and DynEntityDef's size. Then what only the word size moves: GfxWorld's
        // base name, surface count, sky count and skies (GfxSky: its size, surface list, image);
        // GfxWorldDpvsStatic's surfaces (GfxSurface: size, material) and static model draw records
        // (size, model); ComWorld's light count and array; MapEnts' MapTriggers.
        struct MapLayout
        {
            int64_t VertexCount, Vertices, IndexCount, Indices;
            int64_t BrushCount, Brushes, Dpvs;
            int64_t LightSize, LightOrigin, LightRadius, LightCosOuter;
            int64_t DynCount, Dyns, DynSize;
            int64_t WorldBaseName = 8, WorldSurfaceCount = 0x18, WorldSkyCount = 0x1c, WorldSkies = 0x20;
            int64_t SkySize = 0x20, SkySurfs = 8, SkyImage = 0x10;
            int64_t DpvsSurfaces = 0x78, DpvsSModels = 0x88;
            int64_t SurfaceSize = 0x20, SurfaceMaterial = 0x10, SModelSize = 0x58, SModelModel = 0x38;
            int64_t LightCount = 0xc, Lights = 0x10;
            int64_t Triggers = 0x18;
        } Map;
        // The build's word size: 8 in the rebuild, 4 in the originals
        int64_t PointerSize = 8;
        // GfxImage: the pixels' GfxImageLoadDef pointer, mapType, semantic, width, height (u16) and
        // the name pointer (GfxImageLoadDef: D3D format at 8, resourceSize at 0xc, the pixels from
        // 0x10, in every build)
        struct ImageLayout
        {
            int64_t LoadDef = 0x0, MapType = 0x8, Semantic = 0x9, Width = 0x18, Height = 0x1a, Name = 0x20;
        } Image;
        // LoadedSound: its format, channels and bits (read as u16), sample rate, payload length
        // (u32) and payload pointer: the rebuild's WAVEFORMATEX at +8 (payload length at +0x20,
        // pointer at +0x38); the originals' _AILSOUNDINFO at +4 (CoD4's: data_len +0xc, the
        // payload pointer at +0x28)
        struct LoadedLayout
        {
            int64_t Format = 0x8, Channels = 0xa, Rate = 0xc, Bits = 0x16, Bytes = 0x20, Data = 0x38;
        } Loaded;
        // snd_alias_list_t / snd_alias_t as the alias CSV reads them (its SoundFile, every build)
        IWSoundAliases::Layout Aliases;
        // XModel, XSurface, XAnimParts (Iw4Family::Layout's shapes; its material and image fields
        // and lookups are this Layout's)
        Iw4Family::Layout Shapes;
        // The XFile's size: the XAssetList (and its script strings' count) follows it
        int64_t XFileSize = 0x28;
        // This build's own readers through the zone (OpenAssetTools' weapons, the fx and material
        // kinds' effect layout): the rebuild's only
        bool Rebuild = true;
    };
    extern const Layout Mw2, Mw3, Mw2x86, Mw3x86;

    // A model's name-only material header (",name"), by the name without the ",": where the zone
    // served now defines that material (0: it does not), else the images another zone's material
    // of that name has (false: none booked); unset: none is looked up
    // (Iw4Family::Layout::MaterialByName, MaterialImagesByName)
    extern thread_local std::function<uint64_t(const std::string& Name)> MaterialDefinedAt;
    extern thread_local std::function<bool(const std::string& Name, std::vector<std::pair<uint32_t, std::string>>& Images)>
        MaterialImagesElsewhere;

    // GfxImageLoadDef: D3D format at 8, resourceSize at 0xc, then resourceSize bytes of pixels
    constexpr int64_t LoadDefFormat = 0x8, LoadDefSize = 0xc, LoadDefData = 0x10;

    std::unique_ptr<XModel_t> ReadXModel(const CoDModel_t* Model, const Layout& L);
    std::unique_ptr<XAnim_t> ReadXAnim(const CoDAnim_t* Animation, const Layout& L);
    XMaterial_t ReadXMaterial(uint64_t MaterialPointer, const Layout& L);
    // The pixels a GfxImage holds in the zone (its loadDef), as a DDS; nullptr when it holds
    // none (resourceSize 0: the image is streamed from the install's .iwd files)
    std::unique_ptr<XImageDDS> ZoneImage(uint64_t ImagePointer, const Layout& L);
    // A loaded sound's PCM, filled into Sound; false when it holds none
    bool LoadedSound(uint64_t LoadedSoundPointer, CoDSound_t& Sound, const Layout& L);
}
