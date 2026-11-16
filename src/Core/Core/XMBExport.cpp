#include "stdafx.h"
#include "XMBExport.h"


#include "MemoryWriter.h"
#include "BinaryWriter.h"
#include "Compression.h"
#include "WraithModel.h"
#include "XBin.h"

#include <algorithm>
#include <format>

int16_t ClampFloatToShort(float Value)
{
    return std::clamp<int16_t>((int16_t)(32767 * VectorMath::Clamp(Value, -1.0f, 1.0f)), -32767, 32767);
}

void WriteMetaInt16Block(MemoryWriter& Writer, int16_t Hash, int16_t Value)
{
    Writer.Write(Hash);
    Writer.Write(Value);
}

void WriteMetaUInt16Block(MemoryWriter& Writer, int16_t Hash, uint16_t Value)
{
    Writer.Write(Hash);
    Writer.Write(Value);
}

void WriteMetaUInt32Block(MemoryWriter& Writer, int32_t Hash, uint32_t Value)
{
    Writer.Write(Hash);
    Writer.Write(Value);
}

void WriteMetaFloatBlock(MemoryWriter& Writer, int32_t Hash, float Value)
{
    Writer.Write(Hash);
    Writer.Write(Value);
}

void WriteMetaVec2Block(MemoryWriter& Writer, int32_t Hash, Vector2 Value)
{
    Writer.Write(Hash);
    Writer.Write(Value.X);
    Writer.Write(Value.Y);
}

void WriteMetaVec4Block(MemoryWriter& Writer, int32_t Hash, float X, float Y, float Z, float W)
{
    Writer.Write(Hash);
    Writer.Write(X);
    Writer.Write(Y);
    Writer.Write(Z);
    Writer.Write(W);
}

// the token stream's strings and container are export2bin's, shared with the XANIM_BIN writer (XBin.h)
using XBin::WriteStringAligned;

void WriteCommentBlock(MemoryWriter& Writer, const std::string& Comment)
{
    Writer.Write(0xC355);
    WriteStringAligned(Writer, Comment);
}

void WriteModelBlock(MemoryWriter& Writer)
{
    Writer.Write(0x46C8);
}

void WriteVersionBlock(MemoryWriter& Writer, int16_t Version)
{
    WriteMetaInt16Block(Writer, (int16_t)0x24D1, Version);
}

void WriteBoneCountBlock(MemoryWriter& Writer, int16_t Count)
{
    WriteMetaInt16Block(Writer, (int16_t)0x76BA, Count);
}

void WriteBoneInfoBlock(MemoryWriter& Writer, const WraithBone& Bone, int32_t Index)
{
    Writer.Write(0xF099);
    Writer.Write(Index);
    Writer.Write(Bone.BoneParent);
    WriteStringAligned(Writer, Bone.TagName);
}

void WriteBoneIndexBlock(MemoryWriter& Writer, short Index)
{
    WriteMetaInt16Block(Writer, (int16_t)0xDD9A, Index);
}

void WriteOffsetBlock(MemoryWriter& Writer, Vector3 Offset)
{
    Writer.Write(0x9383);
    Writer.Write(Offset.X);
    Writer.Write(Offset.Y);
    Writer.Write(Offset.Z);
}

void WriteScaleBlock(MemoryWriter& Writer, Vector3 Offset)
{
    Writer.Write(0x1C56);
    Writer.Write(Offset.X);
    Writer.Write(Offset.Y);
    Writer.Write(Offset.Z);
}

