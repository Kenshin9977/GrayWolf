#pragma once

// The normal maps of the DXT5nm games: CoD4, World at War, Black Ops, CoD Online, MW2 and MW3 keep
// a normal map's X in alpha and its Y in green (DXT5, red and blue copies of green or unused).
// Their format says nothing (DXT5 holds colour and specular maps too); the GfxImage's semantic
// does: TS_NORMAL_MAP (5) on every image their materials use as normalMap and on no other.
// Measured 2026-11-12 over mp_shipment (CoD4, 48), mp_dome (WaW, 37 + the code image
// $identitynormalmap, semantic 0, a flat grey), mp_nuked (BO1, 37), firingrange (CODOL, 41),
// so_crossing_so_bridge (MW2, 31) and so_ied_berlin (MW3, 79).

#include "CoDAssetType.h"
#include "MaterialDescription.h"
#include "SettingsManager.h"

#include <cstdint>

namespace StoredNormals
{
    constexpr uint8_t NormalMapSemantic = 5;   // TS_NORMAL_MAP

    // When "patchnormals" is on, an image of the normal map semantic is written as X, Y and its Z
    // (ImagePatch::Normal_AG): true when it is
    inline bool Patch(XImageDDS* Dds, uint8_t Semantic)
    {
        if (Dds == nullptr || Semantic != NormalMapSemantic || SettingsManager::GetSetting("patchnormals", "true") != "true") return false;
        Dds->ImagePatchType = ImagePatch::Normal_AG;
        return true;
    }

    // A material sidecar's normal as the file is written: "xy" once Patch made it X, Y and Z (the
    // live readers' Normal_Bumpmap too), "ag" (as stored) when "patchnormals" is off
    class Adapter final : public Materials::IAdapter
    {
    public:
        explicit Adapter(const Materials::IAdapter& Inner) : Inner(Inner) {}
        const char* Name() const override { return Inner.Name(); }
        Materials::Description Describe(const Materials::Source& M) const override
        {
            Materials::Description D = Inner.Describe(M);
            if (!D.Normal.Empty())
                D.NormalEnc = SettingsManager::GetSetting("patchnormals", "true") == "true" ? Materials::NormalEncoding::XY : Materials::NormalEncoding::AG;
            return D;
        }
    private:
        const Materials::IAdapter& Inner;
    };
}
