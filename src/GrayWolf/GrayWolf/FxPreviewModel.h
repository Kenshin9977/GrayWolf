#pragma once

// The effects preview's model of an effect, whatever game it comes from: an effect is a list of
// elements, each spawning particles (looping, one-shot or emitted) that move along velocity graphs
// and gravity and change size, colour and rotation over their life, drawn as sprites, tails,
// clouds, models or lights, or starting other effects. Read from the fx kind's JSON
// (Games/Treyarch/TreyarchEffects.cpp writes it) by the adapter that knows the game's fields:
// FxPreview::LoadEffect picks it from what the file holds.

#include <nlohmann/json_fwd.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace FxPreview
{
    // base + random * amplitude
    struct Range
    {
        float Base = 0, Amp = 0;
        float At(float R) const { return Base + Amp * R; }
    };

    enum class ElemType
    {
        Billboard,   // camera-facing quad
        Oriented,    // quad in the element's own plane
        Rotated,     // camera-facing, turned around its velocity (drawn as Billboard)
        Tail,        // stretched along the velocity, behind the particle
        Line,        // a tail of fixed length
        Trail,       // a ribbon along the particle's path
        Cloud,       // a puff of small sprites
        Model,
        Light,       // omni or dynamic light: a glow
        SpotLight,   // a cone of light
        Sound,
        Decal,       // a mark on the ground
        LensFlare,   // a flare on the screen where it is
        Beam,        // a ray from where it is along its forward axis
        Runner,      // starts another effect where it spawns
        Cylinder,    // CoD1's: a ring wall along its forward axis, radius Size[0] there, Size[1] at its height (Scale) away
        Other,
    };

    // Additive: the colour times its alpha added (the games' ONE/ONE and screen materials too:
    // their shaders fade by the alpha)
    enum class Blend { Alpha, Additive, Premultiplied, Opaque };

    // What a material visual draws: its colour image (a file found by name among the exported
    // images) and how it blends.
    struct Material
    {
        std::string Name;
        std::vector<std::string> Images;   // the images the export names, colour map first; empty: guessed from Name
        std::string Techset;
        int BlendSrc = -1, BlendDst = -1;  // the game's blend factors when the export gives them
        Blend Mode = Blend::Alpha;
        bool Lit = false;                  // a lit material: drawn darker than an emissive one
        bool Distortion = false;           // a heat haze: bends what is behind it, not drawn here
        bool LightSprite = false;          // a sprite that lights what is around it (MWR's post_spritelight): its glow
        // an emissive material's HDR intensity (Black Ops III: its hdrScale constant), 0: not given
        float HdrScale = 0;
        // Black Ops 4: an image whose red is the colour image's alpha (its effects' HDR colour
        // maps carry none: fxt8_fire_*_hdr and fxt8_fire_*_alpha), "" when none
        std::string AlphaMask;
        int AtlasRows = 0, AtlasCols = 0;  // the material's own atlas grid (Infinite Warfare and after), 0: not given
        // an animated material's frames, one image each (CoD1's animMap shaders), played FrameRate a
        // second: drawn as an atlas of one row, the frames side by side
        std::vector<std::string> Frames;
        float FrameRate = 0;
        // Black Ops 6/7: the alpha from another image (OpacityInverted: a transmittance map, 1 - it), a grey
        // emissive map coloured through a ramp image; Emissive: no colour map, the emissive one drawn
        std::string Opacity, Ramp;
        bool OpacityInverted = false, Emissive = false;
        // the images as the export lists them (usage, semantic, name): read again by the game's
        // semantics when the export says "unknown" (Library: MWIII)
        struct Slot { std::string Usage; uint32_t Semantic = 0; std::string Name; };
        std::vector<Slot> Slots;
    };

    // One sample of the visual graphs: each value base + random * amplitude, the colour between
    // base and amplitude (one random for the four channels).
    struct VisualSample
    {
        float ColorBase[4] = { 1, 1, 1, 1 }, ColorAmp[4] = { 1, 1, 1, 1 };   // 0..1
        Range RotationDelta, RotationTotal, Size[2], Scale;
        Range LightIntensity, LightRadius, LightFov;   // Black Ops III's lights (0: not given)
        Range Intensity{ 1, 0 };                       // a sprite's brightness (vfx IntensityGraph)
    };

    // One sample of the velocity graphs, in the element's frame (Local) and the world's (World):
    // the velocity and the distance covered so far, per normalized life, times the life in msec.
    struct VelocitySample
    {
        Range Velocity[2][3], TotalDelta[2][3];   // [local, world][axis]
    };

    // A spark fountain's sparks (FxSparkFountainDef: MW2, MW3 and the Infinity Ward games after;
    // its fields' sense from their names and the values the games give, the game's code not
    // read): Count sparks launched together from where its particle spawned, again every
    // LoopTime msec, each at VelMin..VelMax units/msec within a cone around the particle's
    // direction (ConeFrac of the sphere: 1 all of it, 0.5 the half ahead), falling at
    // Gravity units/msec^2 (negative: down), bouncing on the ground with BounceFrac + BounceRand
    // of their speed until slower than RestSpeed (units/sec), BoostFactor times faster over their
    // first BoostTime msec; each drawn as a streak SparkLength units long
    struct SparkFountain
    {
        float Gravity = 0, BounceFrac = 0, BounceRand = 0, Spacing = 0, Length = 0, LoopTime = 0;
        float VelMin = 0, VelMax = 0, ConeFrac = 1, RestSpeed = 0, BoostTime = 0, BoostFactor = 0;
        int Count = 0;
    };

    // A trail's profile (FxTrailDef): its cross-section's segments (Inds, by pairs of Verts:
    // a position across the ribbon, times its width, the outward normal, the texture's u)
    // swept along it; the texture repeated every RepeatDist units along it (0: once over it),
    // scrolled once every ScrollTimeMsec (0: still)
    struct TrailDef
    {
        struct Vert { float Pos[2], Normal[2], U; };
        std::vector<Vert> Verts;
        std::vector<uint16_t> Inds;
        int ScrollTimeMsec = 0;
        float RepeatDist = 0;
    };

    // A light's shape when its def gives it (FxSpotLightDef: MW3's, Ghosts': a cone from
    // StartRadius at the light to EndRadius MaxLength away; AW's, MWR's: half its angle, its
    // reach), what its visual graph does not: the preview's halo and cone
    struct LightShape
    {
        float Reach = 0, HalfFov = 0, StartRadius = 0, Brightness = 0;   // units, radians; 0: not given
    };

    enum class SpawnGroup { Looping, OneShot, Emission };
    enum class SpawnOffset { None, Sphere, Cylinder };
    enum class RunRelative { World, Spawn, Effect, Offset };

    struct Element
    {
        SpawnGroup Group = SpawnGroup::OneShot;
        ElemType Type = ElemType::Billboard;
        std::string TypeName;               // as the game names it

        // Looping: one spawn every IntervalMsec, Count spawns in all (<= 0: no end), PerInterval
        // particles each time. One-shot: Count particles once.
        int IntervalMsec = 0, LoopCount = 0;
        Range PerInterval{ 1, 0 };
        Range OneShotCount{ 1, 0 };
        Range DelayMsec, LifeMsec;

        Range Origin[3];
        SpawnOffset Offset = SpawnOffset::None;
        Range OffsetRadius, OffsetHeight;
        // the vfx spawn shapes (Games/IW/VfxPreview.cpp): a point (x, y, z) of the shape, a cylinder's
        // radius in x, y and its height in z, each axis kept by its sign (AxisFlags: per axis 1
        // positive, 2 negative, neither: 0), then turned by the shape's quaternion (x, y, z, w) into
        // the effect's frame; the Treyarch shapes (Shaped false): a cylinder along forward, a sphere
        // in the world's axes
        bool Shaped = false;
        uint8_t AxisFlags = 0x3f;
        float ShapeQuat[4] = { 0, 0, 0, 1 };
        RunRelative Relative = RunRelative::Effect;
        Range Angles[3], AngularVelocity[3];   // radians
        Range InitialRotation;
        Range Gravity;                          // a fraction of 800 units/s^2
        bool UseLocalVelocity = true, UseWorldVelocity = true;
        bool NonUniformScale = false;
        bool Collide = false, DieOnTouch = false;   // stops at (or dies on) the ground
        bool UnderwaterOnly = false;                // drawn under water only: not shown
        int LitType = 0;                            // Advanced Warfare, MWR: 2, 3 lit by the light grid

        // atlas: Frames sub-images in Cols x Rows; start fixed (Index), random or by sequence;
        // played over the life or at Fps
        int AtlasStart = 0, AtlasIndex = 0, AtlasFps = 0, AtlasLoops = 0, AtlasCols = 1, AtlasRows = 1;
        bool AtlasOverLife = false, AtlasLoopLimited = false;
        bool AtlasFromMaterial = false;         // the grid is its material's (set once the material is known)

        std::vector<VelocitySample> Velocity;
        std::vector<VisualSample> Visual;

        std::vector<Material> Materials;        // one picked per particle
        std::vector<std::string> Models;
        std::vector<std::string> Effects;       // a runner's effects
        std::string OnDeath, Emitted, OnImpact, Attached;
        Range EmitDist, EmitDistVariance;
        float TrailLength = 0;                  // a trail's length in msec of the particle's path (0: half its life)
        std::optional<SparkFountain> Fountain;  // its particles emit sparks (drawn instead of them)
        std::optional<TrailDef> Trail;          // a trail's profile
        LightShape Light;
    };

    struct Effect
    {
        std::string Name, File, Game;
        int LoopingLifeMsec = 0, NonLoopingLifeMsec = 0;
        float BoundsCenter[3] = {}, BoundsHalf[3] = { 16, 16, 16 };
        bool BoundsKnown = false;
        bool Timed = true;                      // false: the game's JSON has no timings (Cold War)
        std::vector<Element> Elements;
        // The longest one-shot run (msec): the last particle's death, the effects it starts not counted
        int OneShotMsec() const;
        bool Loops() const;
    };

    // Reads an effect from the fx kind's JSON. False with Error when the file is not one.
    bool LoadEffect(const std::string& JsonPath, Effect& Out, std::string& Error);
    // A material's techset, images and blend from a material entry of that JSON (or the
    // material kind's file), how it blends classified: for another game's adapter
    void ReadMaterial(const nlohmann::json& V, Material& M);

    class TextureIndex;

    // The effects an effect starts, by name, and where their files are: the fx folder above the
    // first one's (fx/<name>.json), loaded once. Lookups by the game's name (fire/fx_fire_sm).
    class Library
    {
    public:
        // Root: the folder fx names are relative to (<game>/fx); found from the first effect's
        // file and its name when empty.
        explicit Library(std::string Root = {});
        // the effect of that file, as the root effect (its folder sets Root when unset)
        std::shared_ptr<const Effect> LoadRoot(const std::string& JsonPath, std::string& Error);
        // an effect by its game name, null when its file is not in the export
        std::shared_ptr<const Effect> ByName(const std::string& Name);
        const std::string& Root() const { return RootDir; }
        // other export folders to look for effects in (a common zone exported apart)
        void AddSearchDir(const std::string& FxDir) { Extra.push_back(FxDir); }
        // where a material the effect only names (another zone's) finds its images, techset and
        // blend: the material kind's files (set before the effects are loaded)
        void SetMaterials(std::shared_ptr<const TextureIndex> Index) { Materials = std::move(Index); }

    private:
        void Complete(Effect& Fx) const;
        std::string RootDir;
        std::vector<std::string> Extra;
        std::shared_ptr<const TextureIndex> Materials;
        std::map<std::string, std::shared_ptr<const Effect>> Cache;
    };

    // Finds the exported images by name: every image file under the given folders (recursively),
    // indexed once. A material's image is the one its export names, else the one its name
    // suggests (gfx_<x>[_suffix] -> fxt_<x>, the games' naming).
    class TextureIndex
    {
    public:
        void AddFolder(const std::string& Dir);
        // the image file for a material, "" when none is found
        std::string Find(const Material& M) const;
        std::string FindImage(const std::string& ImageName) const;
        size_t Size() const { return Files.size(); }
        // an exported model's file (its LOD0) by the model's name, "" when not exported
        std::string FindModel(const std::string& ModelName) const;
        size_t Models() const { return ModelFiles.size(); }
        // a material the material kind wrote (materials/<name>.json), null when none
        const Material* FindMaterial(const std::string& Name) const;
        size_t MaterialCount() const { return MaterialFiles.size(); }

    private:
        std::map<std::string, std::string> Files;   // lower-case stem -> path (.dds before .png)
        std::map<std::string, std::string> ModelFiles;
        std::map<std::string, Material> MaterialFiles;   // lower-case name -> its images, techset, blend
        std::vector<std::string> Folders;
    };

    // The folders a preview of the effect at JsonPath looks in: its game folder (above fx/) and
    // the other game folders listed beside it with the same game id (a common zone's export).
    std::vector<std::string> GameFolders(const std::string& JsonPath);
}
