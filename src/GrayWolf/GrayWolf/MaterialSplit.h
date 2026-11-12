#pragma once

// The packed maps of an export split into standard files beside them (the split_maps setting):
// for every image a <model>.materials.json reads in more than one role (a "nog": gloss, normal,
// occlusion; a specular map with its gloss in alpha; a colour with its metalness in alpha), or as
// an "ag" normal, one file per role in the layout other tools read: <stem>_normal (x, y, z),
// <stem>_roughness (a gloss inverted), <stem>_ao, <stem>_metalness, <stem>_specular (rgb),
// <stem>_albedo (rgb), <stem>_thickness, <stem>_opacity. Only the descriptions are read, so a
// game supports it through its material adapter; the sidecars keep describing the files as stored.

#include "MaterialDescription.h"

#include <string>
#include <vector>

namespace DiskGame { struct Context; struct Totals; }

namespace MaterialSplit
{
    // One file a split writes: Suffix after the source's stem, what it takes of the source
    struct Output
    {
        enum class Op { Normal, Grey, Rgb };
        std::string Suffix;            // "_roughness"
        Op What = Op::Grey;
        int Channel = 0;               // Grey: the source's channel
        bool Invert = false;           // Grey: 255 - value (a gloss as a roughness)
        Materials::NormalEncoding Enc = Materials::NormalEncoding::XY;   // Normal: where x is
        bool operator<(const Output& O) const { return Suffix < O.Suffix; }
    };

    // What the split of one material's images writes, by image path (as the description gives it):
    // the packed ones only. Materials of one sidecar: an image packed in one is split for all.
    std::vector<std::pair<std::string, std::vector<Output>>> Plan(const std::vector<Materials::Description>& Materials);

    // Every sidecar under the run's output folder, its packed images split (kept when newer than
    // their source). Counters: split_maps_images, split_maps_files, split_maps_unread.
    void WriteForRun(const DiskGame::Context& C, DiskGame::Totals& T);
}
