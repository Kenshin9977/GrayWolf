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
#include "ImGuiTheme.h"        // the title font, icons
#include "GameBadges.h"        // the game's icon in the header
#include "PreviewEnvironmentUi.h"   // the environment's settings

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

        // the model filling the view (a map keeps its own framing, FramePreviewForTurn)
        void FitCamera(ViewportState& V)
        {
            auto& F = V.Fitted;
            F.Kept = FitPreviewCamera(V.Mesh, V.Yaw, V.Pitch, V.ViewAspect, V.DistScale, V.Pan);
            F.Aspect = V.ViewAspect; F.Yaw = V.Yaw; F.Pitch = V.Pitch; F.DistScale = V.DistScale; F.Pan = V.Pan;
        }

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
            DL->PathStroke(IM_COL32(245, 165, 36, 230), 0, 3.0f);
            const char* Text = "Loading\xE2\x80\xA6";
            const ImVec2 Ts = ImGui::CalcTextSize(Text);
            DL->AddText(ImVec2(C.x - Ts.x * 0.5f, C.y + R + 8.0f), IM_COL32(220, 220, 225, 255), Text);
        }

        // The selection's name as a title (the details toggle at its right), then its numbers, small.
        void DrawSelectionHeader(ShellState& S)
        {
            ViewportState& V = S.Viewport;
            const Item& Sel = S.Catalog.Models[S.Selected];
            Theme::PushTitle();
            ImGui::TextUnformatted(Sel.Name.c_str());
            Theme::PopFont();
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight()));
            if (Theme::IconButton("details", Theme::Icon::Info, "Show details: path, format, counts and sizes", V.ShowDetails))
            {
                V.ShowDetails = !V.ShowDetails;
                SettingsManager::SetSetting("viewport_details", V.ShowDetails ? "true" : "false");
            }
            Theme::PushSmall();
            // the game as its icon (its title on hover), then the numbers
            if (!Sel.Game.empty())
            {
                const float Side = ImGui::GetTextLineHeight();
                GameBadges::Draw(ImGui::GetWindowDrawList(), S.Dev.Get(), Sel.Game, ImGui::GetCursorScreenPos(), Side);
                ImGui::Dummy(ImVec2(Side, Side));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", GameBadges::Title(Sel.Game).c_str());
                ImGui::SameLine();
            }
            if ((Sel.Kind == 0 || Sel.Kind == 4) && V.Loader.Busy())
                ImGui::TextDisabled("Loading\xE2\x80\xA6  \xC2\xB7  %s", FormatSize(Sel.SizeBytes).c_str());
            else if ((Sel.Kind == 0 || Sel.Kind == 4) && V.Mesh.Valid)
                ImGui::TextDisabled("%u verts  \xC2\xB7  %u tris  \xC2\xB7  %u textures  \xC2\xB7  %s",
                    V.Mesh.VertexCount, V.Mesh.TriCount, V.Mesh.TexCount, FormatSize(Sel.SizeBytes).c_str());
            else if (Sel.Kind == 1 && V.ImageSRV) ImGui::TextDisabled("%d x %d  \xC2\xB7  %s", V.ImageW, V.ImageH, FormatSize(Sel.SizeBytes).c_str());
            else if (Sel.Kind == 2)             ImGui::TextDisabled("sound  \xC2\xB7  %s", FormatSize(Sel.SizeBytes).c_str());
            else if (Sel.Kind == 3)             ImGui::TextDisabled("animation  \xC2\xB7  %s", FormatSize(Sel.SizeBytes).c_str());
            else if (Sel.Kind == 5)             ImGui::TextDisabled("%s", V.FxPanel.Summary().c_str());
            Theme::PopFont();
            if (Sel.Kind == 4) DrawOpenInBlender(Sel.Path);   // a map export: its .blend, or its import
            ImGui::Spacing();
        }

        // The sidebar: every field the selection has, the raw numbers behind the header's line.
        void DrawDetails(ShellState& S)
        {
            ViewportState& V = S.Viewport;
            const float Em = ImGui::GetFontSize();
            const Item& Sel = S.Catalog.Models[S.Selected];
            ImGui::SeparatorText("Details");
            {
                ImGui::TextDisabled("Path");     ImGui::SameLine(Em * 7.0f); ImGui::TextWrapped("%s", Sel.Path.c_str());
                ImGui::TextDisabled("Format");   ImGui::SameLine(Em * 7.0f); ImGui::Text("%s", Sel.Format.c_str());
                ImGui::TextDisabled("Size");     ImGui::SameLine(Em * 7.0f); ImGui::Text("%s (%lld bytes)", FormatSize(Sel.SizeBytes).c_str(), Sel.SizeBytes);
                if (Sel.Kind == 0) // model
                {
                    ImGui::TextDisabled("Bones");  ImGui::SameLine(Em * 7.0f); ImGui::Text("%u", Sel.Bones);
                    ImGui::TextDisabled("Meshes"); ImGui::SameLine(Em * 7.0f); ImGui::Text("%u", Sel.Meshes);
                    ImGui::TextDisabled("Materials");   ImGui::SameLine(Em * 7.0f); ImGui::Text("%u", Sel.Mats);
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
            // the clip's own numbers (a folder still being read has the item's at 0)
            const uint32_t Frames = V.AnimClipOk ? V.AnimClip.FrameCount : A.Frames;
            const float Rate = V.AnimClipOk ? V.AnimClip.FrameRate : A.FrameRate;
            const bool Looping = V.AnimClipOk ? V.AnimClip.Looping : A.Looping;
            Theme::PushSmall();
            ImGui::TextDisabled("%u frames  \xC2\xB7  %.1f fps  \xC2\xB7  %.2fs  \xC2\xB7  %u bones  \xC2\xB7  %s",
                Frames, Rate, Rate > 0.0f ? (float)Frames / Rate : 0.0f, V.AnimClipOk ? (uint32_t)V.AnimClip.Bones.size() : A.Bones,
                Looping ? "looping" : "one-shot");
            Theme::PopFont();
            if (!V.AnimClipOk) ImGui::TextDisabled("Could not read this animation");
            else
            {
                // the pick, followed while it changes (a game's reference models coming in)
                AnimRigs::Pick Pick;
                const bool Known = S.Rigs->PickFor(A.Path, Pick);
                if (Known && !Pick.Paths.empty() && Pick.Paths != V.AnimRigPaths) LoadAnimRig(S, Pick.Paths, Pick.Label, Pick.Score, Pick.Manual);
                PollAnimRig(S);
                if (!Known) ImGui::TextDisabled("Finding a model to play it on\xE2\x80\xA6");
                else if (V.AnimRigLoading) ImGui::TextDisabled("Loading %s\xE2\x80\xA6", V.AnimRigLabel.c_str());
                else if (Pick.Paths.empty()) ImGui::TextDisabled("No model in the Library has its bones: add the folder of its model");
                else ImGui::Text("Plays on %s  \xC2\xB7  %.0f%% of its bones%s%s", V.AnimRigLabel.c_str(), V.AnimRigScore * 100.0f,
                                 V.AnimRigManual ? "  \xC2\xB7  your choice" : "", Pick.Reference ? "  \xC2\xB7  the game's reference model" : "");
                if (Known)
                {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Change\xE2\x80\xA6") && !V.ShowDetails)
                    {
                        V.ShowDetails = true;
                        SettingsManager::SetSetting("viewport_details", "true");
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Choose the model it plays on, in the details sidebar");
                }
                // the bones it moves that no model has: the model it was made for is not here
                if (Known && !Pick.Paths.empty() && !V.AnimRigLoading && Pick.OwnModelAbsent)
                    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Its own model is not in the Library: %.0f%% of its bones are in no model",
                                       Pick.Unmodelled * 100.0f);

                const bool Loaded = !V.AnimRigPaths.empty() && !V.AnimRigLoading;
                if (Loaded && !V.AnimBindOk) ImGui::TextDisabled("That model could not be read, or has no mesh to show");
                else if (Loaded && V.AnimBind.Bones.empty()) ImGui::TextDisabled("That model has no skeleton: nothing to animate");
                else if (Loaded && V.AnimBind.VtxBoneIdx.empty()) ImGui::TextDisabled("That model has no skin weights: it shows but won't move");

                if (Loaded && V.AnimBindOk && Frames > 0)
                {
                    if (Theme::IconButton("play", V.AnimPlaying ? Theme::Icon::Pause : Theme::Icon::Play, V.AnimPlaying ? "Pause" : "Play"))
                        V.AnimPlaying = !V.AnimPlaying;
                    ImGui::SameLine();
                    const float MaxFrame = std::max(0.0f, (float)Frames - 1.0f);
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::SliderFloat("##animframe", &V.AnimFrame, 0.0f, MaxFrame, "Frame %.1f")) V.AnimPlaying = false;

                    if (V.AnimPlaying && Rate > 0.0f)
                    {
                        V.AnimFrame += ImGui::GetIO().DeltaTime * Rate;
                        if (V.AnimFrame > MaxFrame)
                        {
                            if (Looping) V.AnimFrame = std::fmod(V.AnimFrame, MaxFrame + 1.0f);
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
            ImGui::Spacing();
        }

        // anim, in the sidebar: the models it can play on, best fit first, filtered by name
        void DrawAnimRigPicker(ShellState& S)
        {
            ViewportState& V = S.Viewport;
            const Item& A = S.Catalog.Models[S.Selected];
            if (!V.AnimClipOk) return;
            ImGui::SeparatorText("Plays on");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##animmodelfilter", "Filter by name\xE2\x80\xA6", V.AnimModelFilter, sizeof(V.AnimModelFilter));
            std::vector<AnimRigs::Candidate> Ranked;
            S.Rigs->Ranked(A.Path, Ranked);
            if (ImGui::BeginListBox("##animmodels", ImVec2(-1, ImGui::GetTextLineHeightWithSpacing() * 9.0f)))
            {
                if (ImGui::Selectable("Automatic (best fit)", !V.AnimRigManual) && V.AnimRigManual)
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
        }

        // 3D model, map, or an animated model - same GPU mesh, same orbit viewport
        void Draw3DView(ShellState& S, int SelKind)
        {
            const auto& Models = S.Catalog.Models;
            ViewportState& V = S.Viewport;
            ImGuiIO& io = ImGui::GetIO();
            ImVec2 Avail = ImGui::GetContentRegionAvail();
            const int VW = (int)std::max(16.0f, Avail.x), VH = (int)std::max(16.0f, Avail.y);
            V.ViewAspect = (float)VW / (float)VH;
            {
                auto& F = V.Fitted;
                const bool Moved = V.Yaw != F.Yaw || V.Pitch != F.Pitch || V.DistScale != F.DistScale || V.Pan.x != F.Pan.x || V.Pan.y != F.Pan.y || V.Pan.z != F.Pan.z;
                if (F.Kept && Moved) F.Kept = false;
                else if (F.Kept && std::abs(V.ViewAspect - F.Aspect) > F.Aspect * 0.01f) FitCamera(V);
            }
            RenderMeshToRT(V, S.Dev.Get(), S.Ctx.Get(), VW, VH);
            if (V.Vrt.OutputSRV)
            {
                ImGui::Image((ImTextureID)(intptr_t)V.Vrt.OutputSRV.Get(), ImVec2((float)VW, (float)VH));
                const ImVec2 ViewMin = ImGui::GetItemRectMin(), ViewMax = ImGui::GetItemRectMax();
                const bool ViewHovered = ImGui::IsItemHovered();
                if (V.Loader.Busy()) DrawLoadingOverlay(ViewMin, ViewMax);
                // the toolbar over the view's top left corner, on a dark plate (its own child window: the
                // view under it takes no drag while the pointer is on it)
                {
                    const ImVec2 Back = ImGui::GetCursorScreenPos();
                    const float Pad = ImGui::GetStyle().ItemSpacing.x;
                    ImGui::SetCursorScreenPos(ImVec2(ViewMin.x + Pad, ViewMin.y + Pad));
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(Theme::Base.x, Theme::Base.y, Theme::Base.z, 0.78f));
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
                    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 4));
                    if (ImGui::BeginChild("##viewtools", ImVec2(0, 0), ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                                          ImGuiWindowFlags_NoScrollbar))
                        DrawScreenshotBar(V, SelKind == 3 && !V.AnimRigPaths.empty() ? V.AnimRigLabel.substr(0, V.AnimRigLabel.find(' ')) + "_" + Models[S.Selected].Name : Models[S.Selected].Name);
                    ImGui::EndChild();
                    ImGui::PopStyleVar(2);
                    ImGui::PopStyleColor();
                    ImGui::SetCursorScreenPos(Back);
                    ImGui::Dummy(ImVec2(0, 0));   // the cursor moved back: an item there, as ImGui asks
                }
                if (ViewHovered)
                {
                    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                    {
                        ImVec2 D = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
                        V.Yaw -= D.x * 0.01f; V.Pitch += D.y * 0.01f;   // the model follows the mouse
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
                        V.DistScale = std::max(0.02f, std::min(8.0f, V.DistScale));
                    }
                }
            }
        }
    }

    ViewportState::ViewportState(ID3D11Device* Dev, ID3D11DeviceContext* Ctx)
        : Renderer(Dev), FxPanel(Dev, Ctx)
    {
        ShotScale = std::clamp(std::atoi(SettingsManager::GetSetting("preview_shot_scale", "1").c_str()), 1, 4);
        Lighting.Exposure = SettingsManager::GetSettingFloat("preview_exposure", 0.0f, -4.0f, 4.0f);
        Lighting.Lights = SettingsManager::GetSettingFloat("preview_lights", Lighting.Lights, 0.0f, 3.0f);
        Lighting.Specular = SettingsManager::GetSetting("preview_specular", "true") != "false";
        LoadPreviewEnvironmentSettings(Lighting);
        Lighting.Shadow = LoadPreviewShadowSettings();
        ShowDetails = SettingsManager::GetSetting("viewport_details", "false") == "true";
    }

    void ResetCamera(ViewportState& V)
    {
        V.Yaw = 0.9f; V.Pitch = 0.32f; V.DistScale = 1.0f; V.Pan = {};
        FitCamera(V);
    }

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
            RequestMesh(V, "map|" + Path, [D, Path](const std::function<bool()>& Cancelled) {
                PreviewGeometry Geo;
                GlbPreviewOptions Options;
                Options.Cancelled = Cancelled;
                GlbPreviewStats St;
                const bool Ok = LoadPreviewGeometryGlb(Path, Geo, Options, &St);
                AppLog::Log().info("map preview {}: read {:.0f} ms, parse {:.0f} ms, geometry {:.0f} ms, textures {:.0f} ms ({} of {} read, {} missing), "
                                   "{} of {} triangles", Path, St.ReadMs, St.ParseMs, St.GeometryMs, St.TexturesMs, St.ImagesRead, St.Images,
                                   St.ImagesMissing, St.TrianglesKept, St.Triangles);
                if (!Ok) return GpuMesh{};
                GpuMesh Mesh = BuildGpuMesh(D.Get(), Geo);
                FramePreviewForTurn(Geo, 0.32f, Mesh, true);   // the play area (ResetCamera's pitch), not the vistas around it
                return Mesh;
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

    // Nothing selected: where an asset to show comes from, centred in the view
    static void DrawEmptyView()
    {
        const ImVec2 Avail = ImGui::GetContentRegionAvail();
        const float X0 = ImGui::GetCursorPosX();
        const auto Centred = [&](const char* Text) {
            ImGui::SetCursorPosX(X0 + std::max(0.0f, (Avail.x - ImGui::CalcTextSize(Text).x) * 0.5f));
            ImGui::TextUnformatted(Text);
        };
        const char* Steps[] = {
            "Extract: pick a game, search its assets, double-click one to preview it",
            "Library: open a folder of exports, then click an asset",
        };
        const float Line = ImGui::GetTextLineHeightWithSpacing();
        const float Glyph = ImGui::GetFontSize() * 3.0f;
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.0f, (Avail.y - Glyph - Line * 4.0f) * 0.42f));
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Muted);
        ImGui::PushFont(nullptr, Glyph);
        Centred(Theme::Icon::Model);
        ImGui::PopFont();
        ImGui::PopStyleColor();
        ImGui::Spacing();
        Theme::PushTitle();
        Centred("Nothing selected");
        Theme::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Muted);
        for (const char* Step : Steps) Centred(Step);
        ImGui::PopStyleColor();
    }

    void DrawViewportPanel(ShellState& S)
    {
        ViewportState& V = S.Viewport;
        ImGui::Begin("Viewport");

        const int SelKind = (S.Selected >= 0 && S.Selected < (int)S.Catalog.Models.size()) ? S.Catalog.Models[S.Selected].Kind : -1;
        if (SelKind >= 0) DrawSelectionHeader(S);
        // the sidebar's width, taken from the view's
        const float SideW = SelKind >= 0 && V.ShowDetails ? std::min(ImGui::GetFontSize() * 18.0f, ImGui::GetContentRegionAvail().x * 0.45f) : 0.0f;
        if (SideW > 0) ImGui::BeginChild("##view", ImVec2(ImGui::GetContentRegionAvail().x - SideW - ImGui::GetStyle().ItemSpacing.x, 0));
        if (SelKind == 3) DrawAnimControls(S);

        if (SelKind == 0 || SelKind == 4 || (SelKind == 3 && V.AnimBindOk)) Draw3DView(S, SelKind);
        else if (SelKind == 1) // image
        {
            if (V.ImageSRV)
            {
                if (V.LastZoomedImage != V.ImageSRV.Get()) { V.ImageZoom = 1.0f; V.LastZoomedImage = V.ImageSRV.Get(); }
                DrawZoomableImage(V.ImageSRV.Get(), V.ImageW, V.ImageH, V.ImageZoom);
            }
            else ImGui::TextDisabled("Could not decode this image");
        }
        else if (SelKind == 2) // audio
        {
            DrawAudioPlayer(V.Audio, S.Catalog.Models[S.Selected].Format);
        }
        else if (SelKind == 5) V.FxPanel.Draw(); // an effect: FxPreviewPanel
        else if (SelKind == 3) {} // anim with no model picked yet (or it failed to load) - the picker
                                   // UI above already covers this; nothing more to show here.
        else DrawEmptyView();
        if (SideW > 0)
        {
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Raised);
            if (ImGui::BeginChild("##details", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding))
            {
                if (SelKind == 3) DrawAnimRigPicker(S);
                DrawDetails(S);
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }
        ImGui::End();
    }
}
