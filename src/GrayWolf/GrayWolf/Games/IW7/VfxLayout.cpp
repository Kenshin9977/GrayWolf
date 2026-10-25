#include "stdafx.h"

#include "VfxLayout.h"

// Infinite Warfare's particle system. Levels and sizes: the iw7 tables (vfx fa17c90 0x80, emitter
// f9ff150 0x90, state fa01010 0x20, group f9ff680 0x10, module f9ff540 0xa0 its type u16 at 0 and
// its data at 0x10, linked asset 0x20, curve f9ff080 0x10). Module ids, names and data sizes: the
// exe's module table (iw7_flat 0x1526a90, 0x30 per type: update, init, defaults, size, name).
// Fields: x64-zt's IW7 structs (ParticleModule*), the ones the preview reads checked against the
// exe's init functions (Attributes 0xd16170, Rotation 0xd16660, Rotation3D 0xd16530, shapes box
// 0xd16750, cylinder 0xd16970, sphere 0xd17420) and the tables' curve and asset offsets.
namespace IW7
{
namespace
{
    using Vfx::AssetKind;
    using Vfx::Field;
    using T = Vfx::FieldType;

    Vfx::Layout Make()
    {
        Vfx::Layout L;
        L.Game = "iw7";
        L.SystemFields = { { "version", 0x18, T::I32 }, { "flags", 0x24, T::Hex32 }, { "occlusionOverrideEmitter", 0x28, T::I32 },
                           { "phaseOptions", 0x2c, T::U32 }, { "drawFrustumCullRadius", 0x30, T::F32 },
                           { "updateFrustumCullRadius", 0x34, T::F32 }, { "sunDistance", 0x38, T::F32 }, { "preRollMsec", 0x3c, T::I32 } };
        // ParticleEmitterDef: spawn rate per second, lives and delays in seconds (checked on the data)
        L.EmitterFields = { { "spawnRate", 0xc, T::Range }, { "particleLife", 0x14, T::Range }, { "particleDelay", 0x1c, T::Range },
                            { "particleCountMax", 0x24, T::U32 }, { "burstCount", 0x28, T::IntRange }, { "emitterLife", 0x30, T::Range },
                            { "emitterDelay", 0x38, T::Range }, { "randomSeed", 0x40, T::I32 }, { "spawnRangeSq", 0x44, T::Range },
                            { "fadeOutMaxDistance", 0x4c, T::F32 }, { "fadeCurve", 0x50, T::Curve }, { "spawnFrustumCullRadius", 0x60, T::F32 },
                            { "flags", 0x64, T::Hex32 }, { "gravityOptions", 0x68, T::U32 }, { "groupIds", 0x6c, T::I32, 4 },
                            { "spawnShapeRange", 0x84, T::Range } };
        L.StateFields = { { "flags", 0x10, T::Hex64 } };
        L.MaterialAtlasRows = 0xb, L.MaterialAtlasCols = 0xc;   // MaterialInfo textureAtlasRowCount, ColumnCount
        L.ElementTypes = { "billboardSprite", "beam", "cloud", "sparkCloud", "decal", "flare", "geoTrail", "lightOmni", "lightSpot",
                           "model", "orientedSprite", "runner", "tail", "vectorField", "volumetric" };

        // the module's data: {u16 type, u32 flags at 4} then its fields
        const auto C = [](const char* N, int64_t O, int Count = 1) { return Field{ N, O, T::Curve, Count }; };
        const std::vector<Field> Test = { { "moduleIndex", 8, T::U16 }, { "useOrientation", 0xa, T::U8 }, { "useScale", 0xb, T::U8 },
                                          { "useVelocity", 0xc, T::U8 }, { "nextState", 0x10, T::U32 }, { "kill", 0x28, T::Bool },
                                          { "age", 0x2c, T::F32 } };
        const std::vector<Field> Shape = { { "axisFlags", 8, T::U8 }, { "spawnFlags", 9, T::U8 }, { "normalAxis", 0xa, T::U8 },
                                           { "spawnType", 0xb, T::U8 }, { "volumeCubeRoot", 0xc, T::F32 } };
        using Vfx::With;
        L.Modules = {
            { 0, "InitAtlas", 0x20, { { "playRate", 8, T::I32 }, { "startFrame", 0xc, T::I32 }, { "loopCount", 0x10, T::I32 } } },
            { 1, "InitAttributes", 0x70, { { "nonUniformColor", 8, T::Bool }, { "nonUniformSize", 9, T::Bool }, { "sizeMin", 0x10, T::Vec4 },
                                          { "sizeMax", 0x20, T::Vec4 }, { "colorMin", 0x30, T::Vec4 }, { "colorMax", 0x40, T::Vec4 },
                                          { "velocityMin", 0x50, T::Vec4 }, { "velocityMax", 0x60, T::Vec4 } } },
            { 2, "InitBeam", 0x80, { { "beamFlags", 8, T::Hex32 }, { "splitDistance", 0xc, T::F32 }, { "startPos", 0x10, T::Vec4 },
                                    { "endPos", 0x20, T::Vec4 }, { "offset", 0x30, T::Vec4 }, { "curvePoint1", 0x40, T::Vec4 },
                                    { "curvePoint2", 0x50, T::Vec4 }, { "scrollRateUV", 0x70, T::F32 }, { "scrollRateSpiral", 0x74, T::F32 },
                                    { "spiralRepeatDistance", 0x78, T::F32 } }, AssetKind::Effect, 0x60 },
            { 3, "InitCameraOffset", 0x10, {} },
            { 4, "InitCloud", 0x30, { C("curves", 0x10, 2) } },
            { 5, "InitDecal", 0x30, { { "fadeInTime", 8, T::U16 }, { "fadeOutTime", 0xa, T::U16 }, { "stoppableFadeOutTime", 0xc, T::U16 },
                                     { "lerpWaitTime", 0xe, T::U16 }, { "lerpColor", 0x10, T::Vec4 } }, AssetKind::Decal, 0x20 },
            { 6, "InitFlare", 0x90, { { "position", 0xc, T::F32 }, { "direction", 0x10, T::Vec4 }, { "angularRotCount", 0x20, T::I32 },
                                     { "flareFlags", 0x24, T::Hex32 }, { "depthScaleRange", 0x28, T::Range }, { "depthScaleValue", 0x30, T::Range },
                                     { "radialRot", 0x38, T::Range }, { "radialScaleX", 0x40, T::Range }, { "radialScaleY", 0x48, T::Range },
                                     C("curves", 0x50, 4) } },
            { 7, "InitGeoTrail", 0x40, { { "numPointsMax", 8, T::U32 }, { "splitDistance", 0xc, T::F32 }, { "splitAngle", 0x10, T::F32 },
                                        { "centerOffset", 0x14, T::F32 }, { "numSheets", 0x18, T::U32 }, { "fadeInDistance", 0x1c, T::F32 },
                                        { "fadeOutDistance", 0x20, T::F32 }, { "tileDistance", 0x24, T::F32 }, { "tileOffset", 0x28, T::Range },
                                        { "scrollTime", 0x30, T::F32 }, { "useLocalVelocity", 0x34, T::Bool }, { "useVerticalTexture", 0x35, T::Bool },
                                        { "cameraFacing", 0x36, T::Bool }, { "fixLeadingEdge", 0x37, T::Bool }, { "clampUVs", 0x38, T::Bool } } },
            { 8, "InitLightOmni", 0x30, { { "tonemappingScaleFactor", 0x18, T::F32 }, { "intensityIR", 0x1c, T::F32 },
                                         { "disableVolumetric", 0x20, T::Bool }, { "exponent", 0x21, T::U8 }, { "shadowSoftness", 0x24, T::F32 },
                                         { "shadowBias", 0x28, T::F32 }, { "shadowArea", 0x2c, T::F32 } }, AssetKind::LightDef, 8 },
            { 9, "InitLightSpot", 0x50, { { "fovOuter", 8, T::F32 }, { "fovInner", 0xc, T::F32 }, { "bulbRadius", 0x10, T::F32 },
                                         { "bulbLength", 0x14, T::F32 }, { "brightness", 0x18, T::F32 }, { "intensityUV", 0x20, T::F32 },
                                         { "intensityIR", 0x24, T::F32 }, { "shadowNearPlane", 0x28, T::F32 }, { "toneMappingScaleFactor", 0x2c, T::F32 },
                                         { "shadowSoftness", 0x30, T::F32 }, { "shadowBias", 0x34, T::F32 }, { "shadowArea", 0x38, T::F32 } },
              AssetKind::LightDef, 0x40 },
            { 10, "InitMaterial", 0x20, {}, AssetKind::Material, 0x10 },
            { 11, "InitMirrorTexture", 0x10, { { "mirrorHorizontal", 8, T::U32 }, { "mirrorVertical", 0xc, T::U32 } } },
            { 12, "InitModel", 0x20, { { "usePhysics", 8, T::Bool }, { "motionBlurHQ", 9, T::Bool } }, AssetKind::Model, 0x10 },
            { 13, "InitOcclusionQuery", 0x20, { { "fadeInTime", 8, T::F32 }, { "fadeOutTime", 0xc, T::F32 }, { "scale", 0x10, T::Range },
                                               { "worldSize", 0x18, T::Range } } },
            { 14, "InitOrientedSprite", 0x20, { { "orientationQuat", 0x10, T::Vec4 } } },
            { 15, "InitParticleSim", 0x30, { { "scaleFactor", 0x18, T::Range }, { "holdLastFrame", 0x20, T::Bool } }, AssetKind::ParticleSim, 8 },
            { 16, "InitPlayerFacing", 0x10, {} },
            { 17, "InitRelativeVelocity", 0x10, { { "velocityType", 8, T::U32 }, { "useBoltInfo", 0xc, T::Bool } } },
            { 18, "InitRotation", 0x20, { { "angle", 0x10, T::Range }, { "rate", 0x18, T::Range } } },
            { 19, "InitRotation3D", 0x50, { { "angleMin", 0x10, T::Vec4 }, { "angleMax", 0x20, T::Vec4 }, { "rateMin", 0x30, T::Vec4 },
                                           { "rateMax", 0x40, T::Vec4 } } },
            { 20, "InitRunner", 0x20, {}, AssetKind::Effect, 0x10 },
            { 21, "InitSound", 0x20, {}, AssetKind::Sound, 0x10 },
            { 22, "InitSpawn", 0x20, { C("curves", 0x10) } },
            { 23, "InitSpawnShapeBox", 0x50, With(Shape, { { "useBeamInfo", 0x20, T::Bool }, { "dimensionsMin", 0x30, T::Vec4 },
                                                           { "dimensionsMax", 0x40, T::Vec4 } }) },
            { 24, "InitSpawnShapeCylinder", 0x50, With(Shape, { { "hasRotation", 0x20, T::Bool }, { "rotateCalculatedOffset", 0x21, T::Bool },
                                                                { "halfHeight", 0x24, T::F32 }, { "radius", 0x28, T::Range },
                                                                { "offset", 0x30, T::Vec4 }, { "directionQuat", 0x40, T::Vec4 } }) },
            { 25, "InitSpawnShapeEllipsoid", 0x50, With(Shape, { { "offset", 0x10, T::Vec4 }, { "radiusMin", 0x20, T::Vec4 },
                                                                 { "radiusMax", 0x30, T::Vec4 } }) },
            { 26, "InitSpawnShapeMesh", 0x50, With(Shape, { { "numMeshAssets", 0x20, T::U32 } }), AssetKind::Model, 0x28 },
            { 27, "InitSpawnShapeSphere", 0x40, With(Shape, { { "radius", 0x28, T::Range }, { "offset", 0x30, T::Vec4 } }) },
            { 28, "InitTail", 0x10, { { "averagePastVelocities", 8, T::U16 }, { "maxParentSpeed", 0xa, T::U16 }, { "tailLeading", 0xc, T::Bool },
                                     { "scaleWithVelocity", 0xd, T::Bool }, { "rotateAroundPivot", 0xe, T::Bool } } },
            { 29, "InitVectorField", 0x20, {}, AssetKind::VectorField, 0x10 },
            { 30, "InitVolumetric", 0x20, { { "density", 8, T::F32 }, { "falloff", 0xc, T::F32 }, { "noiseAmplitude", 0x10, T::F32 },
                                           { "noiseMode", 0x14, T::U32 } } },
            { 31, "Attractor", 0x30, { { "killWhenNear", 0x14, T::Bool }, { "forceMag", 0x18, T::F32 }, { "nearDistanceSq", 0x1c, T::F32 },
                                      { "attractPoint", 0x20, T::Vec4 } } },
            { 32, "ColorGraph", 0x90, { { "firstCurve", 8, T::Bool }, { "modulateColorByAlpha", 0xc, T::Bool }, C("curves", 0x10, 8) } },
            { 33, "ColorLerp", 0x30, { { "colorBegin", 0x10, T::Vec4 }, { "colorEnd", 0x20, T::Vec4 } } },
            { 34, "EmissionGraph", 0x30, { C("curves", 0x10, 2) } },
            { 35, "EmissiveGraph", 0x30, { { "firstCurve", 8, T::Bool }, C("curves", 0x10, 2) } },
            { 36, "Force", 0x30, { { "forceMin", 0x10, T::Vec4 }, { "forceMax", 0x20, T::Vec4 } } },
            { 37, "Gravity", 0x10, { { "gravity", 8, T::Range } } },
            { 38, "IntensityGraph", 0x30, { { "firstCurve", 8, T::Bool }, C("curves", 0x10, 2) } },
            { 39, "ParentVelocityGraph", 0x30, { C("curves", 0x10, 2) } },
            { 40, "PhysicsLight", 0x20, { { "ignoreEmitterOrientation", 8, T::Bool }, { "useSurfaceType", 9, T::Bool } }, AssetKind::Physics, 0x10 },
            { 41, "PhysicsRayCast", 0x30, { { "bounce", 8, T::Range }, { "boundsMidPoint", 0x10, T::Vec4 }, { "useItemClip", 0x28, T::Bool },
                                           { "useSurfaceType", 0x29, T::Bool }, { "collideWithWater", 0x2a, T::Bool },
                                           { "ignoreContentItem", 0x2b, T::Bool } } },
            { 42, "PositionGraph", 0x70, { C("curves", 0x10, 6) } },
            { 43, "RotationGraph", 0x30, { { "useRotationRate", 8, T::Bool }, C("curves", 0x10, 2) } },
            { 44, "RotationGraph3D", 0x70, { { "useRotationRate", 8, T::Bool }, C("curves", 0x10, 6) } },
            { 45, "SizeGraph", 0x90, { { "firstCurve", 8, T::Bool }, C("curves", 0x10, 6), { "sizeBegin", 0x70, T::Vec4 },
                                      { "sizeEnd", 0x80, T::Vec4 } } },
            { 46, "SizeLerp", 0x30, { { "sizeBegin", 0x10, T::Vec4 }, { "sizeEnd", 0x20, T::Vec4 } } },
            { 47, "VelocityGraph", 0x90, { C("curves", 0x10, 6), { "velocityBegin", 0x70, T::Vec4 }, { "velocityEnd", 0x80, T::Vec4 } } },
            { 48, "TestAge", 0x30, Test, AssetKind::Effect, 0x18 },
            { 49, "TestBirth", 0x30, Test, AssetKind::Effect, 0x18 },
            { 50, "TestDeath", 0x30, Test, AssetKind::Effect, 0x18 },
            { 51, "TestGravity", 0x30, Test, AssetKind::Effect, 0x18 },
            { 52, "TestImpact", 0x30, Test, AssetKind::Effect, 0x18 },
            { 53, "TestPos", 0x30, Test, AssetKind::Effect, 0x18 },
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
