#include "stdafx.h"

#include "VfxLayout.h"


#include <map>
#include <string>

// Black Ops 6's particle system; the facts and their proofs: graywolf-tools docs/vfx-format.md (Black Ops 6/7).
// Levels and sizes: the bo6 tables (particlesystem f2854df0, module data switch f2852ba0). Ids and names: the
// exe's name table (bo6_flat 0xb303c00), sizes its descriptors after it (0xb303e70). Fields: the init and update
// functions those descriptors name, decompiled (addresses below), else the data holding IW7's values in place.
// The data has no type word: flags u32 at 0 (IW7's bits), then the module's own fields; a graph points at its
// curves (one pointer) and keeps their scales right after it.
namespace BO6
{
namespace
{
    using Vfx::AssetKind;
    using Vfx::Field;
    using T = Vfx::FieldType;

    // Count curves behind the pointer at O, their scales after it (Scale: false, none)
    Field Curves(int64_t O, int Count, bool Scale = true) { return Field{ "curves", O, T::CurveArray, Count, Scale ? O + 8 : -1 }; }

    struct Def
    {
        int64_t Size = 0;
        std::vector<Field> Fields;
        AssetKind Assets = AssetKind::None;
        int64_t AssetList = -1;
    };

    // every module of Black Ops 6 and 7 by name (Black Ops 6's sizes)
    const std::map<std::string, Def>& Defs()
    {
        // the shapes' first bytes (axis filter '?' all, spawn flags: 2 the curves over the emitter's life,
        // 4 world units, 8 a fixed random), their volume's cube root, the shared offset (0x8513070)
        static const std::vector<Field> Shape = { { "axisFlags", 4, T::U8 }, { "spawnFlags", 5, T::U8 }, { "normalAxis", 6, T::U8 },
                                                  { "spawnType", 7, T::U8 }, { "volumeCubeRoot", 8, T::F32 }, { "offset", 0x20, T::Vec4 } };
        using Vfx::With;
        static const std::map<std::string, Def> D = {
            // startFrame copied per particle, a random one when randomStartFrame (0x8514340); the play rate the
            // first curve's scale (frames per second: 30, 50, 60)
            { "InitAtlas", { 0x30, { { "startFrame", 4, T::I32 }, { "loopCount", 8, T::I32 }, { "randomStartFrame", 0xc, T::Bool },
                                     Curves(0x18, 2), { "playRate", 0x20, T::F32 } } } },
            // size (particle stream 8), colour (stream 2) and velocity (0x8513f50) lerped per particle, one random
            // or one per channel (0x8514600); sizes then times a random entry of the Vec4 list at 0x70
            { "InitAttributes", { 0x90, { { "nonUniformColor", 4, T::Bool }, { "nonUniformSize", 5, T::Bool }, { "sizeMin", 0x10, T::Vec4 },
                                          { "sizeMax", 0x20, T::Vec4 }, { "colorMin", 0x30, T::Vec4 }, { "colorMax", 0x40, T::Vec4 },
                                          { "velocityMin", 0x50, T::Vec4 }, { "velocityMax", 0x60, T::Vec4 } } } },
            { "InitBeam", { 0x160, { Curves(0xd0, 1, false) }, AssetKind::Effect, 8 } },
            { "InitCameraOffset", { 0x10 } },
            { "InitCloud", { 0x10 } },
            { "InitDecal", { 0x30, {}, AssetKind::Decal, 0x10 } },
            { "InitDismember", { 0x10 } },
            { "InitFlare", { 0x70, { Curves(0x50, 4) } } },
            { "InitFXCompute", { 0x50 } },
            { "InitGeoTrail", { 0x40 } },
            { "InitGPURibbon", { 0x50 } },
            { "InitGPURibbonGeneric", { 0x10 } },
            { "InitGPURibbonGenerator", { 0x10 } },
            { "InitHudOutlineVFX", { 0x20, { Curves(0x18, 1, false) } } },
            { "InitKillWrapBox", { 0x30 } },
            { "InitLightOmni", { 0x70, {}, AssetKind::LightDef, 8 } },
            { "InitLightSpot", { 0x80, {}, AssetKind::LightDef, 8 } },
            { "InitVfxLight", { 0xb0 } },
            { "InitMaterial", { 0x110, {}, AssetKind::Material, 0x10 } },
            { "InitMayhem", { 0x30, { Curves(8, 2) } } },
            { "InitMirrorTexture", { 0x10 } },
            { "InitModel", { 0x20, {}, AssetKind::Model, 0x10 } },
            { "InitMotionBlur", { 0x10 } },
            { "InitOcclusionQuery", { 0x30 } },
            { "InitOrientedSprite", { 0x20, { { "orientationQuat", 0x10, T::Vec4 } } } },
            { "InitParticleSim", { 0x20, {}, AssetKind::ParticleSim, 0x10 } },
            { "InitPivot", { 0x30 } },
            { "InitPlayerFacing", { 0x10 } },
            { "InitRelativeVelocity", { 0x10, { { "velocityType", 4, T::U32 } } } },
            // angle (stream 3) and rate (stream 4), radians (0x8514e80)
            { "InitRotation", { 0x20, { { "angle", 4, T::Range }, { "rate", 0xc, T::Range } } } },
            { "InitRotation3D", { 0x70, { { "angleMin", 0x10, T::Vec4 }, { "angleMax", 0x20, T::Vec4 }, { "rateMin", 0x30, T::Vec4 },
                                          { "rateMax", 0x40, T::Vec4 }, Curves(0x50, 6, false) } } },
            { "InitRunner", { 0x60, {}, AssetKind::Effect, 0x50 } },
            { "InitScreenSpace", { 0x10 } },
            { "InitSound", { 0x20 } },
            { "InitSpawn", { 0x20, { Curves(0x10, 1) } } },
            // the dimensions, or the 6 curves when spawnFlags & 2 (0x85125a0)
            { "InitSpawnShapeBox", { 0x60, With(Shape, { { "dimensionsMin", 0x30, T::Vec4 }, { "dimensionsMax", 0x40, T::Vec4 },
                                                         Curves(0x50, 6, false), { "useBeamInfo", 0x58, T::Bool } }) } },
            // the radius and half height, or curves 3 and 4 (0x85128f0); the direction when hasRotation
            { "InitSpawnShapeCylinder", { 0x60, With(Shape, { { "hasRotation", 0x30, T::U32 }, { "halfHeight", 0x34, T::F32 },
                                                              { "radius", 0x38, T::Range }, { "directionQuat", 0x40, T::Vec4 },
                                                              Curves(0x50, 5, false) }) } },
            { "InitSpawnShapeEllipsoid", { 0x60, With(Shape, { { "radiusMin", 0x30, T::Vec4 }, { "radiusMax", 0x40, T::Vec4 },
                                                               Curves(0x50, 6, false) }) } },
            { "InitSpawnShapeMesh", { 0x60, With(Shape, { Curves(0x50, 3, false) }), AssetKind::Model, 0x30 } },
            { "InitSpawnShapePointList", { 0x70 } },
            // the radius, or curve 3 (0x8512d10)
            { "InitSpawnShapeSphere", { 0x40, With(Shape, { { "radius", 0x30, T::Range }, Curves(0x38, 4, false) }) } },
            { "InitSpawnShapeWeather", { 0x30 } },
            { "InitSpriteSizeLimits", { 0x10 } },
            { "InitTail", { 0x20 } },
            { "InitVectorField", { 0x20, {}, AssetKind::VectorField, 0x10 } },
            { "InitVolumetric", { 0x90 } },
            { "InitWeather", { 0x10 } },
            { "Attractor", { 0x20 } },
            { "AttractorGraph", { 0x60, { Curves(0x10, 8) } } },
            { "BlurGraph", { 0x20, { Curves(8, 4) } } },
            // rgba, a second rgba picked between under flag 0x10 (0x8534a20 -> 0x852d0d0, 0x852cbe0)
            { "ColorGraph", { 0x30, { Curves(8, 8) } } },
            { "ColorLerp", { 0x30, { { "colorBegin", 0x10, T::Vec4 }, { "colorEnd", 0x20, T::Vec4 } } } },
            { "EmissionGraph", { 0x20, { Curves(8, 2) } } },
            { "EmissiveGraph", { 0x40, { Curves(8, 10) } } },
            { "TransmittanceGraph", { 0x20, { Curves(8, 2) } } },
            { "Force", { 0x30, { { "forceMin", 0x10, T::Vec4 }, { "forceMax", 0x20, T::Vec4 } } } },
            { "ForceGraph", { 0x50, { Curves(0x30, 6) } } },
            { "ForceDragGraph", { 0x20, { Curves(8, 2) } } },
            // a random lerp, times dt and the world's gravity, added to the velocity (0x8535a20)
            { "Gravity", { 0x10, { { "gravity", 4, T::Range } } } },
            { "IntensityGraph", { 0x20, { Curves(8, 2) } } },
            { "ParentVelocityGraph", { 0x30, { Curves(0x18, 2) } } },
            { "PhysicsLight", { 0x20, {}, AssetKind::Physics, 0x10 } },
            { "PhysicsRayCast", { 0x30 } },
            { "PositionGraph", { 0x30, { Curves(8, 6) } } },
            { "RotationGraph", { 0x20, { { "useRotationRate", 4, T::Bool }, Curves(8, 2) } } },
            { "RotationGraph3D", { 0x30, { Curves(8, 6) } } },
            { "ScaleByDistance", { 0x30, { Curves(0x18, 2) } } },
            { "ShaderGraph", { 0x40, { Curves(8, 12) } } },
            { "SizeGraph", { 0x50, { Curves(8, 6), { "sizeBegin", 0x30, T::Vec4 }, { "sizeEnd", 0x40, T::Vec4 } } } },
            { "SizeLerp", { 0x30, { { "sizeBegin", 0x10, T::Vec4 }, { "sizeEnd", 0x20, T::Vec4 } } } },
            { "TemperatureGraph", { 0x20, { Curves(8, 2) } } },
            { "VelocityGraph", { 0x50, { Curves(8, 6), { "velocityBegin", 0x30, T::Vec4 }, { "velocityEnd", 0x40, T::Vec4 } } } },
            { "VectorFieldGraph", { 0x30, { Curves(0x18, 3) } } },
            { "VectorFieldRMGraph", { 0x20, { Curves(8, 2) } } },
            { "WaterAdvection", { 0x30, { Curves(0x18, 4) } } },
            { "TestAge", { 0x80, {}, AssetKind::Effect, 0x50 } },
            { "TestBirth", { 0x70, {}, AssetKind::Effect, 0x50 } },
            { "TestDeath", { 0x70, {}, AssetKind::Effect, 0x50 } },
            { "TestGravity", { 0x80, {}, AssetKind::Effect, 0x50 } },
            { "TestImpact", { 0x80, {}, AssetKind::Effect, 0x50 } },
            { "TestPos", { 0x80, {}, AssetKind::Effect, 0x50 } },
            { "TestTimeInState", { 0x80, {}, AssetKind::Effect, 0x50 } },
            { "TestWater", { 0x80, {}, AssetKind::Effect, 0x50 } },
        };
        return D;
    }
}

Vfx::Layout BlackOpsVfxLayout(const char* Game, const std::vector<const char*>& Names, int64_t AssetSize)
{
    Vfx::Layout L;
    L.Game = Game;
    // particlesystem 0x80: its name a 63-bit hash at 0 (the caller names it), emitters 8, count i32 0x14
    L.SystemEmitters = 8, L.SystemEmitterCount = 0x14;
    // emitter 0x70: states 8, count u32 0x18. spawnRate lerped near to far by the spawn distance (0x84f7600),
    // particleLife read at spawn (0x8546590), randomSeed (the shapes' (seed + i * 0x277f + ...) % 0x3fe9), flags
    // (IW7's bits); particleCountMax, particleDelay, spawnRangeSq by their values. Through the pointer at 0: the
    // burst count, the emitter's life and its delay (tested when it restarts, 0x84f62b0), a curve.
    L.EmitterSize = 0x70, L.EmitterStates = 8, L.EmitterStateCount = 0x18;
    L.EmitterFields = { { "spawnRate", 0x10, T::Range }, { "particleCountMax", 0x1c, T::U32 }, { "particleLife", 0x20, T::Range },
                        { "particleDelay", 0x28, T::Range }, { "spawnRangeSq", 0x30, T::Range }, { "randomSeed", 0x38, T::I32 },
                        { "flags", 0x40, T::Hex32 },
                        { "burstCount", 0, T::IntRange, 1, -1, 0 }, { "emitterLife", 8, T::Range, 1, -1, 0 },
                        { "emitterDelay", 0x10, T::Range, 1, -1, 0 }, { "fadeCurve", 0x20, T::Curve, 1, -1, 0 } };
    // state 0x20: the 3 groups at 0 (init, update, test; 0x10 each: modules 0, count 8), the element type u32 8
    L.StateSize = 0x20, L.StateGroups = 0, L.StateElementType = 8;
    L.GroupSize = 0x10, L.GroupModules = 0, L.GroupModuleCount = 8;
    L.ModuleSize = 0x20, L.ModuleType = 0x12, L.ModuleData = 0, L.ModuleDataPointer = true, L.ModuleFlags = 0;
    L.AssetSize = AssetSize;
    // element types, each proven by the Init module all of its states hold (InitDecal, InitFlare, ...); ids no
    // zone showed are written as numbers
    L.ElementTypes = { "billboardSprite", "", "decal", "", "flare", "geoTrail", "", "lightOmni", "lightSpot", "",
                       "model", "orientedSprite", "runner", "tail", "vectorField" };
    const auto& D = Defs();
    for (size_t Id = 0; Id < Names.size(); Id++)
    {
        const auto It = D.find(Names[Id]);
        if (It == D.end()) continue;
        Vfx::Module M;
        M.Id = (int)Id;
        M.Name = It->first.c_str();
        M.Size = It->second.Size;
        M.Fields = It->second.Fields;
        M.Assets = It->second.Assets;
        M.AssetList = It->second.AssetList;
        L.Modules.push_back(std::move(M));
    }
    return L;
}

const Vfx::Layout& VfxLayout()
{
    // the exe's module names by type id
    static const std::vector<const char*> Names = {
        "InitAtlas", "InitAttributes", "InitBeam", "InitCameraOffset", "InitCloud", "InitDecal", "InitDismember", "InitFlare",
        "InitFXCompute", "InitGeoTrail", "InitGPURibbon", "InitHudOutlineVFX", "InitKillWrapBox", "InitLightOmni", "InitLightSpot",
        "InitMaterial", "InitMayhem", "InitMirrorTexture", "InitModel", "InitMotionBlur", "InitOcclusionQuery", "InitOrientedSprite",
        "InitParticleSim", "InitPivot", "InitPlayerFacing", "InitRelativeVelocity", "InitRotation", "InitRotation3D", "InitRunner",
        "InitScreenSpace", "InitSound", "InitSpawn", "InitSpawnShapeBox", "InitSpawnShapeCylinder", "InitSpawnShapeEllipsoid",
        "InitSpawnShapeMesh", "InitSpawnShapePointList", "InitSpawnShapeSphere", "InitSpawnShapeWeather", "InitSpriteSizeLimits",
        "InitTail", "InitVectorField", "InitVolumetric", "Attractor", "AttractorGraph", "ColorGraph", "ColorLerp", "EmissionGraph",
        "EmissiveGraph", "TransmittanceGraph", "Force", "ForceGraph", "ForceDragGraph", "Gravity", "IntensityGraph",
        "ParentVelocityGraph", "PhysicsLight", "PhysicsRayCast", "PositionGraph", "RotationGraph", "RotationGraph3D",
        "ScaleByDistance", "ShaderGraph", "SizeGraph", "SizeLerp", "TemperatureGraph", "VelocityGraph", "VectorFieldGraph",
        "VectorFieldRMGraph", "WaterAdvection", "TestAge", "TestBirth", "TestDeath", "TestGravity", "TestImpact", "TestPos",
        "TestTimeInState", "TestWater",
    };
    static const Vfx::Layout L = [] {
        Vfx::Layout V = BlackOpsVfxLayout("bo6", Names, 0x20);
        // a material's atlas: rows u8 0x25, columns u8 0x26 (1 to 8 on the materials an InitMaterial names,
        // 1 on every other)
        V.MaterialAtlasRows = 0x25, V.MaterialAtlasCols = 0x26;
        return V;
    }();
    return L;
}
}
