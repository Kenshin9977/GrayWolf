#include "stdafx.h"

#include "WraithModel.h"

WraithBone::WraithBone()
{
    TagName = "";
    BoneParent = -1;
    LocalPosition = Vector3(0, 0, 0);
    LocalRotation = Quaternion::Identity();
    GlobalPosition = Vector3(0, 0, 0);
    GlobalRotation = Quaternion::Identity();
    BoneScale = Vector3(1, 1, 1);
    IsCosmetic = false;
}

WraithFace::WraithFace()
{
    Index1 = 0;
    Index2 = 0;
    Index3 = 0;
}

WraithFace::WraithFace(uint32_t Vert1, uint32_t Vert2, uint32_t Vert3)
{
    Index1 = Vert1;
    Index2 = Vert2;
    Index3 = Vert3;
}

WraithVertexWeight::WraithVertexWeight()
{
    BoneIndex = 0;
    Weight = 1.0f;
}

WraithVertexWeight::WraithVertexWeight(uint32_t Bone, float Value)
{
    BoneIndex = Bone;
    Weight = Value;
}

WraithVertex::WraithVertex()
{
    Position = Vector3(0, 0, 0);
    Normal = Vector3(0, 0, 0);
}

uint32_t WraithVertex::WeightCount() const
{
    return (uint32_t)Weights.size();
}

uint32_t WraithVertex::UVLayerCount() const
{
    return (uint32_t)UVLayers.size();
}

WraithSubmesh::WraithSubmesh()
{
}

uint32_t WraithSubmesh::VertexCount() const
{
    return (uint32_t)Verticies.size();
}

uint32_t WraithSubmesh::FacesCount() const
{
    return (uint32_t)Faces.size();
}

uint32_t WraithSubmesh::MaterialCount() const
{
    return (uint32_t)MaterialIndicies.size();
}

WraithVertex& WraithSubmesh::AddVertex()
{
    Verticies.emplace_back();
    return Verticies.back();
}

void WraithSubmesh::AddFace(uint32_t Index1, uint32_t Index2, uint32_t Index3)
{
    Faces.emplace_back(Index1, Index2, Index3);
}

WraithFace& WraithSubmesh::AddFace()
{
    Faces.emplace_back();
    return Faces.back();
}

void WraithSubmesh::AddMaterial(int32_t Index)
{
    MaterialIndicies.push_back(Index);
}

void WraithSubmesh::PrepareMesh(uint32_t VertexCount, uint32_t FaceCount)
{
    Verticies.reserve(VertexCount);
    Faces.reserve(FaceCount);
}

void WraithVertex::AddVertexWeight(uint32_t BoneIndex, float Weight)
{
    Weights.emplace_back(BoneIndex, Weight);
}

void WraithVertex::AddUVLayer(float UVU, float UVV)
{
    UVLayers.emplace_back(UVU, UVV);
}

void WraithSubmesh::ScaleSubmesh(float ScaleFactor)
{
    for (auto& Vertex : Verticies)
    {
        Vertex.Position *= ScaleFactor;
    }
}

WraithMaterial::WraithMaterial()
{
    MaterialName = "wraith_material";
    DiffuseMapName = "";
    NormalMapName = "";
    SpecularMapName = "";
}

WraithMaterial::WraithMaterial(const std::string& Name, const std::string& Diffuse, const std::string& Normal, const std::string& Specular)
{
    MaterialName = Name;
    DiffuseMapName = Diffuse;
    NormalMapName = Normal;
    SpecularMapName = Specular;
}

const WraithMaterial WraithMaterial::DefaultMaterial = WraithMaterial("wraith_no_material", "", "", "");

WraithModel::WraithModel()
{
    AssetType = WraithAssetType::Model;

    LodDistance = 100.0f;
    LodMaxDistance = 200.0f;
}

WraithModel::~WraithModel()
{
}

uint32_t WraithModel::BoneCount() const
{
    return (uint32_t)Bones.size();
}

uint32_t WraithModel::VertexCount() const
{
    uint32_t Buffer = 0;

    for (auto& Submesh : Submeshes)
    {
        Buffer += (uint32_t)Submesh.Verticies.size();
    }

    return Buffer;
}

uint32_t WraithModel::FaceCount() const
{
    uint32_t Buffer = 0;

    for (auto& Submesh : Submeshes)
    {
        Buffer += (uint32_t)Submesh.Faces.size();
    }

    return Buffer;
}

uint32_t WraithModel::SubmeshCount() const
{
    return (uint32_t)Submeshes.size();
}

uint32_t WraithModel::MaterialCount() const
{
    return (uint32_t)Materials.size();
}

WraithMaterial& WraithModel::AddMaterial()
{
    Materials.emplace_back();
    return Materials.back();
}

void WraithModel::AddMaterial(WraithMaterial& Material)
{
    Materials.push_back(std::move(Material));
}

WraithBone& WraithModel::AddBone()
{
    Bones.emplace_back();
    return Bones.back();
}

WraithSubmesh& WraithModel::AddSubmesh()
{
    Submeshes.emplace_back();
    return Submeshes.back();
}

void WraithModel::ScaleModel(float ScaleFactor)
{
    for (auto& Bone : Bones)
    {
        Bone.LocalPosition *= ScaleFactor;
        Bone.GlobalPosition *= ScaleFactor;
    }

    for (auto& Submesh : Submeshes)
    {
        Submesh.ScaleSubmesh(ScaleFactor);
    }
}

void WraithModel::PrepareSubmeshes(uint32_t SubmeshCount)
{
    Submeshes.reserve(SubmeshCount);
}

void WraithModel::PrepareBones(uint32_t BoneCount)
{
    Bones.reserve(BoneCount);
}

void WraithModel::GenerateLocalPositions(bool Translations, bool Rotations)
{
    auto BoneCount = (uint32_t)Bones.size();

    for (uint32_t i = 0; i < BoneCount; i++)
    {
        if (Bones[i].BoneParent != -1)
        {
            if (Translations)
            {
                Matrix ParentGlobalMatrix = Matrix::CreateFromQuaternion(~Bones[Bones[i].BoneParent].GlobalRotation);
                Bones[i].LocalPosition = Matrix::TransformVector(Bones[i].GlobalPosition - Bones[Bones[i].BoneParent].GlobalPosition, ParentGlobalMatrix);
            }

            // Local rotation is inverse parent global * our global
            if (Rotations) { Bones[i].LocalRotation = (~Bones[Bones[i].BoneParent].GlobalRotation * Bones[i].GlobalRotation); }
        }
        else
        {
            if (Translations) { Bones[i].LocalPosition = Bones[i].GlobalPosition; }
            if (Rotations) { Bones[i].LocalRotation = Bones[i].GlobalRotation; }
        }
    }
}
