#pragma once

// Modern Warfare 2 (IW4) and Modern Warfare 3 (IW5), the 64-bit rebuild: the live
// readers, run over a zone served by VirtualGameSession (Games/IW4/DiskGame.cpp). Every offset
// below is the one the game's own loaders read (the zonekit iw4/iw5 tables, from the machine
// code: XModel 0x198 / 0x1a0, XSurface 0x58, XAnimParts 0x88, Material 0x78 / 0x80, GfxImage
// 0x28), not a community header. MW2 and MW3 differ only where Layout says.

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
        // Material: the texture count (u8) and the 16-byte MaterialTextureDef table pointer
        int64_t MaterialTextureCount, MaterialTextureTable;
        // snd_alias_t: its size in an alias list (its SoundFile pointer is at 0x28 in both)
        int64_t AliasSize;
        // The level (Games/IW4/IwMap.cpp), where the two games' structs differ: GfxWorld's
        // GfxWorldDraw (vertex count and buffer, index count and buffer, from the world), brush
        // models (count, array) and GfxWorldDpvsStatic; ComPrimaryLight's size and the offsets of
        // its origin, radius and cosHalfFovOuter; clipMap_t's two dynamic entity lists (u16
        // counts, arrays) and DynEntityDef's size.
        struct MapLayout
        {
            int64_t VertexCount, Vertices, IndexCount, Indices;
            int64_t BrushCount, Brushes, Dpvs;
            int64_t LightSize, LightOrigin, LightRadius, LightCosOuter;
            int64_t DynCount, Dyns, DynSize;
        } Map;
    };
    extern const Layout Mw2, Mw3;

    // A model's name-only material header (",name"), by the name without the ",": where the zone
    // served now defines that material (0: it does not), else the images another zone's material
    // of that name has (false: none booked); unset: none is looked up
    // (Iw4Family::Layout::MaterialByName, MaterialImagesByName)
    extern thread_local std::function<uint64_t(const std::string& Name)> MaterialDefinedAt;
    extern thread_local std::function<bool(const std::string& Name, std::vector<std::pair<uint32_t, std::string>>& Images)>
        MaterialImagesElsewhere;

    // GfxImage: the pixels' GfxImageLoadDef pointer, mapType, cardMemory (8 bytes at 0x10), width,
    // height, then the name pointer
    constexpr int64_t ImageLoadDef = 0x0, ImageMapType = 0x8, ImageSemantic = 0x9, ImageWidth = 0x18,
                      ImageHeight = 0x1a, ImageName = 0x20;
    // GfxImageLoadDef: levelCount (u8) at 0, flags at 4, D3D format at 8, resourceSize at 0xc,
    // then resourceSize bytes of pixels
    constexpr int64_t LoadDefLevels = 0x0, LoadDefFormat = 0x8, LoadDefSize = 0xc, LoadDefData = 0x10;

    std::unique_ptr<XModel_t> ReadXModel(const CoDModel_t* Model, const Layout& L);
    std::unique_ptr<XAnim_t> ReadXAnim(const CoDAnim_t* Animation);
    XMaterial_t ReadXMaterial(uint64_t MaterialPointer, const Layout& L);
    // The pixels a GfxImage holds in the zone (its loadDef), as a DDS; nullptr when it holds
    // none (resourceSize 0: the image is streamed from the install's .iwd files)
    std::unique_ptr<XImageDDS> ZoneImage(uint64_t ImagePointer);
    // A loaded sound's PCM (LoadedSound + 8: a WAVEFORMATEX, the payload length at +0x18, the
    // payload pointer at +0x30), filled into Sound; false when it holds none
    bool LoadedSound(uint64_t LoadedSoundPointer, CoDSound_t& Sound);
}
