#include "stdafx.h"

#include "VfxLayout.h"

// Modern Warfare III's particle system: MW2019's (Games/IW8/VfxLayout.cpp), its modules behind a
// pointer (module 0x20: data 0, type u16 0x12), names hashed. No module name table in this dump: ids
// paired with MW2019's by 212 effects both games hold (graywolf-tools docs/vfx-format.md), the rest
// by order, asset lists and data shapes. A module's data is MW2019's without its type word {flags,
// MW2019's 8 at 4, 0xc at 8, pad}, its fields at MW2019's offsets from 0x10; exceptions noted.
namespace MWIII
{
namespace
{
    using Vfx::AssetKind;
    using Vfx::Field;
    using T = Vfx::FieldType;

    Vfx::Layout Make()
    {
        Vfx::Layout L;
        L.Game = "mwiii";
        L.SystemName = -1, L.SystemEmitters = 8, L.SystemEmitterCount = 0x14;
        // ParticleSystemDef (0x80): MW2019's fields 8 or 0xc bytes earlier (the shared effects)
        L.SystemFields = { { "flags", 0x18, T::Hex32 }, { "drawFrustumCullRadius", 0x24, T::F32 }, { "sunDistance", 0x30, T::F32 } };
        // ParticleEmitterDef (0xc0): MW2019's at the same offsets up to 0x6c
        L.EmitterSize = 0xc0;
        L.EmitterFields = { { "spawnRate", 0xc, T::Range }, { "particleLife", 0x14, T::Range }, { "particleDelay", 0x1c, T::Range },
                            { "particleCountMax", 0x24, T::U32 }, { "burstCount", 0x28, T::IntRange }, { "emitterLife", 0x30, T::Range },
                            { "emitterDelay", 0x38, T::Range }, { "randomSeed", 0x40, T::I32 }, { "spawnRangeSq", 0x44, T::Range },
                            { "fadeOutMaxDistance", 0x4c, T::F32 }, { "fadeCurve", 0x50, T::Curve }, { "spawnFrustumCullRadius", 0x60, T::F32 },
                            { "flags", 0x64, T::Hex32 }, { "gravityOptions", 0x68, T::U32 } };
        L.StateFields = { { "flags", 0x10, T::Hex64 } };
        L.ModuleSize = 0x20, L.ModuleType = 0x12, L.ModuleData = 0, L.ModuleDataPointer = true, L.ModuleFlags = 0;
        // Material (0x78): the atlas rows, columns u8 0x1d, 0x1e (vfx_particulate_dust_sheet_1: 1 x 8,
        // MW2019's too)
        L.MaterialAtlasRows = 0x1d, L.MaterialAtlasCols = 0x1e;
        // element types by the init modules each state holds (walked zones; 15 with InitVolumetric),
        // Black Ops 6's order
        L.ElementTypes = { "billboardSprite", "beam", "decal", "dismember", "flare", "geoTrail", "", "lightOmni", "lightSpot", "",
                           "model", "orientedSprite", "runner", "tail", "vectorField", "volumetric" };

        const auto C = [](const char* N, int64_t O, int Count = 1) { return Field{ N, O, T::Curve, Count }; };
        const std::vector<Field> Test = { { "moduleIndex", 4, T::U16 } };
        const std::vector<Field> Shape = { { "axisFlags", 4, T::U8 }, { "spawnFlags", 5, T::U8 } };
        using Vfx::With;
        L.Modules = {
            // packed: MW2019's startFrame 8, loopCount 0xc, curves 0x20 4 bytes / 0x10 earlier
            { 0, "InitAtlas", 0x30, { { "startFrame", 4, T::I32 }, { "loopCount", 8, T::I32 }, { "playRate", 0x1c, T::F32 }, C("curves", 0x10, 2) } },
            { 1, "InitAttributes", 0x90, { { "nonUniformColor", 4, T::Bool }, { "nonUniformSize", 5, T::Bool }, { "sizeMin", 0x10, T::Vec4 },
                                          { "sizeMax", 0x20, T::Vec4 }, { "colorMin", 0x30, T::Vec4 }, { "colorMax", 0x40, T::Vec4 },
                                          { "velocityMin", 0x50, T::Vec4 }, { "velocityMax", 0x60, T::Vec4 } } },
            { 2, "InitBeam", 0x150, {}, AssetKind::Effect, 8 },
            { 3, "InitCameraOffset", 0x10, {} },
            { 4, "InitCloud", 0x10, {} },
            { 5, "InitDecal", 0x30, { { "lerpColor", 0x20, T::Vec4 } }, AssetKind::Decal, 0x10 },
            { 6, "InitDismember", 0x10, {} },
            { 7, "InitFlare", 0x90, { C("curves", 0x50, 4) } },
            { 8, "InitFXCompute", 0x40, {} },
            // packed: MW2019's fields 4 bytes earlier throughout
            { 9, "InitGeoTrail", 0x60, { { "numPointsMax", 4, T::U32 }, { "splitDistance", 8, T::F32 }, { "numSheets", 0x14, T::U32 },
                                        { "tileDistance", 0x20, T::F32 } } },
            { 11, "InitKillWrapBox", 0x30, {} },
            { 12, "InitLightOmni", 0x60, {}, AssetKind::LightDef, 8 },
            { 13, "InitLightSpot", 0x60, {}, AssetKind::LightDef, 8 },
            { 14, "InitMaterial", 0x110, {}, AssetKind::Material, 0x10 },
            { 15, "InitMayhem", 0x40, { C("curves", 0x10, 2) } },
            { 16, "InitMirrorTexture", 0x10, { { "mirrorHorizontal", 4, T::U32 }, { "mirrorVertical", 8, T::U32 } } },
            { 17, "InitModel", 0x20, { { "usePhysics", 4, T::Bool } }, AssetKind::Model, 0x10 },
            { 18, "InitOcclusionQuery", 0x30, {} },
            { 19, "InitOrientedSprite", 0x20, { { "orientationQuat", 0x10, T::Vec4 } } },
            { 20, "InitParticleSim", 0x20, {}, AssetKind::ParticleSim, 0x10 },
            { 21, "InitPivot", 0x30, {} },
            { 22, "InitPlayerFacing", 0x10, {} },
            // its velocity type counts differently (MW2019's 2, 3, 5 are 2, 4, 6 here): left out
            { 23, "InitRelativeVelocity", 0x10, {} },
            // packed: MW2019's angle 0x10, rate 0x18
            { 24, "InitRotation", 0x20, { { "angle", 4, T::Range }, { "rate", 0xc, T::Range } } },
            { 25, "InitRotation3D", 0x70, { { "angleMin", 0x10, T::Vec4 }, { "angleMax", 0x20, T::Vec4 }, { "rateMin", 0x30, T::Vec4 },
                                           { "rateMax", 0x40, T::Vec4 } } },
            { 26, "InitRunner", 0x60, {}, AssetKind::Effect, 0x50 },
            { 27, "InitScreenSpace", 0x10, {} },
            { 28, "InitSound", 0x20, {}, AssetKind::Sound, 0x10 },
            { 29, "InitSpawn", 0x20, { C("curves", 0x10) } },
            // MW2019's 0x40 on 0x10 earlier
            { 30, "InitSpawnShapeBox", 0xc0, With(Shape, { { "dimensionsMin", 0x30, T::Vec4 }, { "dimensionsMax", 0x40, T::Vec4 },
                                                           C("curves", 0x50, 6) }) },
            { 31, "InitSpawnShapeCylinder", 0xa0, With(Shape, { { "offset", 0x20, T::Vec4 }, { "hasRotation", 0x30, T::Bool },
                                                                { "halfHeight", 0x34, T::F32 }, { "radius", 0x38, T::Range },
                                                                { "directionQuat", 0x40, T::Vec4 }, C("curves", 0x50, 5) }) },
            { 32, "InitSpawnShapeEllipsoid", 0xb0, With(Shape, { { "offset", 0x20, T::Vec4 }, { "radiusMin", 0x30, T::Vec4 },
                                                                 { "radiusMax", 0x40, T::Vec4 }, C("curves", 0x50, 6) }) },
            { 33, "InitSpawnShapeMesh", 0x80, With(Shape, { C("curves", 0x50, 3) }), AssetKind::Model, 0x30 },
            { 34, "InitSpawnShapePointList", 0xb0, With(Shape, { C("curves", 0x50, 6) }) },
            { 35, "InitSpawnShapeSphere", 0x80, With(Shape, { { "offset", 0x20, T::Vec4 }, { "radius", 0x30, T::Range }, C("curves", 0x40, 4) }) },
            { 36, "InitTail", 0x20, { { "tailLeading", 8, T::Bool } } },
            { 37, "InitVectorField", 0x30, {}, AssetKind::VectorField, 0x20 },
            { 38, "InitVolumetric", 0x10, {} },
            { 40, "Attractor", 0x20, {} },
            { 41, "AttractorGraph", 0xb0, { C("curves", 0x10, 8) } },
            { 42, "ColorGraph", 0x90, { { "firstCurve", 4, T::Bool }, { "modulateColorByAlpha", 8, T::Bool }, C("curves", 0x10, 8) } },
            { 43, "ColorLerp", 0x30, { { "colorBegin", 0x10, T::Vec4 }, { "colorEnd", 0x20, T::Vec4 } } },
            { 44, "EmissionGraph", 0x30, { C("curves", 0x10, 2) } },
            { 45, "EmissiveGraph", 0xb0, { C("curves", 0x10, 10) } },
            { 46, "Force", 0x30, { { "forceMin", 0x10, T::Vec4 }, { "forceMax", 0x20, T::Vec4 } } },
            { 47, "ForceGraph", 0x90, { C("curves", 0x30, 6) } },
            { 48, "ForceDragGraph", 0x30, { C("curves", 0x10, 2) } },
            // in g, not MW2019's 800 in/s2 (its 0.2, 0.3 are 0.414, 0.622 here): the preview converts
            { 49, "Gravity", 0x10, { { "gravityG", 4, T::Range } } },
            { 50, "IntensityGraph", 0x30, { { "firstCurve", 4, T::Bool }, C("curves", 0x10, 2) } },
            { 51, "ParentVelocityGraph", 0x40, { C("curves", 0x10, 2) } },
            { 52, "PhysicsLight", 0x20, {}, AssetKind::Physics, 0x10 },
            { 53, "PhysicsRayCast", 0x30, { { "bounce", 4, T::Range }, { "boundsMidPoint", 0x10, T::Vec4 } } },
            { 54, "PositionGraph", 0x70, { C("curves", 0x10, 6) } },
            { 55, "RotationGraph", 0x30, { { "useRotationRate", 4, T::Bool }, C("curves", 0x10, 2) } },
            { 56, "RotationGraph3D", 0x70, { { "useRotationRate", 4, T::Bool }, C("curves", 0x10, 6) } },
            { 57, "ScaleByDistance", 0x30, { C("curves", 0x10, 2) } },
            { 58, "ShaderGraph", 0xd0, { C("curves", 0x10, 12) } },
            { 59, "SizeGraph", 0x90, { { "firstCurve", 4, T::Bool }, C("curves", 0x10, 6), { "sizeBegin", 0x70, T::Vec4 },
                                      { "sizeEnd", 0x80, T::Vec4 } } },
            { 60, "SizeLerp", 0x30, { { "sizeBegin", 0x10, T::Vec4 }, { "sizeEnd", 0x20, T::Vec4 } } },
            { 61, "TemperatureGraph", 0x30, { C("curves", 0x10, 2) } },
            { 62, "VelocityGraph", 0x90, { C("curves", 0x10, 6) } },
            { 63, "VectorFieldGraph", 0x30, { C("curves", 0x10, 2) } },
            { 64, "VectorFieldRMGraph", 0x30, { C("curves", 0x10, 2) } },
            { 65, "WaterAdvection", 0x50, { C("curves", 0x10, 4) } },
            // the effect list 8 bytes before MW2019's
            { 66, "TestAge", 0x80, Test, AssetKind::Effect, 0x50 },
            { 67, "TestBirth", 0x70, Test, AssetKind::Effect, 0x50 },
            { 68, "TestDeath", 0x70, Test, AssetKind::Effect, 0x50 },
            { 69, "TestGravity", 0x80, Test, AssetKind::Effect, 0x50 },
            { 70, "TestImpact", 0x80, Test, AssetKind::Effect, 0x50 },
            { 71, "TestPos", 0x80, Test, AssetKind::Effect, 0x50 },
            { 72, "TestTimeInState", 0x80, Test, AssetKind::Effect, 0x50 },
            { 73, "TestWater", 0x80, Test, AssetKind::Effect, 0x50 },
        };
        return L;
    }
}

const Vfx::Layout& VfxLayout()
{
    static const Vfx::Layout L = Make();
    return L;
}
}
