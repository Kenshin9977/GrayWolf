#pragma once

// The effects preview's particles at a time: an effect evaluated in closed form, each particle
// from its spawn time, life and its own randoms (a hash of the run's seed, the element and the
// particle's sequence), so a time can be shown in any order (a slider, a restart, a frame of a
// sequence) and always gives the same picture. Effects started by particles (runners, on death,
// emitted along a path) are evaluated the same way from where and when they start.

#include "FxPreviewModel.h"

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace FxPreview
{
    // One particle as it is drawn
    struct Particle
    {
        const Element* El = nullptr;
        const Material* Mat = nullptr;           // null: no material (a light, a model)
        const std::string* Model = nullptr;
        ElemType Type = ElemType::Billboard;
        float Pos[3] = {};
        float Dir[3] = { 0, 0, 1 };              // the velocity's direction (tails, lines, clouds)
        float Axis[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };   // the element's axes (oriented sprites, models)
        float Size[2] = { 1, 1 };                // half extents
        float Rotation = 0;
        float Scale = 1;
        float LightIntensity = 0, LightRadius = 0, LightFov = 0;   // Black Ops III's lights
        float Intensity = 1;                     // its colour's brightness (vfx IntensityGraph)
        float Color[4] = { 1, 1, 1, 1 };         // straight, 0..1
        float Uv[4] = { 0, 0, 1, 1 };            // the atlas frame: u0, v0, du, dv
        uint32_t Seed = 0;
    };

    // A trail element's particle: its path, oldest first
    struct TrailStrip
    {
        const Material* Mat = nullptr;
        struct Point { float Pos[3]; float Width; float Color[4]; float V; };
        std::vector<Point> Points;
        float Uv[4] = { 0, 0, 1, 1 };
        const TrailDef* Def = nullptr;           // its profile, null: a ribbon facing the camera
        bool Wrap = false;                       // V past 1: the texture repeated along it
    };

    struct Frame
    {
        std::vector<Particle> Particles;
        std::vector<TrailStrip> Trails;
        size_t Effects = 0;                      // effect instances evaluated (the root included)
        bool Capped = false;                     // the particle cap was reached
    };

    struct SimOptions
    {
        uint32_t Seed = 1;
        bool ForwardUp = false;                  // the effect's X axis up (an impact's normal), else as authored (Z up)
        size_t MaxParticles = 60000;
        size_t MaxEffects = 512;                 // effect instances a frame (the root included)
        size_t MaxTrailPoints = 128;             // particles a trail element's ribbon joins
        int MaxDepth = 4;                        // effects started by effects
        // how long an effect started by another loops when its own looping life has no end
        int ChildLoopMsec = 2000;
        // the ground colliding elements stop on (or die on): the effect's origin when its forward
        // is up (an impact on the ground) or its bounds stand on it, else this far below it
        float GroundBelow = 64;
    };

    class Simulator
    {
    public:
        Simulator(std::shared_ptr<Library> Lib, std::shared_ptr<const Effect> Root);
        void SetOptions(const SimOptions& O) { Opt = O; Spans.clear(); Cached = false; }
        const SimOptions& Options() const { return Opt; }
        const Effect& Root() const { return *RootFx; }
        // The particles Msec after the effect started
        void Evaluate(double Msec, Frame& Out);
        // How long one run of the effect lasts: its one-shot elements and what they start; a
        // looping effect's is its longest particle life, at least a second
        double DurationMsec();
        // the ground's height (SimOptions::GroundBelow)
        float GroundZ() const;

    private:
        struct Instance
        {
            const Effect* Fx = nullptr;
            double StartMsec = 0;
            double LoopStopMsec = 1e300;        // looping elements spawn until then (effect time)
            float Origin[3] = {};
            float Axis[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
            uint32_t Seed = 0;
            int Depth = 0;
        };
        void Run(const Instance& I, double Now, Frame& Out);
        // how long after it starts an effect may still draw something (its children's included)
        double Span(const Effect* Fx, int Depth);
        const Effect* Child(const std::string& Name);

        std::shared_ptr<Library> Lib;
        std::shared_ptr<const Effect> RootFx;
        std::vector<std::shared_ptr<const Effect>> Keep;
        std::map<const Effect*, double> Spans;   // Span's, by effect (at the depth it was first asked)
        SimOptions Opt;
        // the last frame evaluated: asked again for the same time (framing, then drawing; the
        // CLI's count, then its picture), copied
        bool Cached = false;
        double CachedMsec = 0;
        Frame Last;
    };
}
