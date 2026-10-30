#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "WraithAsset.h"

#include "VectorMath.h"

// Specifies how the animation data is interpreted by the importer
enum class WraithAnimationType : uint8_t
{
    // Animation translations are set to this exact value each frame
    Absolute = 0,
    // This animation is applied to existing animation data in the scene (and is absolute)
    Additive = 1,
    // Animation translations are based on rest position in scene
    Relative = 2,
    // This animation is relative and contains delta data (Whole model movement) Delta tag name must be set!
    Delta = 3,
};

template<class T>
struct WraithAnimFrame
{
    uint32_t Frame;
    T Value;
};

class WraithAnim : public WraithAsset
{
public:
    WraithAnim();
    ~WraithAnim();

    std::unordered_map<std::string, std::vector<WraithAnimFrame<Vector3>>> AnimationPositionKeys;
    std::unordered_map<std::string, std::vector<WraithAnimFrame<Quaternion>>> AnimationRotationKeys;
    std::unordered_map<std::string, std::vector<WraithAnimFrame<Vector3>>> AnimationScaleKeys;
    // A list of animation blendshape weight keys per axis
    std::unordered_map<std::string, std::vector<WraithAnimFrame<Vector3>>> AnimationBlendShapeWeightKeys;
    std::unordered_map<std::string, std::vector<uint32_t>> AnimationNotetracks;
    std::unordered_map<std::string, WraithAnimationType> AnimationBoneModifiers;

    const uint32_t FrameCount() const;
    const uint32_t BoneCount() const;
    const std::unordered_set<std::string> Bones() const;
    const uint32_t NotificationCount() const;

    WraithAnimationType AnimType;
    bool Looping;
    // The name of the delta tag of which to treat as the delta bone (If any)
    std::string DeltaTagName;
    // The framerate of the animation as a float (Defaults to 30.0)
    float FrameRate;

    void AddTranslationKey(const std::string& Bone, uint32_t Frame, float X, float Y, float Z);
    void AddRotationKey(const std::string& Bone, uint32_t Frame, float X, float Y, float Z, float W);
    void AddScaleKey(const std::string& Bone, uint32_t Frame, float X, float Y, float Z);
    void AddBlendShapeKey(const std::string& Shape, uint32_t Frame, float X, float Y, float Z);

    void AddNoteTrack(const std::string& Notification, uint32_t Frame);
    // Adds a bone modifier for this bone, overwriting if existing
    void AddBoneModifier(const std::string& Bone, WraithAnimationType Modifier);

    void ScaleAnimation(float ScaleFactor);
};