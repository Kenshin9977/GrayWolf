#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <vector>

struct PreviewGeometry;
struct SkeletonPoseBone;

// Linear blend skinning on the GPU (a compute shader), the same sums as SkinPreviewGeometry: the
// anim cells pose their rig every frame without the CPU touching its vertices. A rig's bind pose is
// uploaded once; each pose writes the cell's own vertex buffer (pos3, nrm3, uv2, as the renderer reads).
class GpuSkinning
{
public:
    struct Rig
    {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> Vertices;   // bind pose, bones and weights
        Microsoft::WRL::ComPtr<ID3D11Buffer> Counts;                  // vertex and bone count
        UINT VertexCount = 0, BoneCount = 0;
        bool Valid() const { return Vertices && Counts && VertexCount; }
    };

    // Device only: a loader thread may call it.
    static Rig Upload(ID3D11Device* Dev, const PreviewGeometry& Bind);
    // A vertex buffer the skinning writes (Count vertices), and its view for the shader.
    static bool MakeOutput(ID3D11Device* Dev, UINT VertexCount, Microsoft::WRL::ComPtr<ID3D11Buffer>& Vb,
                           Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>& Uav);

    bool Init(ID3D11Device* Dev);
    // Bind posed by WorldPose (EvaluateSkeletonPose's) into Out.
    void Skin(ID3D11DeviceContext* Ctx, const Rig& R, const PreviewGeometry& Bind, const std::vector<SkeletonPoseBone>& WorldPose,
              ID3D11UnorderedAccessView* Out);

private:
    Microsoft::WRL::ComPtr<ID3D11Device> Dev;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> Cs;
    Microsoft::WRL::ComPtr<ID3D11Buffer> Bones;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> BonesSrv;
    UINT BonesCapacity = 0;
    std::vector<float> Scratch;
};