void WriteMatrixBlock(MemoryWriter& Writer, Matrix RotationMatrix)
{
    Writer.Write((int16_t)0xDCFD);
    Writer.Write(ClampFloatToShort(RotationMatrix.Mat(0, 0)));
    Writer.Write(ClampFloatToShort(RotationMatrix.Mat(0, 1)));
    Writer.Write(ClampFloatToShort(RotationMatrix.Mat(0, 2)));
    Writer.Write((int16_t)0xCCDC);
    Writer.Write(ClampFloatToShort(RotationMatrix.Mat(1, 0)));
    Writer.Write(ClampFloatToShort(RotationMatrix.Mat(1, 1)));
    Writer.Write(ClampFloatToShort(RotationMatrix.Mat(1, 2)));
    Writer.Write((int16_t)0xFCBF);
    Writer.Write(ClampFloatToShort(RotationMatrix.Mat(2, 0)));
    Writer.Write(ClampFloatToShort(RotationMatrix.Mat(2, 1)));
    Writer.Write(ClampFloatToShort(RotationMatrix.Mat(2, 2)));
}

void WriteVertexCount16Block(MemoryWriter& Writer, uint16_t Count)
{
    WriteMetaUInt16Block(Writer, (int16_t)0x950D, Count);
}

void WriteVertexCount32Block(MemoryWriter& Writer, uint32_t Count)
{
    WriteMetaUInt32Block(Writer, 0x2AEC, Count);
}

void WriteVertexIndex16Block(MemoryWriter& Writer, uint16_t Index)
{
    WriteMetaUInt16Block(Writer, (int16_t)0x8F03, Index);
}

void WriteVertexIndex32Block(MemoryWriter& Writer, uint32_t Index)
{
    WriteMetaUInt32Block(Writer, 0xB097, Index);
}

void WriteVertexWeightCountBlock(MemoryWriter& Writer, int16_t Count)
{
    WriteMetaInt16Block(Writer, (int16_t)0xEA46, Count);
}

void WriteVertexWeightBlock(MemoryWriter& Writer, int16_t BoneIndex, float Weight)
{
    WriteMetaInt16Block(Writer, (int16_t)0xF1AB, BoneIndex);
    Writer.Write(Weight);
}

void WriteFaceInfoBlock(MemoryWriter& Writer, uint8_t MeshIndex, uint8_t MaterialIndex)
{
    Writer.Write((int16_t)0x562F);
    Writer.Write(MeshIndex);
    Writer.Write(MaterialIndex);
}

void WriteFaceVertexNormalBlock(MemoryWriter& Writer, Vector3 Normal)
{
    Writer.Write((int16_t)0x89EC);
    Writer.Write(ClampFloatToShort(Normal.X));
    Writer.Write(ClampFloatToShort(Normal.Y));
    Writer.Write(ClampFloatToShort(Normal.Z));
}

void WriteFaceVertexColorBlock(MemoryWriter& Writer, uint8_t* Color)
{
    Writer.Write(0x6DD8);
    Writer.Write(Color[0]);
    Writer.Write(Color[1]);
    Writer.Write(Color[2]);
    Writer.Write(Color[3]);
}

void WriteFaceVertexUVBlock(MemoryWriter& Writer, uint16_t Layer, Vector2 UV)
{
    Writer.Write((int16_t)0x1AD4);
    Writer.Write(Layer);
    Writer.Write((float)UV.X);
    Writer.Write((float)UV.Y);
}

void WriteFaceVertex16(MemoryWriter& Writer, const WraithSubmesh& Mesh, uint32_t VertexIndex, uint32_t Offset)
{
    WriteVertexIndex16Block(Writer, VertexIndex + Offset);
    WriteFaceVertexNormalBlock(Writer, Mesh.Verticies[VertexIndex].Normal);
    WriteFaceVertexColorBlock(Writer, (uint8_t*)&Mesh.Verticies[VertexIndex].Color);
    WriteFaceVertexUVBlock(Writer, 1, Mesh.Verticies[VertexIndex].UVLayers[0]);
}

