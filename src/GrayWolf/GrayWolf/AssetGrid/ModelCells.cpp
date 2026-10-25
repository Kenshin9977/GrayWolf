#include "stdafx.h"

#include "CellRenderers.h"
#include "GridServices.h"
#include "ModelPreview.h"      // LoadPreviewGeometry(Glb), LoadRigGeometry, PreviewGeometry
#include "ModelPreviewGPU.h"   // UploadPreviewMesh, PreviewRenderer
#include "SkeletalPreview.h"   // EvaluateSkeletonPose
#include "GpuSkinning.h"       // the anim cells' skinning

#include "SeAnimReader.h"      // SeAnimClip
#include "CastReader.h"        // ReadAnimClip

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace
{
    // Thumbnails need little texture: each map starts at its first mip within this.
    constexpr int ThumbTextureSize = 256;
    constexpr int MapStillPixels = 256;
    // A cell turns this fast (radians a second), from this angle.
    constexpr float TurnSpeed = 0.55f, StartYaw = 0.9f, ModelPitch = 0.3f, MapPitch = 0.75f;
    constexpr double AnimInterval = 1.0 / 30.0;   // an anim cell's pose is skinned at most this often
    constexpr size_t MaxMeshes = 192;             // models kept on the GPU, the least recently seen go first
    constexpr size_t MaxCells = 4096;
    constexpr size_t MaxRigs = 24;               // anim rigs kept loaded
    constexpr float Framing = 1.0f;               // FrameForTurn's distance (0.82 of the viewer's for the rig)
    const float Background[4] = { 0.135f, 0.14f, 0.155f, 1.0f };

    // The last LOD exported beside a model's file (name_LOD0 -> name_LOD3 when that one is there).
    std::string LowestLod(const std::string& Path)
    {
        const std::filesystem::path P(Path);
        const std::string Stem = P.stem().string();
        const auto Lp = Stem.rfind("_LOD");
        if (Lp == std::string::npos || Lp + 4 >= Stem.size() || Stem.find_first_not_of("0123456789", Lp + 4) != std::string::npos) return Path;
        const int Own = std::atoi(Stem.c_str() + Lp + 4);
        std::error_code Ec;
        for (int L = 7; L > Own; L--)
        {
            const auto Other = P.parent_path() / (Stem.substr(0, Lp + 4) + std::to_string(L) + P.extension().string());
            if (std::filesystem::exists(Other, Ec)) return Other.string();
        }
        return Path;
    }

    void DrawSpinner(ImDrawList* DL, const ImVec2& Min, const ImVec2& Max, double Time)
    {
        const float R = std::max(4.0f, (Max.x - Min.x) * 0.06f);
        const ImVec2 C(Max.x - R * 2.0f, Max.y - R * 2.0f);
        const float T = (float)Time * 6.0f;
        DL->PathArcTo(C, R, T, T + 4.2f, 16);
        DL->PathStroke(IM_COL32(120, 170, 230, 220), 0, 2.0f);
    }

    struct RigModel
    {
        PreviewGeometry Bind;
        PreviewGpuMesh Gpu;
        GpuSkinning::Rig Skin;
        std::vector<int> SkinBones;  // the bones some vertex follows (not a tag left at the origin)
        float BoneCenter[3] = {};    // their centroid in the bind pose
    };

    // The centroid of Bones' positions (Pos of bone b at Pos + b * Stride).
    void BoneBoxCenter(const float* Pos, size_t Stride, const std::vector<int>& Bones, float Out[3])
    {
        double Sum[3] = {};
        for (const int b : Bones)
            for (int c = 0; c < 3; c++) Sum[c] += Pos[b * Stride + c];
        for (int c = 0; c < 3; c++) Out[c] = Bones.empty() ? 0.0f : (float)(Sum[c] / Bones.size());
    }

    // The view a turning cell needs: the box of the triangles drawn, without the outer 1% of their
    // area on each axis (a few small stray pieces far off made the rest tiny), and the distance at
    // which that box stays whole at every yaw of the turn at Pitch.
    void FrameForTurn(const PreviewGeometry& Geo, float Pitch, PreviewGpuMesh& M)
    {
        const size_t N = Geo.Pos.size() / 3, Tris = Geo.Idx.size() / 3;
        if (N == 0 || Tris == 0) return;
        const size_t Step = std::max<size_t>(1, Tris / 200000);
        struct Tri { float C[3]; float Area; };
        std::vector<Tri> T;
        double Total = 0;
        for (size_t t = 0; t < Tris; t += Step)
        {
            const uint32_t I[3] = { Geo.Idx[t * 3], Geo.Idx[t * 3 + 1], Geo.Idx[t * 3 + 2] };
            if (I[0] >= N || I[1] >= N || I[2] >= N) continue;
            const float* A = &Geo.Pos[I[0] * 3]; const float* B = &Geo.Pos[I[1] * 3]; const float* C = &Geo.Pos[I[2] * 3];
            const float U[3] = { B[0] - A[0], B[1] - A[1], B[2] - A[2] }, V[3] = { C[0] - A[0], C[1] - A[1], C[2] - A[2] };
            const float X[3] = { U[1] * V[2] - U[2] * V[1], U[2] * V[0] - U[0] * V[2], U[0] * V[1] - U[1] * V[0] };
            Tri E{ { (A[0] + B[0] + C[0]) / 3, (A[1] + B[1] + C[1]) / 3, (A[2] + B[2] + C[2]) / 3 },
                   0.5f * std::sqrt(X[0] * X[0] + X[1] * X[1] + X[2] * X[2]) + 1e-9f };
            Total += E.Area;
            T.push_back(E);
        }
        if (T.empty()) return;
        float Lo[3], Hi[3];
        for (int c = 0; c < 3; c++)
        {
            std::sort(T.begin(), T.end(), [c](const Tri& X, const Tri& Y) { return X.C[c] < Y.C[c]; });
            double Acc = 0;
            size_t i = 0;
            for (; i + 1 < T.size() && (Acc += T[i].Area) < Total * 0.01; i++) {}
            Lo[c] = T[i].C[c];
            Acc = 0;
            size_t j = T.size() - 1;
            for (; j > i && (Acc += T[j].Area) < Total * 0.01; j--) {}
            Hi[c] = T[j].C[c];
        }
        // the centroids' box is a little inside the surfaces' own: half a typical triangle more
        const float Pad = std::sqrt((float)(Total / T.size())) * 0.5f;
        for (int c = 0; c < 3; c++) { Lo[c] -= Pad; Hi[c] += Pad; }
        const float Half[3] = { (Hi[0] - Lo[0]) * 0.5f, (Hi[1] - Lo[1]) * 0.5f, (Hi[2] - Lo[2]) * 0.5f };
        const float Radius = std::max(1e-3f, std::sqrt(Half[0] * Half[0] + Half[1] * Half[1] + Half[2] * Half[2]));
        const float Tan = std::tan(DirectX::XMConvertToRadians(35.0f) * 0.5f);
        const float Cp = std::cos(Pitch), Sp = std::sin(Pitch);
        float Need = 0;
        for (int k = 0; k < 24; k++)
        {
            const float Y = k * DirectX::XM_2PI / 24;
            const float D[3] = { Cp * std::cos(Y), Cp * std::sin(Y), Sp };                 // centre to camera
            const float R[3] = { -std::sin(Y), std::cos(Y), 0 };                           // right
            const float U[3] = { -Sp * std::cos(Y), -Sp * std::sin(Y), Cp };              // up
            for (int Corner = 0; Corner < 8; Corner++)
            {
                const float P[3] = { (Corner & 1) ? Half[0] : -Half[0], (Corner & 2) ? Half[1] : -Half[1], (Corner & 4) ? Half[2] : -Half[2] };
                const float Depth = P[0] * D[0] + P[1] * D[1] + P[2] * D[2];
                const float X = std::abs(P[0] * R[0] + P[1] * R[1]), Yv = std::abs(P[0] * U[0] + P[1] * U[1] + P[2] * U[2]);
                Need = std::max(Need, Depth + std::max(X, Yv) / Tan);
            }
        }
        M.Center = DirectX::XMVectorSet((Lo[0] + Hi[0]) * 0.5f, (Lo[1] + Hi[1]) * 0.5f, (Lo[2] + Hi[2]) * 0.5f, 0);
        M.Radius = Radius;
        M.FrameDist = std::max(Need * 1.06f, Radius * 0.5f);
    }
}

