#pragma once

#include <cstdint>
#include <string>
#include <vector>

class WraithAnim;

// The skeleton an XANIM_BIN is written for: the model the anim's GDT entry names ("model"), whose
// bones the linker reads the parts' parents and bind pose from. Bind pose in model space (inches),
// as XMODEL_BIN / XMODEL_EXPORT store it; a bone's parent comes before it or is -1.
struct XAnimBinRig
{
    struct Bone
    {
        std::string Name;
        int32_t Parent = -1;
        double Position[3] = {};
        double Rotation[4] = { 0, 0, 0, 1 };   // quaternion x, y, z, w
    };
    std::vector<Bone> Bones;
    std::string Source;   // the model file, named in the bin's comments
};

struct XAnimBinStats
{
    uint32_t Parts = 0, Frames = 0, Notes = 0;
    std::vector<std::string> Unmatched;   // the anim's bones the rig does not have (not written)
};

// Writes a Black Ops III Mod Tools .XANIM_BIN: what export2bin.exe makes of an XANIM_EXPORT, the
// form the stock GDTs name and linker_modtools reads. The parts are the anim's bones the rig has and
// their parents (as the stock bins); every frame holds each as a model-space transform (OFFSET, then
// the X, Y and Z axes of its rotation): the anim's local keys (interpolated between keys) composed
// on the rig's bind pose, a bone the anim does not key at its bind, a root it does not key at the
// origin and left out (the stock view anims: their poses in tag_view's frame). The keys read as SEAnim's and Cast's importers read them (SkeletalPreview.cpp): a
// translation is an offset from the bind (an absolute anim's non-root bones aside), a rotation the
// bone's local one (an additive bone's: a change from the bind, bind * key). Then its
// notetracks as NUMKEYS + (frame, name); "end" and "loop_end" left out. Game units (inches), as the rig.
class XAnimBin
{
public:
    static bool ExportXAnimBin(const WraithAnim& Animation, const XAnimBinRig& Rig, const std::string& FileName,
                               XAnimBinStats* Stats = nullptr);
};