void WriteFaceVertex32(MemoryWriter& Writer, const WraithSubmesh& Mesh, uint32_t VertexIndex, uint32_t Offset)
{
    WriteVertexIndex32Block(Writer, VertexIndex + Offset);
    WriteFaceVertexNormalBlock(Writer, Mesh.Verticies[VertexIndex].Normal);
    WriteFaceVertexColorBlock(Writer, (uint8_t*)&Mesh.Verticies[VertexIndex].Color);
    WriteFaceVertexUVBlock(Writer, 1, Mesh.Verticies[VertexIndex].UVLayers[0]);
}

void WriteBones(MemoryWriter& Writer, const WraithModel& Model)
{
    WriteBoneCountBlock(Writer, Model.BoneCount());
    uint32_t BoneIndex = 0;
    for (auto& Bone : Model.Bones)
    {
        WriteBoneInfoBlock(Writer, Bone, BoneIndex);
        BoneIndex++;
    }
    BoneIndex = 0;
    for (auto& Bone : Model.Bones)
    {
        Matrix RotationMatrix = Matrix::CreateFromQuaternion(Bone.GlobalRotation);
        WriteBoneIndexBlock(Writer, BoneIndex);
        WriteOffsetBlock(Writer, Bone.GlobalPosition);
        WriteScaleBlock(Writer, Bone.BoneScale);
        WriteMatrixBlock(Writer, RotationMatrix);
        BoneIndex++;
    }
}

void WriteMeshData(MemoryWriter& Writer, const WraithModel& Model)
{
    auto VertexCount = Model.VertexCount();
    auto FaceCount = Model.FaceCount();
    if (VertexCount > UINT16_MAX)
    {
        WriteVertexCount32Block(Writer, VertexCount);
    }
    else
    {
        WriteVertexCount16Block(Writer, (uint16_t)VertexCount);
    }
    std::unordered_set<int32_t> MaterialIndicies;
    uint32_t VertexIndex = 0;
    for (auto& Submesh : Model.Submeshes)
    {
        MaterialIndicies.insert(Submesh.MaterialIndicies[0]);
        for (auto& Vertex : Submesh.Verticies)
        {
            if (VertexCount > UINT16_MAX)
            {
                WriteVertexIndex32Block(Writer, VertexIndex);
            }
            else
            {
                WriteVertexIndex16Block(Writer, VertexIndex);
            }
            WriteOffsetBlock(Writer, Vertex.Position);
            if (Vertex.WeightCount() == 1)
            {
                WriteVertexWeightCountBlock(Writer, 1);
                WriteVertexWeightBlock(Writer, Vertex.Weights[0].BoneIndex, 1.0);
            }
            else
            {
                WriteVertexWeightCountBlock(Writer, (int16_t)Vertex.WeightCount());
                for (auto& Weight : Vertex.Weights)
                {
                    WriteVertexWeightBlock(Writer, Weight.BoneIndex, Weight.Weight);
                }
            }
            VertexIndex++;
        }
    }
    uint32_t SubmeshIndex = 0;
    VertexIndex = 0;
    WriteMetaUInt32Block(Writer, 0xBE92, FaceCount);
    for (auto& Submesh : Model.Submeshes)
    {
        auto FindResult = MaterialIndicies.find(Submesh.MaterialIndicies[0]);
        uint32_t IndexOfMaterial = (FindResult != MaterialIndicies.end()) ? (uint32_t)std::distance(MaterialIndicies.begin(), FindResult) : 0;
        for (auto& Face : Submesh.Faces)
        {
            WriteFaceInfoBlock(Writer, (uint8_t)SubmeshIndex, (uint8_t)IndexOfMaterial);
            if (VertexCount > UINT16_MAX)
            {
                WriteFaceVertex32(Writer, Submesh, Face.Index3, VertexIndex);
                WriteFaceVertex32(Writer, Submesh, Face.Index1, VertexIndex);
                WriteFaceVertex32(Writer, Submesh, Face.Index2, VertexIndex);
            }
            else
            {
                WriteFaceVertex16(Writer, Submesh, Face.Index3, VertexIndex);
                WriteFaceVertex16(Writer, Submesh, Face.Index1, VertexIndex);
                WriteFaceVertex16(Writer, Submesh, Face.Index2, VertexIndex);
            }
        }
        VertexIndex += Submesh.VertexCount();
        SubmeshIndex++;
    }
}