struct ModelCellRenderer::Impl
{
    struct Cell
    {
        int Kind = 0;
        AssetGridItem Item;            // its strings point at the copies below
        std::string Path, Name, Format;
        uint64_t SeenFrame = 0;
        double LastSeen = 0;

        ComPtr<ID3D11ShaderResourceView> Still;   // the disk cache's, or the first render's
        bool StillLooked = false, StillFromDisk = false, StillSaving = false;

        std::shared_ptr<const PreviewGpuMesh> Mesh;
        bool MeshFailed = false;

        // the cell's own render target (what it shows), rendered through the shared scratch target
        ComPtr<ID3D11Texture2D> Live;
        ComPtr<ID3D11ShaderResourceView> LiveSRV;
        int LivePixels = 0;
        bool LiveValid = false;
        double Start = -1, LastRender = -1;

        std::shared_ptr<const SeAnimClip> Clip;
        bool ClipFailed = false;
        std::string RigKey;                       // the rig it plays on (Rigs' key), empty while not known
        bool RigKnown = false, NoRig = false;     // its pick is in; nothing fits it
        const RigModel* PosedFor = nullptr;       // the rig Posed was made for, and framed on
        const RigModel* FramedFor = nullptr;
        float FirstHeight = 0;
        PreviewGpuMesh Posed;                     // the rig's mesh with this cell's own vertex buffer
        ComPtr<ID3D11UnorderedAccessView> PosedUav;   // that buffer, written by the skinning
        bool MapDone = false;
    };

