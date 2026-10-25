#pragma once

#include <cstdint>
#include <memory>

class CoDAnim_t;
class CoDModel_t;
struct XImage_t;
struct XMaterial_t;
struct XModel_t;
struct XAnim_t;
struct XImageDDS;

class GameBlackOps
{
public:
    static std::unique_ptr<XAnim_t> ReadXAnim(const CoDAnim_t* Animation);
    static std::unique_ptr<XModel_t> ReadXModel(const CoDModel_t* Model);

    static std::unique_ptr<XImageDDS> LoadXImage(const XImage_t& Image);

    // Image table, plus a blendMode setting derived from its GfxStateBits
    static const XMaterial_t ReadXMaterial(uint64_t MaterialPointer);

    // The blendMode setting off a material's GfxStateBits table (Count entries; none when empty): the
    // src/dst RGB blend factors, bits 0-3/4-7 of the first entry's loadBits (the base pass). Black
    // Ops II's GfxStateBits starts as Black Ops'.
    static void AddBlendMode(XMaterial_t& Material, uint64_t StateBits, uint32_t Count);
};