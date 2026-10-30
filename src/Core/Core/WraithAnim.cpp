#include "stdafx.h"

#include "WraithAnim.h"

#include <string_view>

WraithAnim::WraithAnim()
{
    AssetType = WraithAssetType::Animation;
    AnimType = WraithAnimationType::Absolute;
    Looping = false;
    DeltaTagName = "";
    FrameRate = 30.0f;
}

WraithAnim::~WraithAnim()
{
}

const uint32_t WraithAnim::FrameCount() const
{
    uint32_t Result = 0;

    for (auto& Frames : AnimationPositionKeys)
    {
        for (auto& Frame : Frames.second)
        {
            Result = std::max(Result, Frame.Frame);
        }
    }
    for (auto& Frames : AnimationRotationKeys)
    {
        for (auto& Frame : Frames.second)
        {
            Result = std::max(Result, Frame.Frame);
        }
    }
    for (auto& Frames : AnimationScaleKeys)
    {
        for (auto& Frame : Frames.second)
        {
            Result = std::max(Result, Frame.Frame);
        }
    }
    for (auto& Frames : AnimationNotetracks)
    {
        for (auto& Frame : Frames.second)
        {
            Result = std::max(Result, Frame);
        }
    }

    // Frames start at 0, so length = highest frame + 1
    return Result + 1;
}

const uint32_t WraithAnim::BoneCount() const
{
    // Bones()' names counted where they are, not copied into a set of strings
    std::unordered_set<std::string_view> Unique;
    for (const auto& Frames : AnimationPositionKeys) Unique.insert(Frames.first);
    for (const auto& Frames : AnimationRotationKeys) Unique.insert(Frames.first);
    for (const auto& Frames : AnimationScaleKeys) Unique.insert(Frames.first);
    for (const auto& Modifiers : AnimationBoneModifiers) Unique.insert(Modifiers.first);
    return (uint32_t)Unique.size();
}

const std::unordered_set<std::string> WraithAnim::Bones() const
{
    std::unordered_set<std::string> UniqueBoneNames;

    for (auto& Frames : AnimationPositionKeys)
    {
        UniqueBoneNames.insert(Frames.first);
    }
    for (auto& Frames : AnimationRotationKeys)
    {
        UniqueBoneNames.insert(Frames.first);
    }
    for (auto& Frames : AnimationScaleKeys)
    {
        UniqueBoneNames.insert(Frames.first);
    }
    for (auto& Modifiers : AnimationBoneModifiers)
    {
        UniqueBoneNames.insert(Modifiers.first);
    }

    return UniqueBoneNames;
}

const uint32_t WraithAnim::NotificationCount() const
{
    uint32_t Result = 0;

    for (auto& NoteList : AnimationNotetracks)
    {
        Result += (uint32_t)NoteList.second.size();
    }

    return Result;
}

void WraithAnim::AddTranslationKey(const std::string& Bone, uint32_t Frame, float X, float Y, float Z)
{
    WraithAnimFrame<Vector3> NewFrame;
    NewFrame.Frame = Frame;
    NewFrame.Value = Vector3(X, Y, Z);
    AnimationPositionKeys[Bone].push_back(NewFrame);
}

void WraithAnim::AddRotationKey(const std::string& Bone, uint32_t Frame, float X, float Y, float Z, float W)
{
    WraithAnimFrame<Quaternion> NewFrame;
    NewFrame.Frame = Frame;
    NewFrame.Value = Quaternion(X, Y, Z, W);
    AnimationRotationKeys[Bone].push_back(NewFrame);
}

void WraithAnim::AddScaleKey(const std::string& Bone, uint32_t Frame, float X, float Y, float Z)
{
    WraithAnimFrame<Vector3> NewFrame;
    NewFrame.Frame = Frame;
    NewFrame.Value = Vector3(X, Y, Z);
    AnimationScaleKeys[Bone].push_back(NewFrame);
}

void WraithAnim::AddBlendShapeKey(const std::string& Shape, uint32_t Frame, float X, float Y, float Z)
{
    WraithAnimFrame<Vector3> NewFrame;
    NewFrame.Frame = Frame;
    NewFrame.Value = Vector3(X, Y, Z);
    AnimationBlendShapeWeightKeys[Shape].push_back(NewFrame);
}

void WraithAnim::AddNoteTrack(const std::string& Notification, uint32_t Frame)
{
    AnimationNotetracks[Notification].push_back(Frame);
}

void WraithAnim::AddBoneModifier(const std::string& Bone, WraithAnimationType Modifier)
{
    AnimationBoneModifiers[Bone] = Modifier;
}

void WraithAnim::ScaleAnimation(float ScaleFactor)
{
    for (auto& Frames : AnimationPositionKeys)
    {
        for (auto& Frame : Frames.second)
        {
            Frame.Value *= ScaleFactor;
        }
    }
}