    std::unordered_map<std::string, Cell> Cells;
    std::vector<Cell*> Visible;

    // the rigs anims play on, by their models' paths joined with '|' (one rig serves every anim picking it)
    struct RigSlot
    {
        std::shared_ptr<const RigModel> Model;
        bool Failed = false;
        uint64_t SeenFrame = 0;
    };
    std::unordered_map<std::string, RigSlot> Rigs;
    std::string Forced;                 // SetRig: every anim plays on it
    ModelCellRenderer::RigResolver Resolve;

    bool WasSettled = true, Turning = true;
    PreviewLighting Lighting;
    std::string LightKey;      // added to the disk stills' variant: empty for the default lighting
    uint64_t LightGen = 0;     // a still read back under older lighting is dropped
    size_t LastVisible = 0, LastLive = 0, LastMeshes = 0;
    int LastRenders = 0;
    double PoseMs = 0; size_t Poses = 0;   // anim poses evaluated so far, and their time (CPU)
    GpuSkinning Skinner;

    Cell& CellFor(const AssetGridItem& A)
    {
        Cell& C = Cells[*A.Path];
        if (C.Path.empty())
        {
            C.Kind = A.Kind; C.Path = *A.Path; C.Name = A.Name ? *A.Name : std::string(); C.Format = A.Format ? *A.Format : std::string();
            C.Item = A; C.Item.Path = &C.Path; C.Item.Name = &C.Name; C.Item.Format = &C.Format;
        }
        return C;
    }

    Cell* Find(const std::string& Path)
    {
        const auto It = Cells.find(Path);
        return It == Cells.end() ? nullptr : &It->second;
    }

    // The disk cache's still, looked up once per cell.
    void WantStill(AssetCellContext& X, Cell& C, const char* Variant)
    {
        if (C.StillLooked) return;
        GridServices* S = &X.Services;
        const std::string File = ThumbCache::FileFor(C.Item, Variant), Path = C.Path;
        const uint64_t Gen = LightGen;
        X.Services.Jobs.Want("still|" + Path, 0, false, [this, S, File, Path, Gen] {
            return [this, S, File, Path, Gen](const GridJobs::StaleFn&) {
                int W = 0, H = 0; std::vector<uint8_t> Px;
                ComPtr<ID3D11ShaderResourceView> Srv;
                if (ThumbCache::Load(File, W, H, Px)) Srv = MakeRgbaSRV(S->Dev, W, H, Px);
                S->Jobs.PostUi([this, Path, Srv, Gen] {
                    if (Cell* C = Find(Path); C && Gen == LightGen) { C->StillLooked = true; C->Still = Srv; C->StillFromDisk = Srv != nullptr; }
                });
            };
        });
    }

