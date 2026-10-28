#pragma once

#include "ByteView.h"

#include <cstdint>
#include <memory>
#include <string>
#include <functional>
#include <vector>

class CoDAnim_t;
class CoDModel_t;
struct XMaterial_t;
struct XModelLod_t;
struct XModel_t;
struct XAnim_t;
class WraithModel;
class WraithNameIndex;

class GameBlackOps4
{
public:
    static WraithNameIndex AssetNameCache;

    static std::unique_ptr<XAnim_t> ReadXAnim(const CoDAnim_t* Animation);
    static std::unique_ptr<XModel_t> ReadXModel(const CoDModel_t* Model);
    static const XMaterial_t ReadXMaterial(uint64_t MaterialPointer);
    // What another zone gives a material header without images: its images (name hash, semantic
    // hash), its technique set's name hash and its constant blob.
    struct MaterialElsewhere
    {
        std::vector<std::pair<uint64_t, uint32_t>> Images;
        uint64_t Techset = 0;
        std::vector<uint8_t> Constants;
    };
    // A disk export's: a material header without images (another zone defines the material) ->
    // that zone's material; false when unknown. Unset outside a disk export.
    static thread_local std::function<bool(uint64_t MaterialHash, MaterialElsewhere& Out)> ImagesElsewhere;
    static void LoadXModel(const XModelLod_t& ModelLOD, const std::unique_ptr<WraithModel>& ResultModel);

    static std::string DecryptString(uint8_t* InputBuffer, uint8_t InputLength, uint8_t EncryptionID, uint64_t StringHash);

    static void PerformInitialSetup();

    // The on-disk Localize cipher, distinct from live memory's DecryptString: an arithmetic XOR
    // keystream seeded by the on-disk EncryptionID byte, no DecryptionTable. Only 4 EncryptionID
    // values are known (unknown IDs return false); a rare 2-byte anomaly in some longer strings is unexplained.
    static bool DecryptDiskString(const uint8_t* Ciphertext, uint8_t Length, uint8_t EncryptionID, std::string& OutText);
    // Offline Localize export: scans a decompressed zone buffer for Localize headers
    // (Value==T8FollowsInline, pad==0) and decrypts each one via DecryptDiskString.
    static bool ExportDiskLocalize(ByteView Zone, const std::string& OutPath, std::string& Report);
    // A second header shape, 24 bytes: Value==T8FollowsInline(8) + Hash(8) + AlwaysZero(8), Hash/Pad
    // swapped from LocalizeEntry's on-disk order. The string that follows has no
    // [EncryptionID][StringSize] prefix and no cipher; likely T8XAssetType_RAWSTRING (157), unconfirmed.
    static bool ExportDiskRawStrings(ByteView Zone, const std::string& OutPath, std::string& Report);
};