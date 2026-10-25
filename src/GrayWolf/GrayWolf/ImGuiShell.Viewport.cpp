#include "stdafx.h"

// The Viewport: what the selection loads (LoadSelection, LoadMerged) and how it is shown, one view per
// asset kind (the 3D view for models, maps and an anim's model; the image, audio and effect views).
#include "ImGuiShell.Internal.h"
#include "AppLog.h"           // timed steps
#include "ExportNaming.h"      // CharacterBaseName
#include "CastReader.h"        // ReadAnimClip
#include "SkeletalPreview.h"   // EvaluateSkeletonPose / SkinPreviewGeometry
#include "SettingsManager.h"   // the screenshot's scale, the lighting
#include "ImGuiBlender.h"      // a map's Open in Blender

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace ShellUi
{
    // a rig read off the UI thread (LoadAnimRig): the thread's until Done, then the UI's
    struct AnimRigLoad
    {
        std::vector<std::string> Paths;
        PreviewGeometry Bind;
        GpuMesh Mesh;
        std::vector<int> SkinBones;
        float Center[3] = {};
        bool Ok = false;
        std::atomic<bool> Done{ false };
    };

    namespace
    {
        GpuMesh BuildGpuMesh(ID3D11Device* Dev, const PreviewGeometry& Geo) { return UploadPreviewMesh(Dev, Geo); }

        // Rebuilds only M's vertex buffer from freshly-skinned Pos/Nrm (ranges, SRVs and framing stay from
        // the bind-pose build): called every displayed frame during playback. Vertex count/topology must
        // match the original build; a mismatch is a no-op.
        void UpdateGpuMeshVertices(ID3D11Device* Dev, GpuMesh& M, const std::vector<float>& Pos, const std::vector<float>& Nrm, const std::vector<float>& UV)
        {
            const UINT VertexCount = (UINT)(Pos.size() / 3);
            if (VertexCount == 0 || VertexCount != M.VertexCount || Nrm.size() != Pos.size() || UV.size() != VertexCount * 2) return;
            const std::vector<float> Vtx = InterleavePreviewVertices(Pos, Nrm, UV);
            D3D11_BUFFER_DESC Bd{}; Bd.ByteWidth = (UINT)(Vtx.size() * sizeof(float)); Bd.Usage = D3D11_USAGE_IMMUTABLE; Bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            D3D11_SUBRESOURCE_DATA Sd{}; Sd.pSysMem = Vtx.data();
            ComPtr<ID3D11Buffer> NewVB;
            if (SUCCEEDED(Dev->CreateBuffer(&Bd, &Sd, &NewVB))) M.VB = NewVB;
        }

        // Models and maps are built by Loader off the UI thread: a click moves the selection at once,
        // the previous model stays up, dimmed, until the new one is ready (see the Viewport panel).
        void RequestMesh(ViewportState& V, const std::string& Key, PreviewLoader::Job Build)
        {
            GpuMesh Cached;
            if (V.Loader.Request(Key, std::move(Build), Cached)) { V.Mesh = std::move(Cached); ResetCamera(V); }
        }

        void RequestModels(ShellState& S, const std::vector<std::string>& Paths)
        {
            std::string Key = "models";
            for (const auto& P : Paths) Key += "|" + P;
            ComPtr<ID3D11Device> D = S.Dev;
            RequestMesh(S.Viewport, Key, [D, Paths](const std::function<bool()>& Cancelled) {
                PreviewGeometry Geo;
                return LoadPreviewGeometry(Paths, Geo, Cancelled) ? BuildGpuMesh(D.Get(), Geo) : GpuMesh{};
            });
        }

        // Dims the viewport and spins an arc over it while Loader builds the selected model.
        void DrawLoadingOverlay(ImVec2 Min, ImVec2 Max)
        {
            ImDrawList* DL = ImGui::GetWindowDrawList();
            DL->AddRectFilled(Min, Max, IM_COL32(12, 12, 14, 150));
            const ImVec2 C((Min.x + Max.x) * 0.5f, (Min.y + Max.y) * 0.5f);
            const float R = ImGui::GetFontSize() * 1.1f, T = (float)ImGui::GetTime() * 6.0f;
            DL->PathArcTo(C, R, T, T + 4.2f, 32);
            DL->PathStroke(IM_COL32(120, 170, 230, 230), 0, 3.0f);
            const char* Text = "loading\xE2\x80\xA6";
            const ImVec2 Ts = ImGui::CalcTextSize(Text);
            DL->AddText(ImVec2(C.x - Ts.x * 0.5f, C.y + R + 8.0f), IM_COL32(220, 220, 225, 255), Text);
        }

        // The selection's name, its one-line summary and, collapsed, every field it has.
        void DrawSelectionHeader(ShellState& S)
        {
            ViewportState& V = S.Viewport;
            const float Em = ImGui::GetFontSize();
            const Item& Sel = S.Catalog.Models[S.Selected];
            ImGui::Text("%s", Sel.Name.c_str());
            if ((Sel.Kind == 0 || Sel.Kind == 4) && V.Loader.Busy())
                ImGui::TextDisabled("loading\xE2\x80\xA6  \xC2\xB7  %s", FormatSize(Sel.SizeBytes).c_str());
            else if ((Sel.Kind == 0 || Sel.Kind == 4) && V.Mesh.Valid)
                ImGui::TextDisabled("verts %u  \xC2\xB7  tris %u  \xC2\xB7  textures %u  \xC2\xB7  %s    (drag: orbit   middle drag: pan   wheel: zoom)",
                    V.Mesh.VertexCount, V.Mesh.TriCount, V.Mesh.TexCount, FormatSize(Sel.SizeBytes).c_str());
            else if (Sel.Kind == 1 && V.ImageSRV) ImGui::TextDisabled("%d x %d  \xC2\xB7  %s", V.ImageW, V.ImageH, FormatSize(Sel.SizeBytes).c_str());
            else if (Sel.Kind == 2)             ImGui::TextDisabled("audio  \xC2\xB7  %s", FormatSize(Sel.SizeBytes).c_str());
            else if (Sel.Kind == 3)             ImGui::TextDisabled("anim  \xC2\xB7  %s", FormatSize(Sel.SizeBytes).c_str());
            else if (Sel.Kind == 5)             ImGui::TextDisabled("%s    (drag: orbit   middle drag: pan   wheel: zoom   space: pause)", V.FxPanel.Summary().c_str());
            if (Sel.Kind == 4) DrawOpenInBlender(Sel.Path);   // a map export: its .blend, or its import

            // Full field dump for the selection, collapsed by default - the raw numbers behind the
            // one-line summary above, for whichever fields this asset kind actually has.
            if (ImGui::CollapsingHeader("Details"))
            {
                ImGui::TextDisabled("Path");     ImGui::SameLine(Em * 7.0f); ImGui::TextWrapped("%s", Sel.Path.c_str());
                ImGui::TextDisabled("Format");   ImGui::SameLine(Em * 7.0f); ImGui::Text("%s", Sel.Format.c_str());
                ImGui::TextDisabled("Size");     ImGui::SameLine(Em * 7.0f); ImGui::Text("%s (%lld bytes)", FormatSize(Sel.SizeBytes).c_str(), Sel.SizeBytes);
                if (Sel.Kind == 0) // model
                {
                    ImGui::TextDisabled("Bones");  ImGui::SameLine(Em * 7.0f); ImGui::Text("%u", Sel.Bones);
                    ImGui::TextDisabled("Meshes"); ImGui::SameLine(Em * 7.0f); ImGui::Text("%u", Sel.Meshes);
                    ImGui::TextDisabled("Mats");   ImGui::SameLine(Em * 7.0f); ImGui::Text("%u", Sel.Mats);
                    if (V.Mesh.Valid)
                    {
                        ImGui::TextDisabled("GPU verts");    ImGui::SameLine(Em * 7.0f); ImGui::Text("%u", V.Mesh.VertexCount);
                        ImGui::TextDisabled("GPU tris");     ImGui::SameLine(Em * 7.0f); ImGui::Text("%u", V.Mesh.TriCount);
                        ImGui::TextDisabled("GPU textures"); ImGui::SameLine(Em * 7.0f); ImGui::Text("%u", V.Mesh.TexCount);
                    }
                }
                else if (Sel.Kind == 1) // image
                {
                    ImGui::TextDisabled("Dimensions"); ImGui::SameLine(Em * 7.0f); ImGui::Text("%d x %d", Sel.ImgW, Sel.ImgH);
                    ImGui::TextDisabled("Decoded");    ImGui::SameLine(Em * 7.0f); ImGui::TextUnformatted(V.ImageSRV ? "yes" : "no");
                }
                else if (Sel.Kind == 2) // audio
                {
                    if (V.Audio.IsLoaded())
                    {
                        ImGui::TextDisabled("Sample rate"); ImGui::SameLine(Em * 7.0f); ImGui::Text("%u Hz", V.Audio.SampleRate());
                        ImGui::TextDisabled("Channels");    ImGui::SameLine(Em * 7.0f); ImGui::Text("%d", V.Audio.Channels());
                        ImGui::TextDisabled("Duration");    ImGui::SameLine(Em * 7.0f); ImGui::Text("%.2fs", V.Audio.Duration());
                    }
                    else ImGui::TextDisabled("(could not decode)");
                }
                else if (Sel.Kind == 3) // anim
                {
                    ImGui::TextDisabled("Frames");     ImGui::SameLine(Em * 7.0f); ImGui::Text("%u", Sel.Frames);
                    ImGui::TextDisabled("Frame rate"); ImGui::SameLine(Em * 7.0f); ImGui::Text("%.2f fps", Sel.FrameRate);
                    ImGui::TextDisabled("Duration");   ImGui::SameLine(Em * 7.0f); ImGui::Text("%.2fs", Sel.FrameRate > 0.0f ? (float)Sel.Frames / Sel.FrameRate : 0.0f);
                    ImGui::TextDisabled("Bones");       ImGui::SameLine(Em * 7.0f); ImGui::Text("%u", Sel.Bones);
                    ImGui::TextDisabled("Looping");     ImGui::SameLine(Em * 7.0f); ImGui::TextUnformatted(Sel.Looping ? "yes" : "no");
                }
                else if (Sel.Kind == 4) // map
                {
                    ImGui::TextDisabled("Meshes");  ImGui::SameLine(Em * 7.0f); ImGui::Text("%u", Sel.Meshes);
                    ImGui::TextDisabled("Verts");   ImGui::SameLine(Em * 7.0f); ImGui::Text("%u", Sel.Verts);
                    ImGui::TextDisabled("Tris");    ImGui::SameLine(Em * 7.0f); ImGui::Text("%u", Sel.Tris);
                    if (V.Mesh.Valid)
                    {
                        ImGui::TextDisabled("GPU textures"); ImGui::SameLine(Em * 7.0f); ImGui::Text("%u", V.Mesh.TexCount);
                    }
                }
            }
            ImGui::Separator();
        }

        // The rig an anim plays on read as one skeleton (LoadRigGeometry) and uploaded on its own thread:
        // the rig shown plays on until it is in (PollAnimRig), then its playback starts over.
        void LoadAnimRig(ShellState& S, const std::vector<std::string>& Paths, const std::string& Label, float Score, bool Manual)
        {
            ViewportState& V = S.Viewport;
            V.AnimRigPaths = Paths; V.AnimRigLabel = Label; V.AnimRigScore = Score; V.AnimRigManual = Manual;
            V.AnimModelPick = -1;
            for (int k = 0; k < (int)S.Catalog.Models.size(); k++) if (S.Catalog.Models[k].Path == Paths.front()) { V.AnimModelPick = k; break; }
            auto L = std::make_shared<AnimRigLoad>();
            L->Paths = Paths;
            V.AnimRigLoading = L;
            ComPtr<ID3D11Device> D = S.Dev;
            std::thread([L, D, Label] {
                AppLog::Timer Bind("anim preview model " + Label, spdlog::level::info, 300);
                L->Ok = LoadRigGeometry(L->Paths, L->Bind);
                if (L->Ok) L->Mesh = BuildGpuMesh(D.Get(), L->Bind);
                std::vector<char> Used(L->Bind.Bones.size(), 0);
                for (size_t k = 0; k < L->Bind.VtxBoneIdx.size() && k < L->Bind.VtxBoneWeight.size(); k++)
                    if (L->Bind.VtxBoneIdx[k] >= 0 && (size_t)L->Bind.VtxBoneIdx[k] < Used.size() && L->Bind.VtxBoneWeight[k] > 0) Used[L->Bind.VtxBoneIdx[k]] = 1;
                double Sum[3] = {};
                for (size_t b = 0; b < Used.size(); b++)
                    if (Used[b])
                    {
                        L->SkinBones.push_back((int)b);
                        for (int c = 0; c < 3; c++) Sum[c] += L->Bind.Bones[b].Pos[c];
                    }
                for (int c = 0; c < 3; c++) L->Center[c] = L->SkinBones.empty() ? 0.0f : (float)(Sum[c] / L->SkinBones.size());
                L->Done = true;
            }).detach();
        }

        // The rig LoadAnimRig asked for, shown once read (a load the selection moved past is dropped)
        void PollAnimRig(ShellState& S)
        {
            ViewportState& V = S.Viewport;
            const std::shared_ptr<AnimRigLoad> L = V.AnimRigLoading;
            if (!L || !L->Done) return;
            V.AnimRigLoading.reset();
            if (L->Paths != V.AnimRigPaths) return;
            V.AnimBindOk = L->Ok;
            V.AnimBind = std::move(L->Bind);
            V.Loader.Forget();
            V.Mesh = V.AnimBindOk ? std::move(L->Mesh) : GpuMesh{};
            ResetCamera(V);
            V.AnimSkinBones = std::move(L->SkinBones);
            for (int c = 0; c < 3; c++) V.AnimBindCenter[c] = L->Center[c];
            V.AnimFramed = false;
            V.AnimFrame = 0.0f; V.AnimPlaying = V.AnimBindOk;
        }

        // anim: the model it plays on (picked for it, or from the list by score), then play/pause/scrub
        void DrawAnimControls(ShellState& S)
        {
            auto& Models = S.Catalog.Models;
            ViewportState& V = S.Viewport;
            const Item& A = Models[S.Selected];
            ImGui::Text("%u frames  \xC2\xB7  %.1f fps  \xC2\xB7  %.2fs  \xC2\xB7  %u bones  \xC2\xB7  %s",
                A.Frames, A.FrameRate, A.FrameRate > 0.0f ? (float)A.Frames / A.FrameRate : 0.0f, A.Bones,
                A.Looping ? "looping" : "one-shot");
            if (!V.AnimClipOk) ImGui::TextDisabled("could not parse this animation");
            else
            {
                // the pick, followed while it changes (a game's reference models coming in)
                AnimRigs::Pick Pick;
                const bool Known = S.Rigs->PickFor(A.Path, Pick);
                if (Known && !Pick.Paths.empty() && Pick.Paths != V.AnimRigPaths) LoadAnimRig(S, Pick.Paths, Pick.Label, Pick.Score, Pick.Manual);
                PollAnimRig(S);
                ImGui::Spacing();
                if (!Known) ImGui::TextDisabled("finding a model for it\xE2\x80\xA6");
                else if (V.AnimRigLoading) ImGui::TextDisabled("loading %s\xE2\x80\xA6", V.AnimRigLabel.c_str());
                else if (Pick.Paths.empty()) ImGui::TextDisabled("no listed model has its bones");
                else ImGui::Text("Plays on %s  \xC2\xB7  %.0f%% of its bones%s%s", V.AnimRigLabel.c_str(), V.AnimRigScore * 100.0f,
                                 V.AnimRigManual ? "  \xC2\xB7  your choice" : "", Pick.Reference ? "  \xC2\xB7  the game's reference model" : "");

                ImGui::TextUnformatted("Preview on model:");
                ImGui::SetNextItemWidth(-1);
                ImGui::InputTextWithHint("##animmodelfilter", "filter by name\xE2\x80\xA6", V.AnimModelFilter, sizeof(V.AnimModelFilter));
                std::vector<AnimRigs::Candidate> Ranked;
                S.Rigs->Ranked(A.Path, Ranked);
                if (ImGui::BeginListBox("##animmodels", ImVec2(0, 110)))
                {
                    if (ImGui::Selectable("automatic (the best fit)", !V.AnimRigManual) && V.AnimRigManual)
                    {
                        S.Rigs->SetManual(A.Path, std::string());
                        V.AnimRigPaths.clear();
                    }
                    for (int k = 0; k < (int)Ranked.size(); k++)
                    {
                        const AnimRigs::Candidate& C = Ranked[k];
                        if (V.AnimModelFilter[0] && !NameMatches(V.AnimModelFilter, C.Name)) continue;
                        const bool IsPicked = V.AnimRigManual && V.AnimRigPaths.size() == 1 && V.AnimRigPaths[0] == C.Path;
                        char Line[512];
                        snprintf(Line, sizeof(Line), "%3.0f%%  %s%s", C.Score * 100.0f, C.Name.c_str(), C.Reference ? "  (reference)" : "");
                        ImGui::PushID(k);
                        if (ImGui::Selectable(Line, IsPicked) && !IsPicked)
                        {
                            // the anim's grid cell plays on it too
                            S.Rigs->SetManual(A.Path, C.Path);
                            LoadAnimRig(S, { C.Path }, C.Name, C.Score, true);
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndListBox();
                }

                const bool Loaded = !V.AnimRigPaths.empty() && !V.AnimRigLoading;
                if (Loaded && !V.AnimBindOk) ImGui::TextDisabled("could not load that model");
                else if (Loaded && V.AnimBind.Bones.empty()) ImGui::TextDisabled("that model has no skeleton - nothing to animate");
                else if (Loaded && V.AnimBind.VtxBoneIdx.empty()) ImGui::TextDisabled("that model has no skin weights - it will show but won't move");

                if (Loaded && V.AnimBindOk && A.Frames > 0)
                {
                    ImGui::Spacing();
                    if (ImGui::Button(V.AnimPlaying ? "Pause" : "Play", ImVec2(90, 0))) V.AnimPlaying = !V.AnimPlaying;
                    ImGui::SameLine();
                    const float MaxFrame = std::max(0.0f, (float)A.Frames - 1.0f);
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::SliderFloat("##animframe", &V.AnimFrame, 0.0f, MaxFrame, "frame %.1f")) V.AnimPlaying = false;

                    if (V.AnimPlaying && A.FrameRate > 0.0f)
                    {
                        V.AnimFrame += ImGui::GetIO().DeltaTime * A.FrameRate;
                        if (V.AnimFrame > MaxFrame)
                        {
                            if (A.Looping) V.AnimFrame = std::fmod(V.AnimFrame, MaxFrame + 1.0f);
                            else { V.AnimFrame = MaxFrame; V.AnimPlaying = false; }
                        }
                    }

                    // Re-evaluated every frame (not just while Playing) so scrubbing the slider above
                    // updates the mesh too.
                    std::vector<SkeletonPoseBone> Pose;
                    EvaluateSkeletonPose(V.AnimBind, V.AnimClip, V.AnimFrame, Pose);
                    std::vector<float> SkPos, SkNrm;
                    SkinPreviewGeometry(V.AnimBind, Pose, SkPos, SkNrm);
                    // the skinned bones' centroid kept where the bind pose has it across the ground, at
                    // the first pose's height (a crawl lies low, a jump still rises), as the grid's cells do
                    if (!V.AnimSkinBones.empty() && Pose.size() == V.AnimBind.Bones.size())
                    {
                        double Sum[3] = {};
                        for (const int b : V.AnimSkinBones) for (int c = 0; c < 3; c++) Sum[c] += Pose[b].Pos[c];
                        float Now3[3];
                        for (int c = 0; c < 3; c++) Now3[c] = (float)(Sum[c] / V.AnimSkinBones.size());
                        if (!V.AnimFramed) { V.AnimFirstZ = Now3[2]; V.AnimFramed = true; }
                        const float Shift[3] = { V.AnimBindCenter[0] - Now3[0], V.AnimBindCenter[1] - Now3[1], V.AnimBindCenter[2] - V.AnimFirstZ };
                        for (size_t v = 0; v + 2 < SkPos.size(); v += 3) for (int c = 0; c < 3; c++) SkPos[v + c] += Shift[c];
                    }
                    UpdateGpuMeshVertices(S.Dev.Get(), V.Mesh, SkPos, SkNrm, V.AnimBind.UV);
                }
            }
            ImGui::Separator();
        }

        // 3D model, map, or an animated model - same GPU mesh, same orbit viewport
        void Draw3DView(ShellState& S, int SelKind)
        {
            const auto& Models = S.Catalog.Models;
            ViewportState& V = S.Viewport;
            ImGuiIO& io = ImGui::GetIO();
            DrawScreenshotBar(V, SelKind == 3 && !V.AnimRigPaths.empty() ? V.AnimRigLabel.substr(0, V.AnimRigLabel.find(' ')) + "_" + Models[S.Selected].Name : Models[S.Selected].Name);
            ImVec2 Avail = ImGui::GetContentRegionAvail();
            const int VW = (int)std::max(16.0f, Avail.x), VH = (int)std::max(16.0f, Avail.y);
            RenderMeshToRT(V, S.Dev.Get(), S.Ctx.Get(), VW, VH);
            if (V.Vrt.OutputSRV)
            {
                ImGui::Image((ImTextureID)(intptr_t)V.Vrt.OutputSRV.Get(), ImVec2((float)VW, (float)VH));
                if (V.Loader.Busy()) DrawLoadingOverlay(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                if (ImGui::IsItemHovered())
                {
                    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                    {
                        ImVec2 D = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
                        V.Yaw += D.x * 0.01f; V.Pitch += D.y * 0.01f;
                        V.Pitch = std::max(-1.5f, std::min(1.5f, V.Pitch));
                        ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
                    }
                    if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f))
                    {
                        ImVec2 D = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle, 0.0f);
                        PanPreviewCamera(OrbitPreviewCamera(V.Mesh, V.Yaw, V.Pitch, V.DistScale, V.Pan), D.x, D.y, (float)VH, V.Pan);
                        ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
                    }
                    if (io.MouseWheel != 0.0f)
                    {
                        V.DistScale *= (io.MouseWheel > 0) ? 0.88f : 1.136f;
                        V.DistScale = std::max(0.1f, std::min(8.0f, V.DistScale));
                    }
                }
            }
        }
    }

    ViewportState::ViewportState(ID3D11Device* Dev, ID3D11DeviceContext* Ctx)
        : Renderer(Dev), FxPanel(Dev, Ctx)
    {
        ShotScale = std::clamp(std::atoi(SettingsManager::GetSetting("preview_shot_scale", "1").c_str()), 1, 4);
        Lighting.Exposure = std::clamp((float)std::atof(SettingsManager::GetSetting("preview_exposure", "0").c_str()), -4.0f, 4.0f);
        Lighting.Lights = std::clamp((float)std::atof(SettingsManager::GetSetting("preview_lights", "1").c_str()), 0.0f, 3.0f);
        Lighting.Specular = SettingsManager::GetSetting("preview_specular", "true") != "false";
    }

    void ResetCamera(ViewportState& V) { V.Yaw = 0.9f; V.Pitch = 0.32f; V.DistScale = 1.0f; V.Pan = {}; }

    void SyncAnimRigs(ShellState& S, bool Walking)
    {
        if (Walking || S.RigModelsVersion == S.Catalog.ModelsVersion + 1) return;
        S.RigModelsVersion = S.Catalog.ModelsVersion + 1;
        std::vector<AnimRigs::ModelRef> Listed;
        for (const Item& M : S.Catalog.Models)
            if (M.Kind == 0) Listed.push_back({ M.Path, M.Name });
        S.Rigs->SetModels(std::move(Listed), S.RigModelsVersion);
    }

    void LoadSelection(ShellState& S, int i)
    {
        const auto& Models = S.Catalog.Models;
        ViewportState& V = S.Viewport;
        static const char* KindNames[] = { "model", "image", "audio", "anim", "map", "fx" };
        AppLog::Timer Preview(std::string("preview ") + KindNames[Models[i].Kind % 6] + " " + Models[i].Path, spdlog::level::info, 300);
        S.Selected = i;
        V.ImageSRV.Reset(); V.ImageW = V.ImageH = 0;
        V.Audio.Unload(); // stop / release any audio from the previous selection
        V.AnimClipOk = false; V.AnimModelPick = -1; V.AnimBindOk = false; V.AnimPlaying = false; V.AnimFrame = 0.0f;
        V.AnimRigPaths.clear(); V.AnimRigLabel.clear(); V.AnimRigScore = 0; V.AnimRigManual = false;
        const Item& Sel = Models[i];
        if (Sel.Kind != 0 && Sel.Kind != 4) { V.Loader.Forget(); V.Mesh = GpuMesh{}; }
        if (Sel.Kind == 5) V.FxPanel.Load(Sel.Path); else V.FxPanel.Unload();

        if (Sel.Kind == 3) // anim: no mesh of its own - the model it plays on is picked for it (DrawAnimControls)
        {
            AppLog::Timer Read("read anim");
            V.AnimClipOk = ReadAnimClip(Sel.Path, V.AnimClip);
        }
        else if (Sel.Kind == 0) // model: assemble parts sharing the base name
        {
            const std::string Base = ExportNaming::CharacterBaseName(Sel.Name);
            std::vector<std::string> Paths;
            for (int k = 0; k < (int)Models.size(); k++)
                if (Models[k].Kind == 0 && ExportNaming::CharacterBaseName(Models[k].Name) == Base)
                {
                    const bool Body = Models[k].Name.find("_body") != std::string::npos || Models[k].Name.find("_torso") != std::string::npos;
                    if (Body) Paths.insert(Paths.begin(), Models[k].Path); else Paths.push_back(Models[k].Path);
                }
            if (Paths.empty()) Paths.push_back(Sel.Path);
            Preview.Detail("(" + std::to_string(Paths.size()) + " parts, queued)");
            RequestModels(S, Paths);
        }
        else if (Sel.Kind == 1) // image
        {
            V.ImageSRV = LoadImageSRV(S.Dev.Get(), Sel.Path, Sel.Format, V.ImageW, V.ImageH);
        }
        else if (Sel.Kind == 2) // audio: decode straight to PCM for playback + waveform
        {
            AppLog::Timer Decode("audio decode");
            V.Audio.Load(Sel.Path, Sel.Format);
        }
        else if (Sel.Kind == 4) // map: one standalone .glb scene, no multi-part assembly
        {
            ComPtr<ID3D11Device> D = S.Dev;
            const std::string Path = Sel.Path;
            RequestMesh(V, "map|" + Path, [D, Path](const std::function<bool()>&) {
                PreviewGeometry Geo;
                return LoadPreviewGeometryGlb(Path, Geo) ? BuildGpuMesh(D.Get(), Geo) : GpuMesh{};
            });
        }
    }

    // Merges a user-picked set of model parts into one scene (e.g. a weapon body plus its Ctrl+clicked
    // attachments), seated by LoadPreviewGeometry. The first path is the reference frame every other
    // part aligns onto, so the part with the most bones goes first.
    void LoadMerged(ShellState& S, const std::set<int>& Picks)
    {
        const auto& Models = S.Catalog.Models;
        ViewportState& V = S.Viewport;
        std::vector<int> Idxs(Picks.begin(), Picks.end());
        std::vector<int> ModelIdxs;
        for (int i : Idxs) if (i >= 0 && i < (int)Models.size() && Models[i].Kind == 0) ModelIdxs.push_back(i);
        if (ModelIdxs.empty()) return;
        std::sort(ModelIdxs.begin(), ModelIdxs.end(), [&](int a, int b) { return Models[a].Bones > Models[b].Bones; });

        S.Selected = ModelIdxs.front();
        V.ImageSRV.Reset(); V.ImageW = V.ImageH = 0; V.Audio.Unload();
        std::vector<std::string> Paths;
        for (int i : ModelIdxs) Paths.push_back(Models[i].Path);
        RequestModels(S, Paths);
    }

    void DrawViewportPanel(ShellState& S)
    {
        ViewportState& V = S.Viewport;
        ImGui::Begin("Viewport");

        const int SelKind = (S.Selected >= 0 && S.Selected < (int)S.Catalog.Models.size()) ? S.Catalog.Models[S.Selected].Kind : -1;
        if (SelKind >= 0) DrawSelectionHeader(S);
        if (SelKind == 3) DrawAnimControls(S);

        if (SelKind == 0 || SelKind == 4 || (SelKind == 3 && V.AnimBindOk)) Draw3DView(S, SelKind);
        else if (SelKind == 1) // image
        {
            if (V.ImageSRV)
            {
                if (V.LastZoomedImage != V.ImageSRV.Get()) { V.ImageZoom = 1.0f; V.LastZoomedImage = V.ImageSRV.Get(); }
                DrawZoomableImage(V.ImageSRV.Get(), V.ImageW, V.ImageH, V.ImageZoom);
            }
            else ImGui::TextDisabled("could not decode this image");
        }
        else if (SelKind == 2) // audio
        {
            DrawAudioPlayer(V.Audio, S.Catalog.Models[S.Selected].Format);
        }
        else if (SelKind == 5) V.FxPanel.Draw(); // an effect: FxPreviewPanel
        else if (SelKind == 3) {} // anim with no model picked yet (or it failed to load) - the picker
                                   // UI above already covers this; nothing more to show here.
        else ImGui::TextUnformatted("Select an asset on the left.");
        ImGui::End();
    }
}
