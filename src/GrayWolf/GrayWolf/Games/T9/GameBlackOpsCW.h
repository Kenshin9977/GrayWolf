#pragma once

#include <cstdint>
#include <memory>

class CoDAnim_t;
class CoDModel_t;
class CoDSound_t;
struct XImage_t;
struct XMaterial_t;
struct XModelLod_t;
struct XModel_t;
struct XAnim_t;
struct XImageDDS;
struct XSound;
class WraithModel;
class WraithNameIndex;

class GameBlackOpsCW
{
public:
    static WraithNameIndex AssetNameCache;
    static WraithNameIndex StringCache;

    static std::unique_ptr<XAnim_t> ReadXAnim(const CoDAnim_t* Animation);
    static std::unique_ptr<XModel_t> ReadXModel(const CoDModel_t* Model);
    static std::unique_ptr<XSound> ReadXSound(const CoDSound_t* Sound);
    // a sound_asset's object (package key 0x38, or the streamkey 0x40's data: in the zone or the
    // packages, its packets from 0x120): Offset/Size its packets, Held the bytes read in all
    static bool ReadSoundObject(uint64_t AssetPointer, std::unique_ptr<uint8_t[]>& Buffer, size_t& Offset, size_t& Size, uint32_t* Held = nullptr);
    static const XMaterial_t ReadXMaterial(uint64_t MaterialPointer);
    static std::unique_ptr<XImageDDS> LoadXImage(const XImage_t& Image);
    static void LoadXModel(const XModelLod_t& ModelLOD, const std::unique_ptr<WraithModel>& ResultModel);

    static void PerformInitialSetup();
};