    void WantMesh(AssetCellContext& X, Cell& C)
    {
        if (C.Mesh || C.MeshFailed) return;
        GridServices* S = &X.Services;
        const std::string Path = C.Path;
        const bool Map = C.Kind == AssetKind::Map;
        X.Services.Jobs.Want((Map ? "map|" : "model|") + Path, Map ? 2 : 1, Map, [this, S, Path, Map] {
            return [this, S, Path, Map](const GridJobs::StaleFn& Stale) {
                PreviewGeometry Geo;
                const bool Ok = Map ? LoadPreviewGeometryGlb(Path, Geo) : LoadPreviewGeometry({ LowestLod(Path) }, Geo, Stale);
                if (Stale()) return;   // scrolled past: asked again when seen
                std::shared_ptr<PreviewGpuMesh> Mesh;
                if (Ok)
                {
                    Mesh = std::make_shared<PreviewGpuMesh>(UploadPreviewMesh(S->Dev, Geo, ThumbTextureSize));
                    FrameForTurn(Geo, Map ? MapPitch : ModelPitch, *Mesh);
                }
                const bool Valid = Mesh && Mesh->Valid;
                S->Jobs.PostUi([this, Path, Mesh, Valid] {
                    if (Cell* C = Find(Path)) { if (Valid) C->Mesh = Mesh; else C->MeshFailed = true; }
                });
            };
        });
    }

    RigSlot* FindRig(const std::string& Key)
    {
        const auto It = Rigs.find(Key);
        return It == Rigs.end() ? nullptr : &It->second;
    }

    void WantRig(AssetCellContext& X, const std::string& Key)
    {
        RigSlot& Slot = Rigs[Key];
        Slot.SeenFrame = X.Frame;
        if (Slot.Model || Slot.Failed) return;
        GridServices* S = &X.Services;
        std::vector<std::string> Paths;
        for (size_t From = 0; From <= Key.size();)
        {
            const size_t Bar = std::min(Key.find('|', From), Key.size());
            if (Bar > From) Paths.push_back(Key.substr(From, Bar - From));
            From = Bar + 1;
        }
        X.Services.Jobs.Want("rig|" + Key, 1, false, [this, S, Key, Paths] {
            return [this, S, Key, Paths](const GridJobs::StaleFn&) {
                auto R = std::make_shared<RigModel>();
                const bool Ok = LoadRigGeometry(Paths, R->Bind);
                if (Ok)
                {
                    R->Gpu = UploadPreviewMesh(S->Dev, R->Bind, ThumbTextureSize);
                    R->Skin = GpuSkinning::Upload(S->Dev, R->Bind);
                    FrameForTurn(R->Bind, ModelPitch, R->Gpu);
                    R->Gpu.FrameDist *= 1.15f;   // an anim reaches past its bind pose
                    std::vector<char> Used(R->Bind.Bones.size(), 0);
                    for (size_t k = 0; k < R->Bind.VtxBoneIdx.size() && k < R->Bind.VtxBoneWeight.size(); k++)
                        if (R->Bind.VtxBoneIdx[k] >= 0 && (size_t)R->Bind.VtxBoneIdx[k] < Used.size() && R->Bind.VtxBoneWeight[k] > 0) Used[R->Bind.VtxBoneIdx[k]] = 1;
                    std::vector<float> BonePos;
                    for (size_t b = 0; b < R->Bind.Bones.size(); b++)
                    {
                        BonePos.insert(BonePos.end(), R->Bind.Bones[b].Pos, R->Bind.Bones[b].Pos + 3);
                        if (Used[b]) R->SkinBones.push_back((int)b);
                    }
                    BoneBoxCenter(BonePos.data(), 3, R->SkinBones, R->BoneCenter);
                }
                const bool Valid = Ok && R->Gpu.Valid && R->Skin.Valid();
                S->Jobs.PostUi([this, R, Valid, Key] {
                    if (RigSlot* Slot = FindRig(Key)) { if (Valid) Slot->Model = R; else Slot->Failed = true; }
                });
            };
        });
    }

