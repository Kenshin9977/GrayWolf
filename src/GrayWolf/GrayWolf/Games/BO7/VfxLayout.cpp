#include "stdafx.h"

#include "VfxLayout.h"

#include "Games/BO6/VfxLayout.h"

#include <cstring>

// Black Ops 7's particle system: Black Ops 6's levels and module data by name (the bo7 tables, module data
// switch f387bbc0, read the same; the init and update functions read the same offsets), the ids of its own
// exe's name table (bo7_flat 0xdc22e60, char*[81]) and its descriptors' sizes (0xdc230f0). Linked asset
// entries 0x10; a decal names one material; InitLightOmni / InitLightSpot hold nothing (InitVfxLight).
namespace BO7
{
const Vfx::Layout& VfxLayout()
{
    static const std::vector<const char*> Names = {
        "InitAtlas", "InitAttributes", "InitBeam", "InitCameraOffset", "InitDecal", "InitDismember", "InitFlare", "InitFXCompute",
        "InitGeoTrail", "InitGPURibbonGeneric", "InitGPURibbonGenerator", "InitGPURibbon", "InitHudOutlineVFX", "InitKillWrapBox",
        "InitLightOmni", "InitLightSpot", "InitVfxLight", "InitMaterial", "InitMayhem", "InitMirrorTexture", "InitModel",
        "InitOcclusionQuery", "InitOrientedSprite", "InitParticleSim", "InitPivot", "InitPlayerFacing", "InitRelativeVelocity",
        "InitRotation", "InitRotation3D", "InitRunner", "InitScreenSpace", "InitSound", "InitSpawn", "InitSpawnShapeBox",
        "InitSpawnShapeCylinder", "InitSpawnShapeEllipsoid", "InitSpawnShapeMesh", "InitSpawnShapePointList", "InitSpawnShapeSphere",
        "InitSpawnShapeWeather", "InitSpriteSizeLimits", "InitTail", "InitVectorField", "InitVolumetric", "InitWeather", "Attractor",
        "AttractorGraph", "BlurGraph", "ColorGraph", "ColorLerp", "EmissionGraph", "EmissiveGraph", "Force", "ForceDragGraph",
        "ForceGraph", "Gravity", "IntensityGraph", "ParentVelocityGraph", "PhysicsLight", "PhysicsRayCast", "PositionGraph",
        "RotationGraph", "RotationGraph3D", "ScaleByDistance", "ShaderGraph", "SizeGraph", "SizeLerp", "TemperatureGraph",
        "TransmittanceGraph", "VectorFieldGraph", "VectorFieldRMGraph", "VelocityGraph", "WaterAdvection", "TestAge", "TestBirth",
        "TestDeath", "TestGravity", "TestImpact", "TestPos", "TestTimeInState", "TestWater",
    };
    static const Vfx::Layout L = [] {
        Vfx::Layout V = BO6::BlackOpsVfxLayout("bo7", Names, 0x10);
        // element 8 the one light module (InitVfxLight, in all 121 of its states on mp_sat_rift)
        V.ElementTypes[7] = "";
        V.ElementTypes[8] = "vfxLight";
        // a material's atlas: rows u8 0x70, columns u8 0x71 (1 to 8 on the materials an InitMaterial names)
        V.MaterialAtlasRows = 0x70, V.MaterialAtlasCols = 0x71;
        // the data that grew or changed (the tables, the exe's descriptors)
        for (Vfx::Module& M : V.Modules)
        {
            const auto Is = [&](const char* N) { return std::strcmp(M.Name, N) == 0; };
            if (Is("InitDecal")) M.Assets = Vfx::AssetKind::Material;
            else if (Is("InitLightOmni") || Is("InitLightSpot")) M.Size = 0xb0, M.Assets = Vfx::AssetKind::None, M.AssetList = -1;
            else if (Is("InitDismember")) M.Size = 0x20;
            else if (Is("InitFlare")) M.Size = 0x80;
            else if (Is("InitFXCompute")) M.Size = 0x60;
            else if (Is("InitGeoTrail")) M.Size = 0x30;
            else if (Is("InitMaterial")) M.Size = 0x120;
            else if (Is("InitRelativeVelocity")) M.Size = 0x20;
            else if (Is("VectorFieldGraph"))
            {
                M.Size = 0x40;
                M.Fields = { Vfx::Field{ "curves", 0x18, Vfx::FieldType::CurveArray, 7, 0x20 } };
            }
        }
        return V;
    }();
    return L;
}
}
