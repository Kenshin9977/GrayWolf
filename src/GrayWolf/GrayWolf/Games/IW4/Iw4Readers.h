#pragma once

// The live readers of Modern Warfare 2's asset shapes (XModel with its LODs and XSurfaces,
// XAnimParts, Material), run over a zone served by VirtualGameSession: what MW2/MW3's 64-bit
// rebuild (Games/IW4/Iw64Readers.h) and Call of Duty Online (Games/CODOL/CodolReaders.h) share.
// Each title gives where its structs keep the fields and its pointer width.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class CoDAnim_t;
class CoDModel_t;
struct XMaterial_t;
struct XModel_t;
struct XAnim_t;

namespace Iw4Family
{
    struct Layout
    {
        int64_t PointerSize = 8;
        // XModel: bone counts (u8), bone names (u16 script strings), parents, quaternions,
        // translations, base matrices, material handles; lodInfo[4] (its size) and numLods (u8). A
        // LOD: distance 0, surface count u16 4, first surface u16 6, its XModelSurfs at 8, whose
        // surfaces are at ModelSurfsSurfs.
        int64_t ModelBones = 0, ModelRootBones = 0, ModelBoneNames = 0, ModelParents = 0, ModelQuats = 0, ModelTrans = 0,
                ModelBaseMat = 0, ModelMaterials = 0, ModelLods = 0, LodSize = 0, ModelLodCount = 0, ModelSurfsSurfs = 0;
        // XSurface: its size, vertex and triangle counts (u16), triangle indices, four i16 blend
        // counts, blend data, packed vertices, the rigid vertex list count and array
        int64_t SurfSize = 0, SurfVerts = 0, SurfTris = 0, SurfTriIndices = 0, SurfBlendCounts = 0, SurfBlend = 0,
                SurfVerts0 = 0, SurfVertListCount = 0, SurfVertList = 0;
        // Material: its name, texture count (u8) and MaterialTextureDef table (records of
        // TextureDefSize: name hash 0, semantic u8 7, image 8); GfxImage's name
        int64_t MaterialName = 0, MaterialTextureCount = 0, MaterialTextureTable = 0, TextureDefSize = 0, ImageName = 0;
        // XAnimParts: numframes (u16), the flags byte (looping: a bit of LoopMask), boneCount[10]
        // and notifyCount (u8), the frame rate, ten pointers from AnimPointers (bone names, data
        // bytes, shorts, ints, random data shorts, bytes, ints, indices, notifies), the delta part
        int64_t AnimFrames = 0, AnimFlags = 0, AnimCounts = 0, AnimRate = 0, AnimPointers = 0, AnimDelta = 0;
        uint8_t LoopMask = 0xff;
        // An XModelSurfs whose surfaces are not in the zone, by name: where they are served (0:
        // nowhere); unset: none is streamed
        std::function<uint64_t(const std::string& Name)> StreamedSurfs;
        // A name-only material header (",name", no texture table), by its name without the ",":
        // where the material it stands for is served (0: nowhere); else its images (name hash,
        // image name) as another zone defines them, read later by name (ExportWriters::
        // ImageElsewhere; false: no zone booked it). Unset: none is looked up.
        std::function<uint64_t(const std::string& Name)> MaterialByName;
        std::function<bool(const std::string& Name, std::vector<std::pair<uint32_t, std::string>>& Images)> MaterialImagesByName;
    };

    std::unique_ptr<XModel_t> ReadXModel(const CoDModel_t* Model, const Layout& L);
    std::unique_ptr<XAnim_t> ReadXAnim(const CoDAnim_t* Animation, const Layout& L);
    XMaterial_t ReadXMaterial(uint64_t MaterialPointer, const Layout& L);
}