    void WantClip(AssetCellContext& X, Cell& C)
    {
        if (C.Clip || C.ClipFailed) return;
        GridServices* S = &X.Services;
        const std::string Path = C.Path;
        X.Services.Jobs.Want("clip|" + Path, 1, false, [this, S, Path] {
            return [this, S, Path](const GridJobs::StaleFn&) {
                auto Clip = std::make_shared<SeAnimClip>();
                const bool Ok = ReadAnimClip(Path, *Clip) && Clip->FrameCount > 0;
                S->Jobs.PostUi([this, Path, Clip, Ok] {
                    if (Cell* C = Find(Path)) { if (Ok) C->Clip = Clip; else C->ClipFailed = true; }
                });
            };
        });
    }

    // The cell's own texture at Pixels square (a new one when the size changed).
    bool EnsureLive(GridServices& S, Cell& C, int Pixels)
    {
        if (C.Live && C.LivePixels == Pixels) return true;
        if (C.Live) { S.Retire(C.Live); S.Retire(C.LiveSRV); }
        C.Live.Reset(); C.LiveSRV.Reset(); C.LiveValid = false;
        D3D11_TEXTURE2D_DESC Td{}; Td.Width = Pixels; Td.Height = Pixels; Td.MipLevels = 1; Td.ArraySize = 1;
        Td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; Td.SampleDesc.Count = 1; Td.Usage = D3D11_USAGE_DEFAULT; Td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(S.Dev->CreateTexture2D(&Td, nullptr, &C.Live)) || FAILED(S.Dev->CreateShaderResourceView(C.Live.Get(), nullptr, &C.LiveSRV)))
        {
            C.Live.Reset(); C.LiveSRV.Reset();
            return false;
        }
        C.LivePixels = Pixels;
        return true;
    }

    // Mesh seen from Yaw into the scratch target, copied into Into.
    bool RenderInto(GridServices& S, const PreviewGpuMesh& Mesh, float Yaw, float Pitch, int Pixels, ID3D11Texture2D* Into)
    {
        PreviewRenderer* R = S.Renderer();
        PreviewTarget* T = S.Scratch(Pixels);
        if (!R || !T || !Into) return false;
        R->Render(S.Ctx, &Mesh, OrbitPreviewCamera(Mesh, Yaw, Pitch, Framing), *T, Background);
        S.Ctx->CopyResource(Into, T->Output.Get());
        S.Rendered();
        return true;
    }

    // The first render of a model, kept as its still: in memory, and on disk for the next time.
    void KeepStill(GridServices& S, Cell& C, const char* Variant, ID3D11Texture2D* From)
    {
        if (C.StillFromDisk || C.StillSaving) return;
        const std::string Path = C.Path, File = ThumbCache::FileFor(C.Item, Variant);
        GridServices* SP = &S;
        const uint64_t Gen = LightGen;
        C.StillSaving = S.ReadBack(From, [this, SP, Path, File, Gen](int W, int H, std::vector<uint8_t>&& Px) {
            Cell* C = Find(Path);
            if (!C || Gen != LightGen) return;
            C->Still = MakeRgbaSRV(SP->Dev, W, H, Px);
            C->StillFromDisk = true;   // nothing more to save
            auto Shared = std::make_shared<std::vector<uint8_t>>(std::move(Px));
            SP->Jobs.Always("save|" + File, 0, [File, W, H, Shared](const GridJobs::StaleFn&) { ThumbCache::Save(File, W, H, *Shared); });
        });
    }

    void Evict(GridServices& S, uint64_t Frame, double Now)
    {
        std::vector<Cell*> WithMesh;
        for (auto& [Key, C] : Cells)
        {
            const bool Gone = C.SeenFrame + 1 < Frame;   // not drawn last frame: nothing of it is in flight
            if (Gone && Now - C.LastSeen > 1.5 && C.Live)
            {
                C.Live.Reset(); C.LiveSRV.Reset(); C.LivePixels = 0; C.LiveValid = false; C.Posed = {}; C.PosedUav.Reset();
            }
            if (C.Mesh && Gone) WithMesh.push_back(&C);
        }
        size_t Meshes = WithMesh.size();
        for (const auto& [Key, C] : Cells) if (C.Mesh && C.SeenFrame + 1 >= Frame) Meshes++;
        if (Meshes > MaxMeshes)
        {
            std::sort(WithMesh.begin(), WithMesh.end(), [](const Cell* A, const Cell* B) { return A->LastSeen < B->LastSeen; });
            for (Cell* C : WithMesh)
            {
                if (Meshes <= MaxMeshes) break;
                C->Mesh.reset(); C->Live.Reset(); C->LiveSRV.Reset(); C->LiveValid = false; C->Start = -1;
                Meshes--;
            }
        }
        if (Cells.size() > MaxCells)
        {
            std::vector<std::pair<double, std::string>> Old;
            for (const auto& [Key, C] : Cells) if (C.SeenFrame + 1 < Frame) Old.emplace_back(C.LastSeen, Key);
            std::sort(Old.begin(), Old.end());
            for (size_t i = 0; i < Old.size() && Cells.size() > MaxCells * 3 / 4; i++) Cells.erase(Old[i].second);
        }
        LastMeshes = Meshes;
        // past a few rigs, those no anim on screen plays on go, the longest unseen first
        if (Rigs.size() > MaxRigs)
        {
            std::vector<std::pair<uint64_t, std::string>> Old;
            for (const auto& [Key, R] : Rigs) if (R.SeenFrame + 1 < Frame) Old.emplace_back(R.SeenFrame, Key);
            std::sort(Old.begin(), Old.end());
            for (size_t i = 0; i < Old.size() && Rigs.size() > MaxRigs; i++)
            {
                const RigModel* Gone = Rigs[Old[i].second].Model.get();
                for (auto& [K, C] : Cells)
                    if (Gone && C.PosedFor == Gone) { C.Posed = {}; C.PosedUav.Reset(); C.PosedFor = nullptr; C.FramedFor = nullptr; }
                Rigs.erase(Old[i].second);
            }
        }
        (void)S;
    }

    void RenderModel(GridServices& S, Cell& C, double Now, int Pixels)
    {
        if (!EnsureLive(S, C, Pixels)) return;
        if (C.Start < 0) C.Start = Now;
        const float Yaw = StartYaw + (Turning ? (float)(Now - C.Start) * TurnSpeed : 0.0f);
        if (!RenderInto(S, *C.Mesh, Yaw, ModelPitch, Pixels, C.Live.Get())) return;
        C.LiveValid = true;
        C.LastRender = Now;
        if (!C.StillFromDisk) KeepStill(S, C, ("model" + LightKey).c_str(), C.Live.Get());
    }

    // The rig's mesh with this cell's own vertex buffer, which the skinning writes.
    bool EnsurePosed(GridServices& S, Cell& C, const RigModel& Rig)
    {
        if (C.PosedFor == &Rig && C.Posed.Valid && C.PosedUav) return true;
        C.Posed = Rig.Gpu;
        C.PosedFor = &Rig;
        C.FramedFor = nullptr;
        ComPtr<ID3D11Buffer> Vb;
        if (!GpuSkinning::MakeOutput(S.Dev, Rig.Skin.VertexCount, Vb, C.PosedUav)) { C.Posed = {}; C.PosedUav.Reset(); C.PosedFor = nullptr; return false; }
        C.Posed.VB = Vb;
        return true;
    }

    // C's pose at Now (CPU: the bones), skinned (GPU) into its vertex buffer, rendered.
    void RenderAnim(GridServices& S, Cell& C, const RigModel& Rig, double Now, int Pixels)
    {
        if (!EnsurePosed(S, C, Rig) || !EnsureLive(S, C, Pixels) || !Skinner.Init(S.Dev)) return;
        if (C.Start < 0) C.Start = Now;
        const PreviewGeometry& Bind = Rig.Bind;
        const SeAnimClip& Clip = *C.Clip;
        const float Rate = Clip.FrameRate > 0 ? Clip.FrameRate : 30.0f;
        const float Frame = std::fmod((float)(Now - C.Start) * Rate, (float)std::max(1u, Clip.FrameCount));
        const auto T0 = std::chrono::steady_clock::now();
        std::vector<SkeletonPoseBone> Pose;
        EvaluateSkeletonPose(Bind, Clip, Frame, Pose);
        PoseMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count();
        Poses++;
        if (!Pose.empty())
        {
            // the view follows the bones' centroid across the ground every frame (root motion stays
            // in the cell); its height is the first pose's (a crawl lies low, a jump still rises)
            float Now3[3];
            BoneBoxCenter(Pose[0].Pos, sizeof(SkeletonPoseBone) / sizeof(float), Rig.SkinBones, Now3);
            if (C.FramedFor != &Rig) { C.FirstHeight = Now3[2]; C.FramedFor = &Rig; }
            const DirectX::XMVECTOR Shift = DirectX::XMVectorSet(Now3[0] - Rig.BoneCenter[0], Now3[1] - Rig.BoneCenter[1], C.FirstHeight - Rig.BoneCenter[2], 0);
            C.Posed.Center = DirectX::XMVectorAdd(Rig.Gpu.Center, Shift);
        }
        Skinner.Skin(S.Ctx, Rig.Skin, Bind, Pose, C.PosedUav.Get());
        const float Yaw = StartYaw + (float)(Now - C.Start) * TurnSpeed * 0.5f;
        if (!RenderInto(S, C.Posed, Yaw, ModelPitch, Pixels, C.Live.Get())) return;
        C.LiveValid = true;
        C.LastRender = Now;
    }

    // A map is rendered once, its still kept (memory and disk), its mesh let go.
    void RenderMap(GridServices& S, Cell& C)
    {
        ComPtr<ID3D11Texture2D> Tex;
        D3D11_TEXTURE2D_DESC Td{}; Td.Width = MapStillPixels; Td.Height = MapStillPixels; Td.MipLevels = 1; Td.ArraySize = 1;
        Td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; Td.SampleDesc.Count = 1; Td.Usage = D3D11_USAGE_DEFAULT; Td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11ShaderResourceView> Srv;
        if (FAILED(S.Dev->CreateTexture2D(&Td, nullptr, &Tex)) || FAILED(S.Dev->CreateShaderResourceView(Tex.Get(), nullptr, &Srv))) return;
        if (!RenderInto(S, *C.Mesh, StartYaw, MapPitch, MapStillPixels, Tex.Get())) return;
        C.Still = Srv;
        KeepStill(S, C, ("map" + LightKey).c_str(), Tex.Get());
        C.Mesh.reset();
        C.MapDone = true;
    }
};

