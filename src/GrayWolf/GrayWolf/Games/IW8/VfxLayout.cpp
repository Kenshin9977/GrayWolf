#include "stdafx.h"

#include "VfxLayout.h"

// Modern Warfare 2019's particle system, Infinite Warfare's one engine later. Levels and sizes: the
// iw8 tables (vfx 0x40 its name at 0, emitter f2d9d3f0 0xa0, state f2d9f4f0 0x20, group f2d9d990
// 0x10, module 0xf0 its type u16 at 0 and its data at 0x10, linked asset f2d9d6a0_e1 0x20, curves
// f2d9d320 / f2d9d2a0 0x10). Module ids, names and data sizes: the exe's module table (iw8_flat
// 0x74c7190, 0x30 per type, its name at +0x28). Fields: IW7's (Games/IW7/VfxLayout.cpp) where 228
// effects both games hold under one name pair the module types one to one and show the field at the
// offset given here (IW7 -> IW8 field moves read off those pairs; graywolf-tools docs/vfx-format.md);
// curve and asset list offsets from the tables. A field with no pair to show it is left out.
namespace IW8
{
namespace
{
    using Vfx::AssetKind;
    using Vfx::Field;
    using T = Vfx::FieldType;

    Vfx::Layout Make()
    {
        Vfx::Layout L;
        L.Game = "iw8";
        L.SystemName = 0, L.SystemEmitters = 8, L.SystemEmitterCount = 0x1c;
        L.SystemFields = { { "version", 0x18, T::I32 }, { "flags", 0x24, T::Hex32 }, { "drawFrustumCullRadius", 0x30, T::F32 },
                           { "sunDistance", 0x38, T::F32 } };
        // ParticleEmitterDef (0xa0): IW7's 0x90 byte for byte up to 0x6c on the shared effects; the
        // fade curve at 0x50 (the tables'), a new u32 at 0x90
        L.EmitterSize = 0xa0;
        L.EmitterFields = { { "spawnRate", 0xc, T::Range }, { "particleLife", 0x14, T::Range }, { "particleDelay", 0x1c, T::Range },
                            { "particleCountMax", 0x24, T::U32 }, { "burstCount", 0x28, T::IntRange }, { "emitterLife", 0x30, T::Range },
                            { "emitterDelay", 0x38, T::Range }, { "randomSeed", 0x40, T::I32 }, { "spawnRangeSq", 0x44, T::Range },
                            { "fadeOutMaxDistance", 0x4c, T::F32 }, { "fadeCurve", 0x50, T::Curve }, { "spawnFrustumCullRadius", 0x60, T::F32 },
                            { "flags", 0x64, T::Hex32 }, { "gravityOptions", 0x68, T::U32 }, { "groupIds", 0x6c, T::I32, 4 } };
        L.StateFields = { { "flags", 0x10, T::Hex64 } };
        L.ModuleSize = 0xf0, L.ModuleType = 0, L.ModuleData = 0x10;
        // element types by the init modules each state holds (walked zones: InitBeam only in 1, InitDecal
        // in 2, InitFlare 3, InitGeoTrail 4, InitLightOmni 5, InitLightSpot 6, InitModel 7,
        // InitOrientedSprite 8, InitRunner 9, InitTail 10, InitVectorField 11, InitDismember's count 13)
        L.ElementTypes = { "billboardSprite", "beam", "decal", "flare", "geoTrail", "lightOmni", "lightSpot", "model",
                           "orientedSprite", "runner", "tail", "vectorField", "volumetric", "dismember" };

        const auto C = [](const char* N, int64_t O, int Count = 1) { return Field{ N, O, T::Curve, Count }; };
        const std::vector<Field> Test = { { "moduleIndex", 8, T::U16 }, { "kill", 0x68, T::Bool } };
        const std::vector<Field> Shape = { { "axisFlags", 8, T::U8 }, { "spawnFlags", 9, T::U8 } };
        using Vfx::With;
        L.Modules = {
            // the fps is now two curves whose scale is it (IW7's playRate 30 -> scale 30)
            { 0, "InitAtlas", 0x40, { { "startFrame", 8, T::I32 }, { "loopCount", 0xc, T::I32 }, { "playRate", 0x2c, T::F32 }, C("curves", 0x20, 2) } },
            { 1, "InitAttributes", 0x70, { { "nonUniformColor", 8, T::Bool }, { "nonUniformSize", 9, T::Bool }, { "sizeMin", 0x10, T::Vec4 },
                                          { "sizeMax", 0x20, T::Vec4 }, { "colorMin", 0x30, T::Vec4 }, { "colorMax", 0x40, T::Vec4 },
                                          { "velocityMin", 0x50, T::Vec4 }, { "velocityMax", 0x60, T::Vec4 } } },
            { 2, "InitBeam", 0x80, {}, AssetKind::Effect, 0x60 },
            { 3, "InitCameraOffset", 0x10, {} },
            { 4, "InitCloud", 0x10, {} },
            { 5, "InitDecal", 0x40, { { "fadeInTime", 8, T::U16 }, { "lerpColor", 0x20, T::Vec4 } }, AssetKind::Decal, 0x10 },
            { 6, "InitFlare", 0x90, { C("curves", 0x50, 4) } },
            { 7, "InitGeoTrail", 0x40, { { "numPointsMax", 8, T::U32 }, { "splitDistance", 0xc, T::F32 }, { "numSheets", 0x18, T::U32 },
                                        { "tileDistance", 0x24, T::F32 } } },
            { 8, "InitKillWrapBox", 0x30, {} },
            { 9, "InitLightOmni", 0x50, {}, AssetKind::LightDef, 8 },
            { 10, "InitLightSpot", 0x60, {}, AssetKind::LightDef, 8 },
            { 11, "InitMaterial", 0xe0, {}, AssetKind::Material, 0x10 },
            { 12, "InitMirrorTexture", 0x10, { { "mirrorHorizontal", 8, T::U32 }, { "mirrorVertical", 0xc, T::U32 } } },
            { 13, "InitModel", 0x20, { { "usePhysics", 8, T::Bool } }, AssetKind::Model, 0x10 },
            { 14, "InitOcclusionQuery", 0x30, {} },
            { 15, "InitOrientedSprite", 0x20, { { "orientationQuat", 0x10, T::Vec4 } } },
            { 16, "InitParticleSim", 0x30, {}, AssetKind::ParticleSim, 8 },
            { 17, "InitPivot", 0x30, {} },
            { 18, "InitPlayerFacing", 0x10, {} },
            { 19, "InitRelativeVelocity", 0x10, { { "velocityType", 8, T::U32 }, { "useBoltInfo", 0xc, T::Bool } } },
            { 20, "InitRotation", 0x20, { { "angle", 0x10, T::Range }, { "rate", 0x18, T::Range } } },
            { 21, "InitRotation3D", 0x50, { { "angleMin", 0x10, T::Vec4 }, { "angleMax", 0x20, T::Vec4 }, { "rateMin", 0x30, T::Vec4 },
                                           { "rateMax", 0x40, T::Vec4 } } },
            { 22, "InitRunner", 0x70, {}, AssetKind::Effect, 0x50 },
            { 23, "InitSound", 0x20, {}, AssetKind::Sound, 0x10 },
            { 24, "InitSpawn", 0x20, { C("curves", 0x10) } },
            // the shapes moved their offset to 0x20 and their extents 0x10 on (IW7 box 0x30/0x40, cylinder
            // 0x24-0x30, sphere 0x28): the shared effects' values found there
            { 25, "InitSpawnShapeBox", 0xc0, With(Shape, { { "dimensionsMin", 0x40, T::Vec4 }, { "dimensionsMax", 0x50, T::Vec4 },
                                                           C("curves", 0x60, 6) }) },
            { 26, "InitSpawnShapeCylinder", 0xa0, With(Shape, { { "offset", 0x20, T::Vec4 }, { "hasRotation", 0x30, T::Bool },
                                                                { "halfHeight", 0x34, T::F32 }, { "radius", 0x38, T::Range },
                                                                { "directionQuat", 0x40, T::Vec4 }, C("curves", 0x50, 5) }) },
            { 27, "InitSpawnShapeEllipsoid", 0xb0, With(Shape, { { "offset", 0x20, T::Vec4 }, { "radiusMin", 0x30, T::Vec4 },
                                                                 { "radiusMax", 0x40, T::Vec4 }, C("curves", 0x50, 6) }) },
            { 28, "InitSpawnShapeMesh", 0x80, With(Shape, { C("curves", 0x50, 3) }), AssetKind::Model, 0x38 },
            { 29, "InitSpawnShapeSphere", 0x80, With(Shape, { { "offset", 0x20, T::Vec4 }, { "radius", 0x38, T::Range }, C("curves", 0x40, 4) }) },
            { 30, "InitTail", 0x20, { { "tailLeading", 0xc, T::Bool } } },
            { 31, "InitVectorField", 0x30, {}, AssetKind::VectorField, 8 },
            { 32, "InitVolumetric", 0x20, {} },
            { 33, "InitDismember", 0x10, {} },
            { 34, "Attractor", 0x30, {} },
            { 35, "AttractorGraph", 0xc0, { C("curves", 0x30, 8) } },
            { 36, "ColorGraph", 0x90, { { "firstCurve", 8, T::Bool }, { "modulateColorByAlpha", 0xc, T::Bool }, C("curves", 0x10, 8) } },
            { 37, "ColorLerp", 0x30, { { "colorBegin", 0x10, T::Vec4 }, { "colorEnd", 0x20, T::Vec4 } } },
            { 38, "EmissionGraph", 0x30, { C("curves", 0x10, 2) } },
            { 39, "EmissiveGraph", 0xb0, { C("curves", 0x10, 10) } },
            { 40, "ShaderGraph", 0xd0, { C("curves", 0x10, 12) } },
            { 41, "Force", 0x30, { { "forceMin", 0x10, T::Vec4 }, { "forceMax", 0x20, T::Vec4 } } },
            { 42, "ForceGraph", 0x90, { C("curves", 0x30, 6) } },
            { 43, "ForceDragGraph", 0x30, { C("curves", 0x10, 2) } },
            { 44, "Gravity", 0x10, { { "gravity", 8, T::Range } } },
            { 45, "IntensityGraph", 0x30, { { "firstCurve", 8, T::Bool }, C("curves", 0x10, 2) } },
            { 46, "TemperatureGraph", 0x30, { C("curves", 0x10, 2) } },
            { 47, "ParentVelocityGraph", 0x30, { C("curves", 0x10, 2) } },
            { 48, "PhysicsLight", 0x20, {}, AssetKind::Physics, 0x10 },
            { 49, "PhysicsRayCast", 0x30, { { "bounce", 8, T::Range }, { "boundsMidPoint", 0x10, T::Vec4 }, { "useItemClip", 0x28, T::Bool },
                                           { "useSurfaceType", 0x29, T::Bool }, { "collideWithWater", 0x2a, T::Bool },
                                           { "ignoreContentItem", 0x2b, T::Bool } } },
            { 50, "PositionGraph", 0x70, { C("curves", 0x10, 6) } },
            { 51, "RotationGraph", 0x30, { { "useRotationRate", 8, T::Bool }, C("curves", 0x10, 2) } },
            { 52, "RotationGraph3D", 0x70, { { "useRotationRate", 8, T::Bool }, C("curves", 0x10, 6) } },
            { 53, "ScaleByDistance", 0x40, { C("curves", 0x10, 2) } },
            { 54, "SizeGraph", 0x90, { { "firstCurve", 8, T::Bool }, C("curves", 0x10, 6), { "sizeBegin", 0x70, T::Vec4 },
                                      { "sizeEnd", 0x80, T::Vec4 } } },
            { 55, "SizeLerp", 0x30, { { "sizeBegin", 0x10, T::Vec4 }, { "sizeEnd", 0x20, T::Vec4 } } },
            { 56, "VelocityGraph", 0x90, { C("curves", 0x10, 6), { "velocityBegin", 0x70, T::Vec4 }, { "velocityEnd", 0x80, T::Vec4 } } },
            { 57, "VectorFieldGraph", 0x30, { C("curves", 0x10, 2) } },
            { 58, "TestAge", 0x80, Test, AssetKind::Effect, 0x58 },
            { 59, "TestBirth", 0x70, Test, AssetKind::Effect, 0x58 },
            { 60, "TestDeath", 0x70, Test, AssetKind::Effect, 0x58 },
            { 61, "TestGravity", 0x80, Test, AssetKind::Effect, 0x58 },
            { 62, "TestImpact", 0x80, Test, AssetKind::Effect, 0x58 },
            { 63, "TestPos", 0x80, Test, AssetKind::Effect, 0x58 },
            { 64, "TestTimeInState", 0x80, Test, AssetKind::Effect, 0x58 },
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
