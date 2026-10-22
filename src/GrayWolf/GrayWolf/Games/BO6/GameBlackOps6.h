#pragma once

// Black Ops 6's readers over its zone served as game memory (VirtualGameSession), written from
// C2M's BlackOps6.cs and every offset checked against the bo6 profile's reader tables (the loader's own reads).

#include <cstdint>
#include <memory>
#include <string>

#include "CoDXModelTranslator.h"

class GameBlackOps6
{
public:
    static std::unique_ptr<XModel_t> ReadXModel(const CoDModel_t* Model);
    static void LoadXModel(const std::unique_ptr<XModel_t>& Model, const XModelLod_t& ModelLOD, const std::unique_ptr<WraithModel>& ResultModel);
    static const XMaterial_t ReadXMaterial(uint64_t MaterialPointer);
    static std::unique_ptr<XImageDDS> ReadXImage(const CoDImage_t* Image);
    static std::unique_ptr<XImageDDS> LoadXImage(const XImage_t& Image);
    // sndasset (0x40, loader 0x28645f0) at Sound->AssetPointer: its Opus frames from the .xsub
    // object (32-byte header, seek table, frames), else the frames the zone primes
    static std::unique_ptr<XSound> ReadXSound(const CoDSound_t* Sound);
    // xanim (0x78, loader 0x2886aa0): the header's counts in the zone, the keys packed as Modern
    // Warfare's (PackedXAnim.h) in one or more .xsub objects (or in the zone); ReadXAnim names the
    // bones, LoadXAnim the keys
    static std::unique_ptr<XAnim_t> ReadXAnim(const CoDAnim_t* Animation);
    static void LoadXAnim(const std::unique_ptr<XAnim_t>& Anim, std::unique_ptr<WraithAnim>& Result);

    // One face of a packed index buffer (models and the world): its three corners (u16 indices at
    // Indices), from 32-byte tables at Tables and the bit-packed corners at PackedBase; offsets into Buffer
    static bool UnpackFace(const uint8_t* Buffer, size_t Size, uint32_t TableCount, uint64_t Indices, uint64_t Tables, uint64_t PackedBase,
                           uint32_t Face, uint32_t (&Out)[3]);
    // An asset's name from its 63-bit hash at +0 (the name index's, else <type>_<hex>)
    static std::string HashedName(const char* Type, uint64_t Hash);
    // The name indexes (package_index), loaded once
    static void PerformInitialSetup();

    // XModel (0xe8): hash 0, name 8, surface count u16 0x10, LOD count u8 0x12, the skeleton at
    // 0x68, the material slots at 0x90, the LODs (0x48 each) at 0x98
    static constexpr int64_t ModelName = 8, ModelSurfaces = 0x10, ModelLodCount = 0x12, ModelSkeleton = 0x68,
                             ModelMaterials = 0x90, ModelLods = 0x98, LodSize = 0x48;
    // GfxImage (0x40): hash 0, in-zone size u32 0x18, width u16 0x20, height 0x22, format u8 0x2a,
    // mip count u8 0x2e (low 4 bits), 24-byte mip records at 0x30, in-zone pixels at 0x38
    // sndasset: hash 0; flags u8 0x30 (bits 3-4 set: frames primed in the zone at 0x10, their size
    // u32 0x18, the .xsub key at 8; else the key at 0x10); seek table size u32 0x20, frames' size u32
    // 0x24, frame count u32 0x28, rate u16 0x32
    static constexpr int64_t SoundFlags = 0x30, SoundSeek = 0x20, SoundLoaded = 0x24, SoundFrames = 0x28, SoundRate = 0x32;
    // The game's languages, the last part of a localized sound's hashed name (IwLanguageNames): its
    // language table, 23 rows {name, zone prefix, id, locale} of 32 bytes at RVA 0xb328ac0 of
    // bo6_flat.exe (0xafafac0 of bo6sp_flat.exe, the same rows); english_safe is the ens_ zones'
    static constexpr const char* Languages[] = { "english", "english_safe", "french", "german", "italian", "spanish",
                                                 "russian_full", "russian_partial", "polish_full", "polish_partial", "korean_full",
                                                 "korean_partial", "japanese_full", "japanese_partial", "simplified_chinese",
                                                 "traditional_chinese", "safe_chinese", "arabic", "czech", "spanishna", "portuguese",
                                                 "thai", "turkish" };
    // The campaign (sp24, profile bo6sp) moves a few fields: the model's skeleton, materials and
    // LODs 8 bytes on (0x70, 0x98, 0xa0), 12-byte rigid runs, 28-byte face tables (no flag byte: the
    // corners plain fixed-width), the sound's flags u8 at 0x3c and its rate u16 at 0x2c, its .xsub object
    // without the 32-byte header (the seek table first), its own image format table. The readers
    // take the calling thread's layout (Campaign: LiveGame.cpp sets it).
    struct Layout
    {
        int64_t ModelSkeleton, ModelMaterials, ModelLods, SoundFlags, SoundRate;
        uint32_t RigidRunSize, FaceTableSize, SoundHeader;
        int32_t SoundPreSkip;          // the Opus pre-skip (multiplayer 312, campaign 960: SABSupport::OpusPreSkip...)
        const uint8_t* ImageFormats;   // GfxImage format -> DXGI, ImageFormatCount entries
        uint32_t ImageFormatCount;
    };
    static const Layout Multiplayer, Campaign;
    static thread_local const Layout* Current;
    // the thread's layout for one call
    struct LayoutScope
    {
        const Layout* Was;
        explicit LayoutScope(const Layout& L) : Was(Current) { Current = &L; }
        ~LayoutScope() { Current = Was; }
        LayoutScope(const LayoutScope&) = delete;
        LayoutScope& operator=(const LayoutScope&) = delete;
    };
    // LoadXImage with the campaign's layout (the zone's image handler)
    static std::unique_ptr<XImageDDS> LoadXImageCampaign(const XImage_t& Image);
    static constexpr int64_t ImageInZoneSize = 0x18, ImageWidth = 0x20, ImageHeight = 0x22, ImageFormat = 0x2a,
                             ImageMipCount = 0x2e, ImageMips = 0x30, ImagePixels = 0x38;
};