ModelCellRenderer::ModelCellRenderer() : P(std::make_unique<Impl>()) {}
ModelCellRenderer::~ModelCellRenderer() = default;

void ModelCellRenderer::SetRig(const std::string& ModelPath) { P->Forced = ModelPath; }

void ModelCellRenderer::SetRigResolver(RigResolver Resolve) { P->Resolve = std::move(Resolve); }

void ModelCellRenderer::SetTurning(bool On) { P->Turning = On; }

void ModelCellRenderer::SetLighting(const PreviewLighting& L)
{
    const PreviewLighting& O = P->Lighting;
    if (L.Exposure == O.Exposure && L.Lights == O.Lights && L.Specular == O.Specular) return;
    P->Lighting = L;
    const PreviewLighting Default;
    char Key[64] = {};
    if (L.Exposure != Default.Exposure || L.Lights != Default.Lights || L.Specular != Default.Specular)
        snprintf(Key, sizeof(Key), "|e%.2f|l%.2f|s%d", L.Exposure, L.Lights, L.Specular ? 1 : 0);
    P->LightKey = Key;
    P->LightGen++;
    // every picture again: the turning ones at their next render, the stills (maps) from the cache or a new render
    for (auto& [Path, C] : P->Cells)
    {
        C.LiveValid = false;
        C.Still.Reset(); C.StillLooked = false; C.StillFromDisk = false; C.StillSaving = false; C.MapDone = false;
    }
}

