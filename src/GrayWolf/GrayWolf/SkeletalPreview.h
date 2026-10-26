#pragma once

#include "SeAnimReader.h"
#include <vector>

struct PreviewGeometry;

// One bone's WORLD-space (model-space) transform at a specific evaluated frame.
struct SkeletonPoseBone
{
    float Pos[3] = { 0, 0, 0 };
    float Rot[4] = { 0, 0, 0, 1 }; // quaternion x,y,z,w
};

// Evaluates every bone's WORLD transform at FrameTime (a fractional frame number) by combining Clip's
// keyframes with Bind's bind pose, walking the hierarchy parent-first (bones are assumed listed
// parent-before-child, true of every CoD model/anim export this reads). OutWorldPose is resized to
// Bind.Bones.size(); bones present in Bind but absent from Clip simply hold their bind pose.
//
// SEAnim's own spec (https://github.com/SE2Dev/SEAnim-Docs) documents the file layout only, not
// playback semantics, so this mirrors the reference importer instead - SE2Dev/io_anim_seanim's
// import_seanim.py, from the format's own author, the closest thing to a normative behaviour:
//  - Per bone, the effective type is that bone's modifier override (SeAnimBoneTrack::TypeOverride) if
//    it has one, else the clip's global Type.
//  - ROTATION is the bone's absolute local-to-parent rotation when a rotKey track exists, but for an
//    Additive bone (which the importer leaves "unknown"): its key is a change applied on top of the
//    bind pose's local rotation (bind * key), as the game adds it to the pose beneath. No track: keep
//    the bind pose's local rotation.
//  - TRANSLATION: Absolute replaces the local-to-parent translation outright. Relative/Additive/Delta
//    add the keyed value on top of the bind pose's local translation. A root bone (no parent) always
//    takes the additive path regardless of declared type - it has no parent to be "absolute" against.
//    No track: keep the bind pose's local translation.
//  - Between two keys, translation lerps and rotation slerps; outside a track's own frame range, the
//    nearest end key holds.
void EvaluateSkeletonPose(const PreviewGeometry& Bind, const SeAnimClip& Clip, float FrameTime,
                           std::vector<SkeletonPoseBone>& OutWorldPose);

// Skins Bind's vertices against WorldPose (from EvaluateSkeletonPose, must have one entry per
// Bind.Bones) via linear blend skinning: each vertex's animated position/normal is the weighted sum,
// over its up-to-PREVIEW_MAX_BONE_INFLUENCES influences, of (bone's current world transform) *
// (bone's inverse bind world transform) * (bind-pose vertex). A vertex with no weight data (or all-
// zero weights) passes through unchanged. OutPos/OutNrm are resized to match Bind.Pos/Bind.Nrm.
void SkinPreviewGeometry(const PreviewGeometry& Bind, const std::vector<SkeletonPoseBone>& WorldPose,
                          std::vector<float>& OutPos, std::vector<float>& OutNrm);

// How torn a skinned mesh is: the share of Bind's triangle edges that Posed (skinned positions, as
// SkinPreviewGeometry gives them) makes more than 3 times longer (an anim played on a skeleton it was
// not made for pulls vertices out into spikes; a coherent pose stretches next to none).
float PoseStretch(const PreviewGeometry& Bind, const std::vector<float>& Posed);