void WriteMetaObjectInfo(MemoryWriter& Writer, int16_t Index, std::string Name)
{
    Writer.Write((int16_t)0x87D4);
    Writer.Write(Index);
    WriteStringAligned(Writer, Name);
}

void WriteObjects(MemoryWriter& Writer, const WraithModel& Model)
{
    WriteMetaInt16Block(Writer, (int16_t)0x62AF, (int16_t)Model.SubmeshCount());
    for (uint32_t i = 0; i < Model.SubmeshCount(); i++)
    {
        WriteMetaObjectInfo(Writer, i, std::format("WraithMesh_{}", i));
    }
}

void WriteMaterialInfoBlock(MemoryWriter& Writer, const WraithMaterial& Material, int16_t Index)
{
    Writer.Write((int16_t)0xA700);
    Writer.Write(Index);
    WriteStringAligned(Writer, Material.MaterialName);
    WriteStringAligned(Writer, "lambert");

    // Ensure we don't exceed 128 characters, APE (and probably Linker) limits it...
    if(Material.DiffuseMapName.size() < 112)
        WriteStringAligned(Writer, std::format("color:{}", Material.DiffuseMapName.c_str()));
    else
        WriteStringAligned(Writer, "");
}

void WriteMaterials(MemoryWriter& Writer, const WraithModel& Model)
{
    uint8_t Color[4]{ 255, 255, 255, 255 };
    WriteMetaInt16Block(Writer, (int16_t)0xA1B2, (int16_t)Model.MaterialCount());
    for (int i = 0; i < Model.MaterialCount(); i++)
    {
        WriteMaterialInfoBlock(Writer, Model.Materials[i], (int16_t)i);
        // Write Default Info (Never used for T7, material set ups handle it)
        WriteFaceVertexColorBlock(Writer, (uint8_t*)&Color);
        WriteMetaVec4Block(Writer, 0x6DAB, 0, 0, 0, 1);
        WriteMetaVec4Block(Writer, 0x37FF, 0, 0, 0, 1);
        WriteMetaVec4Block(Writer, 0x4265, 0, 0, 0, 1);
        WriteMetaVec2Block(Writer, 0xC835, Vector2(0.8f, 0));
        WriteMetaVec2Block(Writer, 0xFE0C, Vector2(0, 0));
        WriteMetaVec2Block(Writer, 0x7E24, Vector2(6, 1));
        WriteMetaVec4Block(Writer, 0x317C, -1, -1, -1, 1);
        WriteMetaVec4Block(Writer, 0xE593, -1, -1, -1, 1);
        WriteMetaVec2Block(Writer, 0x7D76, Vector2(-1, -1));
        WriteMetaVec2Block(Writer, 0x83C7, Vector2(-1, -1));
        WriteMetaFloatBlock(Writer, 0x5CD2, -1.0f);
    }
}

void CodXMB::ExportXMB(const WraithModel& Model, const std::string& FileName)
{
    // Create a new writer (8MB of memory, reallocates if needed)
    auto Writer = MemoryWriter(0x800000);
    WriteCommentBlock(Writer, "Generated by Greyhound - A fork of Wraith Archon");
    WriteCommentBlock(Writer, "Please credit DTZxPorter & Scobalula for using it!");
    WriteModelBlock(Writer);
    WriteVersionBlock(Writer, 7);
    WriteBones(Writer, Model);
    WriteMeshData(Writer, Model);
    WriteObjects(Writer, Model);
    WriteMaterials(Writer, Model);
    XBin::SaveContainer(FileName, Writer.GetCurrentStream(), (uint32_t)Writer.GetPosition());
}