void ModelCellRenderer::BeginFrame(AssetCellContext& X)
{
    P->Visible.clear();
    P->Evict(X.Services, X.Frame, X.Time);
}

void ModelCellRenderer::Draw(AssetCellContext& X, const AssetGridItem& A, ImDrawList* DL, const ImVec2& Min, const ImVec2& Max, bool)
{
    Impl::Cell& C = P->CellFor(A);
    C.SeenFrame = X.Frame;
    C.LastSeen = X.Time;
    P->Visible.push_back(&C);

    bool Loading = false;
    if (A.Kind == AssetKind::Model)
    {
        P->WantStill(X, C, ("model" + P->LightKey).c_str());
        P->WantMesh(X, C);
        Loading = !C.MeshFailed && !C.LiveValid;
    }
    else if (A.Kind == AssetKind::Map)
    {
        P->WantStill(X, C, ("map" + P->LightKey).c_str());
        if (C.StillLooked && !C.Still && !C.MapDone) P->WantMesh(X, C);
        Loading = !C.Still && !C.MeshFailed;
    }
    else if (A.Kind == AssetKind::Anim && (!P->Forced.empty() || P->Resolve))
    {
        // the rig picked for it (asked each frame: a pick can change), loaded once per rig
        std::vector<std::string> Paths;
        C.RigKnown = true;
        if (!P->Forced.empty()) Paths = { P->Forced };
        else C.RigKnown = P->Resolve(C.Path, Paths);
        if (C.RigKnown)
        {
            std::string Key;
            for (const auto& Pth : Paths) Key += (Key.empty() ? "" : "|") + Pth;
            if (Key != C.RigKey) { C.RigKey = Key; C.LiveValid = false; C.Start = -1; }
            C.NoRig = Key.empty();
        }
        if (!C.RigKey.empty())
        {
            P->WantRig(X, C.RigKey);
            P->WantClip(X, C);
        }
        const Impl::RigSlot* Slot = P->FindRig(C.RigKey);
        Loading = !C.NoRig && !(Slot && Slot->Failed) && !C.ClipFailed && !C.LiveValid;
    }

    ID3D11ShaderResourceView* Show = C.LiveValid ? C.LiveSRV.Get() : C.Still.Get();
    if (Show) DL->AddImage((ImTextureID)(intptr_t)Show, Min, Max);
    else DrawAssetKindIcon(DL, Min, Max, A.Kind);
    if (Loading) DrawSpinner(DL, Min, Max, X.Time);
}

