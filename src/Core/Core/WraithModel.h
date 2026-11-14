#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "InlineVector.h"
#include "WraithAsset.h"

#include "VectorMath.h"

class WraithBone
{
public:
    WraithBone();

    std::string TagName;
    // The index of this bones parent in the WraithModel bone list (-1 means connected to the root)
    int32_t BoneParent;
    bool IsCosmetic;
    Vector3 LocalPosition;
    Quaternion LocalRotation;
    Vector3 GlobalPosition;
    Quaternion GlobalRotation;
    // The bone scale, defaults to 1.0
    Vector3 BoneScale;
};

class WraithFace
{
public:
    WraithFace();
    WraithFace(uint32_t Vert1, uint32_t Vert2, uint32_t Vert3);

    uint32_t Index1;
    uint32_t Index2;
    uint32_t Index3;
};

class WraithVertexWeight
{
public:
    WraithVertexWeight();
    WraithVertexWeight(uint32_t Bone, float Value);

    uint32_t BoneIndex;
    // The vertex weight (Out of 1.0)
    float Weight;
};

class WraithVertex
{
public:
    WraithVertex();
    
    Vector3 Position;
    std::vector<std::pair<uint32_t, Vector3>> BlendShapeDeltas;
    // up to 4 weights and 1 UV layer held inline: no allocation per vertex for most models
    InlineVector<WraithVertexWeight, 4> Weights;
    Vector3 Normal;
    InlineVector<Vector2, 1> UVLayers;
    uint8_t Color[4];

    uint32_t WeightCount() const;
    uint32_t UVLayerCount() const;

    void AddVertexWeight(uint32_t BoneIndex, float Weight);
    void AddUVLayer(float UVU, float UVV);

};

class WraithSubmesh
{
public:
    WraithSubmesh();

    std::vector<WraithVertex> Verticies;
    std::vector<WraithFace> Faces;
    // The material index used for each UV layer, in order, -1 means we have no material for this
    std::vector<int32_t> MaterialIndicies;

    uint32_t VertexCount() const;
    uint32_t FacesCount() const;
    uint32_t MaterialCount() const;

    WraithVertex& AddVertex();
    WraithFace& AddFace();
    void AddFace(uint32_t Index1, uint32_t Index2, uint32_t Index3);
    // Add a new material index, -1 means no material found
    void AddMaterial(int32_t Index);

    void ScaleSubmesh(float ScaleFactor);

    void PrepareMesh(uint32_t VertexCount, uint32_t FaceCount);
};

class WraithMaterial
{
public:
    WraithMaterial();
    WraithMaterial(const std::string& Name, const std::string& Diffuse, const std::string& Normal, const std::string& Specular);

    std::string MaterialName;
    // The file name of the diffuse map for this material
    std::string DiffuseMapName;
    // The file name of the normal map for this material
    std::string NormalMapName;
    // The file name of the specular map for this material
    std::string SpecularMapName;
    // Linear rgb the diffuse map is multiplied by (a material colour constant; glTF's baseColorFactor)
    float ColorFactor[3] = { 1.0f, 1.0f, 1.0f };

    static const WraithMaterial DefaultMaterial;
};

class WraithModel : public WraithAsset
{
public:
    WraithModel();
    ~WraithModel();

    std::vector<std::string> BlendShapes;
    std::vector<WraithBone> Bones;
    std::vector<WraithSubmesh> Submeshes;
    std::vector<WraithMaterial> Materials;

    uint32_t BoneCount() const;
    uint32_t VertexCount() const;
    uint32_t FaceCount() const;
    uint32_t SubmeshCount() const;
    uint32_t MaterialCount() const;

    float LodDistance;
    float LodMaxDistance;

    WraithMaterial& AddMaterial();
    void AddMaterial(WraithMaterial& Material);
    WraithBone& AddBone();
    WraithSubmesh& AddSubmesh();

    void ScaleModel(float ScaleFactor);

    void PrepareSubmeshes(uint32_t SubmeshCount);
    void PrepareBones(uint32_t BoneCount);

    void GenerateLocalPositions(bool Translations, bool Rotations);

    // Part joined into this model (a composite's part, a head on its body, both bound in the same
    // space): a bone of a name this model has is that bone (this model's transform kept), the others
    // are appended under their parent; materials joined by name; Part's submeshes moved in with their
    // weights and materials remapped. MaterialOf, when given, gets each of Part's materials' index
    // here and NewMaterial whether it was appended. Rebind: Part is bound in its own space (a head
    // whose root, j_spine4, is its origin): its vertices are first moved onto this model's bind pose by
    // the bones they are weighted to (each by its bones' this-bind x part-bind^-1, the weights blending
    // them, as a skin bound to this skeleton at rest draws it), its own bones by their nearest shared
    // ancestor's. Returns how many bones were appended.
    size_t Join(WraithModel& Part, std::vector<int32_t>* MaterialOf = nullptr, std::vector<bool>* NewMaterial = nullptr,
                bool Rebind = false);
};