void ModelCellRenderer::EndFrame(AssetCellContext& X)
{
    GridServices& S = X.Services;
    const int Pixels = X.ThumbPixels;
    if (PreviewRenderer* R = S.Renderer()) R->SetLighting(P->Lighting);
    // the stalest first: over budget, every cell still turns, only less often
    std::vector<Impl::Cell*> Order = P->Visible;
    std::sort(Order.begin(), Order.end(), [](const Impl::Cell* A, const Impl::Cell* B) { return A->LastRender < B->LastRender; });
    size_t Live = 0;
    bool Settled = true;
    for (Impl::Cell* C : Order)
    {
        if (C->Kind == AssetKind::Model)
        {
            if (C->Mesh && S.CanRender()) P->RenderModel(S, *C, X.Time, Pixels);
            Settled &= C->LiveValid || C->MeshFailed;
        }
        else if (C->Kind == AssetKind::Anim)
        {
            if (P->Forced.empty() && !P->Resolve) continue;
            const Impl::RigSlot* Slot = P->FindRig(C->RigKey);
            if (Slot && Slot->Model && C->Clip && X.Time - C->LastRender >= AnimInterval && S.CanRender())
                P->RenderAnim(S, *C, *Slot->Model, X.Time, Pixels);
            Settled &= C->RigKnown && (C->LiveValid || C->NoRig || C->ClipFailed || (Slot && Slot->Failed));
        }
        else if (C->Kind == AssetKind::Map)
        {
            if (C->Mesh && !C->Still && S.CanRender()) P->RenderMap(S, *C);
            Settled &= C->Still != nullptr || C->MeshFailed;
        }
        Live += C->LiveValid;
    }
    P->WasSettled = Settled;
    P->LastVisible = Order.size();
    P->LastLive = Live;
    P->LastRenders = S.RendersThisFrame();
}

bool ModelCellRenderer::Settled() const { return P->WasSettled; }

std::string ModelCellRenderer::Stats() const
{
    return "3d cells visible " + std::to_string(P->LastVisible) + ", live " + std::to_string(P->LastLive) +
           ", renders this frame " + std::to_string(P->LastRenders) + ", meshes on the GPU " + std::to_string(P->LastMeshes) +
           ", cells known " + std::to_string(P->Cells.size()) +
           (P->Poses ? ", anim poses " + std::to_string(P->Poses) + " (" + std::to_string(P->PoseMs / P->Poses) + " ms each on the CPU, skinned on the GPU, " +
                       std::to_string(P->Rigs.size()) + " rigs)" : std::string());
}
