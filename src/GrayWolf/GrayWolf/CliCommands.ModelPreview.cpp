// NOMINMAX before <windows.h>: std::max/std::min below would be mangled by its macros.
#define NOMINMAX

#include <cmath>
#include <cfloat>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <sstream>
#include <vector>
#include <string>
#include <format>

#include "CliOptions.h"
#include "CliCommands.h"

#include <CLI/CLI.hpp>
#include "CliSupport.h"

#include "WraithX.h"
#include "CoDAssets.h"

#include "ModelPreview.h"
#include "SeAnimReader.h"
#include "CastReader.h"
#include "XAnimBinReader.h"
#include "SkeletalPreview.h"
#include "WraithAnim.h"
#include "SEAnimExport.h"
#include "SEModelExport.h"
#include "CastExport.h"
#include "ModelViewport.h"
#include "AssetGrid/GridShot.h"   // ui-grid-shot
#include "ImGuiShell.h"
#include "ModelPreviewGPU.h"
#include "CerberusCrypto.h"
#include "OatInProcess.h"
#include "GltfSceneWriter.h"
#include "XMEExport.h"
#include "WraithModel.h"

// Writes a tiny GLB scene (a quad + a point light + an instanced second quad) to verify the
// glTF scene writer produces a well-formed file before it's fed real GfxWorld geometry.
int RunGltfSelfTest(const CliOptions& Options)
{
    GltfSceneWriter W;
    const int mat = W.AddMaterial("test_red", -1, 0.85f, 0.15f, 0.15f, 1.0f);
    std::vector<GltfSceneWriter::Vertex> Verts = {
        {{0, 0, 0}, {0, 0, 1}, {0, 0}, {255, 255, 255, 255}},
        {{1, 0, 0}, {0, 0, 1}, {1, 0}, {255, 255, 255, 255}},
        {{1, 1, 0}, {0, 0, 1}, {1, 1}, {255, 255, 255, 255}},
        {{0, 1, 0}, {0, 0, 1}, {0, 1}, {255, 255, 255, 255}},
    };
    std::vector<uint32_t> Idx = {0, 1, 2, 0, 2, 3};
    const int mesh = W.AddMesh("quad", Verts, Idx, mat);
    W.AddNode("quad_a", mesh, GltfSceneWriter::Identity());
    float axis[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float origin[3] = {2.0f, 0.0f, 0.0f};
    W.AddNode("quad_b_instance", mesh, GltfSceneWriter::MatrixFromAxis(axis, origin, 0.5f));
    float lpos[3] = {1.0f, 1.0f, 3.0f};
    W.AddLightNode("test_point", GltfSceneWriter::LightType::Point, 1, 1, 1, 500.0f, 20.0f,
                   GltfSceneWriter::MatrixFromAxis(axis, lpos, 1.0f));
    const bool Ok = W.WriteGlb(Options.GltfSelfTest);
    CliReport(Options, std::format("result={}\nout={}\nmeshes={}\nnodes={}\n",
        Ok ? "ok" : "error", Options.GltfSelfTest.c_str(), W.MeshCount(), W.NodeCount()));
    return Ok ? 0 : 5;
}


// Exports a BO1 (T5) map fastfile's GfxWorld to a drag-and-drop Blender .glb (world mesh in-process
// via OAT + our glTF scene writer) - the map export OAT lacks.
int RunExportMapGlb(const CliOptions& Options)
{
    WraithX::InitializeAPI(); // WIC, for the IWi->PNG world textures
    const std::string Out = Options.Out.empty()
        ? (std::filesystem::path(Options.ExportMapGlb).stem().string() + ".glb") : Options.Out;
    std::string Report;
    const bool Ok = OatExportMapGlb(Options.ExportMapGlb, Out, Report, !Options.MapNoTex);
    CliReport(Options, std::format("result={}\n{}", Ok ? "ok" : "error", Report.c_str()));
    return Ok ? 0 : 5;
}


// Verifies the ported Cerberus script crypto (CRC32 + Salsa20) against known-answer vectors.
int RunCerberusSelfTest(const CliOptions& Options)
{
    std::string Report;
    const bool Ok = Cerberus::CerberusCryptoSelfTest(Report);
    const std::string Full = std::format("result={}\n{}", Ok ? "ok" : "error", Report.c_str());
    if (!Options.Out.empty())
    {
        std::ofstream O(Options.Out, std::ios::binary | std::ios::trunc);
        O.write(Full.data(), (std::streamsize)Full.size());
    }
    CliReport(Options, Full);
    return Ok ? 0 : 5;
}


// Round-trips a synthetic animation through SEAnim::ExportSEAnim and ReadSeAnim, then
// sanity-checks skeleton pose evaluation on a synthetic 2-bone rig.
int RunSeAnimSelfTest(const CliOptions& Options)
{
    std::string Report; bool Ok = true;
    auto Check = [&](bool Cond, const char* What) { if (!Cond) { Ok = false; Report += std::format("FAIL: {}\n", What); } };

    // --- Part 1: write a synthetic anim, read it back, compare ---
    WraithAnim Anim;
    Anim.AnimType = WraithAnimationType::Relative;
    Anim.Looping = true;
    Anim.FrameRate = 30.0f;
    Anim.AddTranslationKey("root", 0, 0, 0, 0);
    Anim.AddTranslationKey("root", 10, 5, 0, 0);
    Anim.AddRotationKey("root", 0, 0, 0, 0, 1);
    Anim.AddRotationKey("root", 10, 0, 0.70710678f, 0, 0.70710678f); // 90 deg around Y
    Anim.AddRotationKey("child", 0, 0, 0, 0, 1);
    Anim.AddRotationKey("child", 10, 0, 0, 0.38268343f, 0.92387953f); // 45 deg around Z
    Anim.AddBoneModifier("child", WraithAnimationType::Absolute);

    const std::filesystem::path TmpPath = std::filesystem::temp_directory_path() / "greyhound_seanim_selftest.seanim";
    SEAnim::ExportSEAnim(Anim, TmpPath.string());

    SeAnimClip Clip;
    Check(ReadSeAnim(TmpPath.string(), Clip), "ReadSeAnim failed to parse the exported file");
    if (Ok)
    {
        Check(Clip.FrameCount == 11, "FrameCount != 11 (WraithAnim::FrameCount() = max keyed frame + 1)");
        Check(std::abs(Clip.FrameRate - 30.0f) < 1e-4f, "FrameRate != 30");
        Check(Clip.Looping, "Looping != true");
        Check(Clip.Type == SeAnimBoneType::Relative, "global Type != Relative");
        Check(Clip.Bones.size() == 2, "BoneCount != 2");

        const SeAnimBoneTrack* Root = nullptr; const SeAnimBoneTrack* Child = nullptr;
        for (auto& B : Clip.Bones) { if (B.Name == "root") Root = &B; if (B.Name == "child") Child = &B; }
        Check(Root != nullptr, "no 'root' bone tag read back");
        Check(Child != nullptr, "no 'child' bone tag read back");
        if (Root)
        {
            Check(Root->PosKeys.size() == 2, "root.PosKeys.size() != 2");
            Check(Root->RotKeys.size() == 2, "root.RotKeys.size() != 2");
            Check(!Root->HasTypeOverride, "root has an unexpected modifier override");
            if (Root->PosKeys.size() == 2)
            {
                Check(Root->PosKeys[1].Frame == 10, "root.PosKeys[1].Frame != 10");
                Check(std::abs(Root->PosKeys[1].Value.x - 5.0f) < 1e-4f, "root.PosKeys[1].Value.x != 5");
            }
            if (Root->RotKeys.size() == 2)
                Check(std::abs(Root->RotKeys[1].Value.y - 0.70710678f) < 1e-4f, "root.RotKeys[1].Value.y != sin(45deg)");
        }
        if (Child)
        {
            Check(Child->HasTypeOverride && Child->TypeOverride == SeAnimBoneType::Absolute, "child modifier override not read back as Absolute");
            Check(Child->RotKeys.size() == 2, "child.RotKeys.size() != 2");
        }
    }

    // --- Part 2: pose evaluation + skinning on a synthetic 2-bone rig (root -> child, child bind-
    // offset (0,0,10) along Z) - verifies the hierarchy/rotation math actually moves a child bone
    // when its parent rotates, not just that file values round-trip.
    if (Ok)
    {
        PreviewGeometry Bind;
        Bind.Bones.resize(2);
        Bind.Bones[0] = { "root", -1, {0,0,0}, {0,0,0,1} };
        Bind.Bones[1] = { "child", 0, {0,0,10}, {0,0,0,1} };
        Bind.Pos = { 0,0,10 }; Bind.Nrm = { 0,0,1 }; Bind.UV = { 0,0 };
        Bind.VtxBoneIdx = { 1,-1,-1,-1 }; Bind.VtxBoneWeight = { 1.0f,0,0,0 }; // the one vertex rides "child" fully

        std::vector<SkeletonPoseBone> Pose;
        EvaluateSkeletonPose(Bind, Clip, 10.0f, Pose);
        Check(Pose.size() == 2, "EvaluateSkeletonPose returned the wrong bone count");
        if (Pose.size() == 2)
        {
            // root: Relative translation -> bind(0,0,0) + keyed(5,0,0) = (5,0,0)
            Check(std::abs(Pose[0].Pos[0] - 5.0f) < 1e-3f, "root world pos.x != 5 after Relative translation");
            // child bind-offset (0,0,10) rotated by root's 90deg-around-Y world rotation should move
            // off the Z axis into the XZ plane at the same radius (~10), composed onto root's pos.
            const float dx = Pose[1].Pos[0] - Pose[0].Pos[0], dz = Pose[1].Pos[2] - Pose[0].Pos[2];
            const float Radius = std::sqrt(dx*dx + dz*dz);
            Check(std::abs(Radius - 10.0f) < 1e-2f, "child offset radius from root != 10 (hierarchy/rotation composition broken)");
            Check(std::abs(dz) < 9.0f, "child stayed on the Z axis - root's rotation isn't reaching it");

            std::vector<float> SkPos, SkNrm;
            SkinPreviewGeometry(Bind, Pose, SkPos, SkNrm);
            Check(SkPos.size() == 3, "SkinPreviewGeometry returned the wrong vertex count");
            if (SkPos.size() == 3)
            {
                Check(std::abs(SkPos[0] - Pose[1].Pos[0]) < 1e-3f, "skinned vertex.x != child bone world pos.x (100% weight case)");
                Check(std::abs(SkPos[2] - Pose[1].Pos[2]) < 1e-3f, "skinned vertex.z != child bone world pos.z (100% weight case)");
            }
        }
    }

    std::error_code Ec; std::filesystem::remove(TmpPath, Ec);
    const std::string Full = std::format("result={}\n{}", Ok ? "ok" : "error", Report.c_str());
    if (!Options.Out.empty())
    {
        std::ofstream O(Options.Out, std::ios::binary | std::ios::trunc);
        O.write(Full.data(), (std::streamsize)Full.size());
    }
    CliReport(Options, Full);
    return Ok ? 0 : 5;
}


// Writes a synthetic model and anim as .cast and as .semodel/.seanim, from one WraithModel and
// one WraithAnim, and reads both back through the readers GrayWolf uses on its own exports
// (LoadPreviewGeometry, CastFile::ModelCounts/FileKind, ReadCastAnim against ReadSeAnim): the two
// formats must give the same geometry, skeleton, weights and clip. The model has what the game
// exports rarely all hold: six influences on a vertex, two UV layers, vertex colours, a mesh
// without a material, blend shapes; the anim scale keys, a bone modifier, notetracks, blend shape
// weights. --out <dir> keeps the four files.
int RunCastSelfTest(const CliOptions& Options)
{
    std::string Report; bool Ok = true;
    auto Check = [&](bool Cond, const std::string& What) { if (!Cond) { Ok = false; Report += "FAIL: " + What + "\n"; } };

    WraithModel Model;
    Model.AssetName = "cast_selftest";
    for (int B = 0; B < 7; B++)
    {
        auto& Bone = Model.AddBone();
        Bone.TagName = B == 0 ? "tag_origin" : "j_bone" + std::to_string(B);
        Bone.BoneParent = B - 1;
        Bone.LocalPosition = Vector3(B == 0 ? 0.0f : 2.0f, 0, 0);
        Bone.GlobalPosition = Vector3(2.0f * B, 0, 0);
        Bone.LocalRotation = Bone.GlobalRotation = Quaternion(0, 0, 0, 1);
        Bone.BoneScale = Vector3(1, 1, 1);
    }
    auto& Mat = Model.AddMaterial();
    Mat.MaterialName = "selftest_mat";
    Mat.DiffuseMapName = "selftest_c.png";
    Mat.NormalMapName = "selftest_n.png";
    Mat.SpecularMapName = "";
    Model.BlendShapes = { "shape_a", "shape_b" };
    for (int M = 0; M < 2; M++)
    {
        auto& Sub = Model.AddSubmesh();
        // one material index per UV layer: SEModel ties its layer count to it (Cast has its own)
        Sub.AddMaterial(M == 0 ? 0 : -1);
        Sub.AddMaterial(M == 0 ? 0 : -1);
        for (int V = 0; V < 8; V++)
        {
            auto& Vert = Sub.AddVertex();
            Vert.Position = Vector3((float)V, (float)(V % 2), (float)M);
            Vert.Normal = Vector3(0, 0, 1);
            Vert.AddUVLayer(V / 8.0f, M * 0.5f);
            Vert.AddUVLayer(1.0f - V / 8.0f, 0.25f);
            Vert.Color[0] = (uint8_t)(V * 30); Vert.Color[1] = 255; Vert.Color[2] = (uint8_t)M; Vert.Color[3] = 128;
            const int Influences = (V == 0) ? 6 : 1 + V % 3;
            for (int W = 0; W < Influences; W++) Vert.AddVertexWeight((uint32_t)((V + W) % 7), 1.0f / Influences);
            if (M == 0 && V < 3) Vert.BlendShapeDeltas.push_back({ 0, Vector3(0, 0, 1.0f + V) });
            if (V >= 5) Vert.BlendShapeDeltas.push_back({ 1, Vector3(0.5f, 0, 0) });
        }
        for (uint32_t F = 0; F + 2 < 8; F++) Sub.AddFace(F, F + 1, F + 2);
    }

    WraithAnim Anim;
    Anim.AssetName = "cast_selftest_anim";
    Anim.AnimType = WraithAnimationType::Relative;
    Anim.FrameRate = 30.0f;
    Anim.Looping = true;
    for (uint32_t F = 0; F <= 300; F += 50)   // past 255: two-byte frames
    {
        Anim.AddTranslationKey("tag_origin", F, (float)F, 0, 0);
        Anim.AddRotationKey("j_bone1", F, 0, 0, std::sin(F * 0.001f), std::cos(F * 0.001f));
        Anim.AddScaleKey("j_bone2", F, 1.0f, 1.0f + F * 0.001f, 1.0f);
        Anim.AddBlendShapeKey("shape_a", F, F / 300.0f, 0, 0);
    }
    Anim.AddBoneModifier("j_bone3", WraithAnimationType::Absolute);
    Anim.AddTranslationKey("j_bone3", 10, 1, 2, 3);
    Anim.AddNoteTrack("end", 300);
    Anim.AddNoteTrack("fire", 12);

    std::error_code Ec;
    const bool Keep = !Options.Out.empty();
    const std::filesystem::path Dir = Keep ? std::filesystem::path(Options.Out)
                                           : std::filesystem::temp_directory_path() / "graywolf_cast_selftest";
    std::filesystem::create_directories(Dir, Ec);
    const std::string M = (Dir / "cast_selftest").string(), A = (Dir / "cast_selftest_anim").string();
    Cast::ExportCastModel(Model, M + ".cast");
    SEModel::ExportSEModel(Model, M + ".semodel");
    Cast::ExportCastAnim(Anim, A + ".cast");
    SEAnim::ExportSEAnim(Anim, A + ".seanim");

    // the model: the counts the browser and the index read, then the preview's geometry
    uint32_t Bones = 0, Meshes = 0, Mats = 0;
    Check(CastFile::FileKind(M + ".cast") == CastFile::Kind::Model, "the model .cast is not read as a model");
    Check(CastFile::FileKind(A + ".cast") == CastFile::Kind::Animation, "the anim .cast is not read as an anim");
    Check(CastFile::ModelCounts(M + ".cast", Bones, Meshes, Mats) && Bones == 7 && Meshes == 2 && Mats == 1,
          std::format("ModelCounts {} bones, {} meshes, {} materials (want 7, 2, 1)", Bones, Meshes, Mats));
    PreviewGeometry FromCast, FromSe;
    Check(LoadPreviewGeometry({ M + ".cast" }, FromCast), "LoadPreviewGeometry failed on the .cast");
    Check(LoadPreviewGeometry({ M + ".semodel" }, FromSe), "LoadPreviewGeometry failed on the .semodel");
    Check(FromCast.Pos == FromSe.Pos && FromCast.Nrm == FromSe.Nrm && FromCast.UV == FromSe.UV, "positions, normals or UVs differ");
    Check(FromCast.Idx == FromSe.Idx, std::format("triangles differ ({} vs {} indices, or their winding)", FromCast.Idx.size(), FromSe.Idx.size()));
    Check(FromCast.VtxMat == FromSe.VtxMat, "per-vertex materials differ");
    Check(FromCast.VtxBoneIdx == FromSe.VtxBoneIdx && FromCast.VtxBoneWeight == FromSe.VtxBoneWeight, "skin weights differ");
    Check(FromCast.Bones.size() == FromSe.Bones.size(), "bone counts differ");
    for (size_t B = 0; B < std::min(FromCast.Bones.size(), FromSe.Bones.size()); B++)
        Check(FromCast.Bones[B].Name == FromSe.Bones[B].Name && FromCast.Bones[B].Parent == FromSe.Bones[B].Parent &&
              std::equal(FromCast.Bones[B].Pos, FromCast.Bones[B].Pos + 3, FromSe.Bones[B].Pos), "bone " + FromSe.Bones[B].Name + " differs");

    // the anim, as the browser's player reads each format
    SeAnimClip CastClip, SeClip;
    Check(ReadAnimClip(A + ".cast", CastClip), "ReadAnimClip failed on the .cast");
    Check(ReadAnimClip(A + ".seanim", SeClip), "ReadAnimClip failed on the .seanim");
    Check(CastClip.FrameCount == SeClip.FrameCount && CastClip.FrameRate == SeClip.FrameRate && CastClip.Looping == SeClip.Looping &&
          CastClip.Type == SeClip.Type, std::format("clip header differs: {} frames vs {}", CastClip.FrameCount, SeClip.FrameCount));
    for (const auto& S : SeClip.Bones)
    {
        const auto It = std::find_if(CastClip.Bones.begin(), CastClip.Bones.end(), [&](const auto& C) { return C.Name == S.Name; });
        if (It == CastClip.Bones.end()) { Check(S.PosKeys.empty() && S.RotKeys.empty() && S.ScaleKeys.empty(), "bone " + S.Name + " missing"); continue; }
        const auto SameV = [](const auto& X, const auto& Y) {
            return X.size() == Y.size() && std::equal(X.begin(), X.end(), Y.begin(), [](const auto& P, const auto& Q) {
                return P.Frame == Q.Frame && P.Value.x == Q.Value.x && P.Value.y == Q.Value.y && P.Value.z == Q.Value.z; });
        };
        Check(SameV(It->PosKeys, S.PosKeys) && SameV(It->ScaleKeys, S.ScaleKeys) && It->RotKeys.size() == S.RotKeys.size(),
              "bone " + S.Name + " keys differ");
        Check(It->HasTypeOverride == S.HasTypeOverride && (!S.HasTypeOverride || It->TypeOverride == S.TypeOverride),
              "bone " + S.Name + " mode differs");
    }

    if (!Keep) std::filesystem::remove_all(Dir, Ec);
    const std::string Full = std::format("result={}\n{}", Ok ? "ok" : "error", Report);
    CliReport(Options, Full);
    return Ok ? 0 : 5;
}


// Writes a visibly-spinning test .seanim targeting one named bone (its full local rotation cycles
// through 360 degrees around Z over 60 frames @ 30fps), to check playback in the GUI.
int RunMakeTestSeAnim(const CliOptions& Options)
{
    WraithAnim Anim;
    Anim.AnimType = WraithAnimationType::Relative;
    Anim.Looping = true;
    Anim.FrameRate = 30.0f;
    const int Frames = 60;
    for (int f = 0; f <= Frames; f++)
    {
        const float Angle = (float)f / (float)Frames * 6.28318530718f; // full turn
        Anim.AddRotationKey(Options.MakeTestSeAnim, f, 0, 0, std::sin(Angle * 0.5f), std::cos(Angle * 0.5f));
    }
    const std::string Out = Options.Out.empty() ? "test.seanim" : Options.Out;
    SEAnim::ExportSEAnim(Anim, Out);
    CliReport(Options, std::format("result=ok\nout={}\nbone={}\n", Out.c_str(), Options.MakeTestSeAnim.c_str()));
    return 0;
}


// Diagnostic: skins a real model (.cast, .semodel) against a real anim (.cast, .seanim) at one frame and renders it, headless,
// with the same EvaluateSkeletonPose/SkinPreviewGeometry calls the GUI's anim playback uses.
int RunRenderAnimModel(const CliOptions& Options)
{
    WraithX::InitializeAPI(); // WIC, for the PNG encode
    std::vector<std::string> Parts; std::stringstream Ss(Options.RenderAnimModel); std::string P;
    while (std::getline(Ss, P, ';')) Parts.push_back(P);
    if (Parts.size() != 3)
    {
        CliReport(Options, "result=error\nreason=usage: --render-anim <model_path>;<anim_path>;<frame>\n");
        return 5;
    }
    PreviewGeometry Bind;
    const bool ModelOk = LoadPreviewGeometry({ Parts[0] }, Bind);
    SeAnimClip Clip;
    const bool AnimOk = ModelOk && ReadAnimClip(Parts[1], Clip);
    std::string Report = std::format("model={}\nanim={}\nframe={}\nmodel_load={}\nanim_load={}\nbones={} weighted_verts={}\n",
        Parts[0].c_str(), Parts[1].c_str(), Parts[2].c_str(), ModelOk ? "ok" : "FAIL", AnimOk ? "ok" : "FAIL",
        Bind.Bones.size(), Bind.VtxBoneIdx.empty() ? "no" : "yes");
    bool Ok = false;
    if (AnimOk)
    {
        const float Frame = std::strtof(Parts[2].c_str(), nullptr);
        std::vector<SkeletonPoseBone> Pose;
        EvaluateSkeletonPose(Bind, Clip, Frame, Pose);
        std::vector<float> SkPos, SkNrm;
        SkinPreviewGeometry(Bind, Pose, SkPos, SkNrm);
        PreviewGeometry Animated = Bind; Animated.Pos = SkPos; Animated.Nrm = SkNrm;
        const std::string OutPath = Options.Out.empty() ? "anim_frame.png" : Options.Out;
        Ok = RenderPreviewGeometryToPNG(Animated, OutPath, 512);
        Report += std::format("render={} out={}\n", Ok ? "ok" : "FAIL", OutPath.c_str());
    }
    CliReport(Options, std::format("result={}\n{}", Ok ? "ok" : "error", Report.c_str()));
    return Ok ? 0 : 5;
}


// Parses a Mod Tools .xanim_bin (XAnimBinReader.h) and dumps the resolved
// [keyword, type, value] record stream - Layer 1 only (container + tagged-record walk), no
// semantic PART/BONE/FRAME assembly (that's ReadXAnimBinClip, below).
int RunXAnimBinDump(const CliOptions& Options)
{
    std::vector<XAnimBinRecord> Records;
    const bool Ok = ReadXAnimBinRecords(Options.XAnimBinDump, Records);
    std::string Report = std::format("result={} records={}\n", Ok ? "ok" : "error", Records.size());
    for (size_t i = 0; i < Records.size(); i++)
    {
        const auto& R = Records[i];
        Report += std::format("[{:4}] {:<16} idx={:2} type=0x{:02x}", i, R.Keyword.c_str(), R.TableIndex, (uint32_t)R.Type);
        if (!R.Strings.empty())
        {
            Report += " strings=[";
            for (size_t s = 0; s < R.Strings.size(); s++) { if (s) Report += ", "; Report += "\"" + R.Strings[s] + "\""; }
            Report += "]";
        }
        if (!R.Raw.empty())
        {
            Report += " raw=";
            for (uint8_t B : R.Raw) Report += std::format("{:02x}", B);
        }
        if (!R.UVs.empty()) Report += std::format(" uvs={}", R.UVs.size());
        Report += "\n";
    }
    CliReport(Options, Report);
    return Ok ? 0 : 5;
}


// Layer 2: assembles the record stream into a SeAnimClip and reports bone/frame/notify counts
// - the invariant checks are inside ReadXAnimBinClip itself (a declared NUMPARTS/NUMFRAMES/
// NUMKEYS that doesn't match what's actually assembled fails the read), so "result=ok" here is
// itself the self-test; this just surfaces what got assembled for a human to sanity-check.
int RunXAnimBinSelfTest(const CliOptions& Options)
{
    SeAnimClip Clip;
    const bool Ok = ReadXAnimBinClip(Options.XAnimBinSelfTest, Clip);
    std::string Report = std::format("result={}\n", Ok ? "ok" : "error");
    if (Ok)
    {
        size_t TotalPos = 0, TotalRot = 0, TotalNotify = 0;
        for (auto& B : Clip.Bones) { TotalPos += B.PosKeys.size(); TotalRot += B.RotKeys.size(); }
        for (auto& N : Clip.Notifications) TotalNotify += N.second.size();
        Report += std::format(
            "bones={} frames={} framerate={:.2f} pos_keys={} rot_keys={} notify_names={} notify_events={}\n",
            Clip.Bones.size(), Clip.FrameCount, Clip.FrameRate, TotalPos, TotalRot, Clip.Notifications.size(), TotalNotify);
        for (size_t b = 0; b < Clip.Bones.size() && b < 10; b++)
            Report += std::format("  bone[{}]={} pos={} rot={}\n", b, Clip.Bones[b].Name.c_str(), Clip.Bones[b].PosKeys.size(), Clip.Bones[b].RotKeys.size());
        for (auto& N : Clip.Notifications)
            Report += std::format("  notify \"{}\" x{}\n", N.first.c_str(), N.second.size());
    }
    CliReport(Options, Report);
    return Ok ? 0 : 5;
}


// .xanim_bin -> .seanim: Layer 2 assembly, converted to a WraithAnim (BuildWraithAnimFromSeAnimClip),
// then written through the existing SEAnim writer.
int RunXAnimBinToSeAnimIn(const CliOptions& Options)
{
    SeAnimClip Clip;
    bool Ok = ReadXAnimBinClip(Options.XAnimBinToSeAnimIn, Clip);
    WraithAnim Anim;
    if (Ok) Ok = BuildWraithAnimFromSeAnimClip(Clip, Anim);
    if (Ok) SEAnim::ExportSEAnim(Anim, Options.XAnimBinToSeAnimOut);
    CliReport(Options, std::format("result={} out={}\n", Ok ? "ok" : "error", Options.XAnimBinToSeAnimOut.c_str()));
    return Ok ? 0 : 5;
}


// Bulk .xanim_bin -> .seanim over an entire Mod Tools `xanim_export/` tree, mirroring its directory
// structure, so the whole corpus is browsable via `--ui <dir>`. Model-side (.xmodel_bin -> mesh)
// bulk conversion is not included: no semantic mesh-assembly layer exists yet.
int RunXAnimBinExportDirIn(const CliOptions& Options)
{
    namespace fs = std::filesystem;
    std::error_code Ec;
    if (!fs::is_directory(Options.XAnimBinExportDirIn, Ec))
    {
        CliReport(Options, std::format("result=error\nreason=folder not found: {}\n", Options.XAnimBinExportDirIn.c_str()));
        return 2;
    }
    fs::create_directories(Options.XAnimBinExportDirOut, Ec);

    size_t Total = 0, Ok = 0;
    std::string Failures;
    for (auto& Entry : fs::recursive_directory_iterator(Options.XAnimBinExportDirIn, fs::directory_options::skip_permission_denied, Ec))
    {
        if (!Entry.is_regular_file(Ec)) continue;
        auto Ext = Entry.path().extension().string();
        std::transform(Ext.begin(), Ext.end(), Ext.begin(), [](unsigned char c) { return (char)::tolower(c); });
        if (Ext != ".xanim_bin") continue;

        Total++;
        const fs::path Rel = fs::relative(Entry.path(), Options.XAnimBinExportDirIn, Ec);
        fs::path OutPath = fs::path(Options.XAnimBinExportDirOut) / Rel;
        OutPath.replace_extension(".seanim");
        fs::create_directories(OutPath.parent_path(), Ec);

        SeAnimClip Clip;
        WraithAnim Anim;
        bool One = ReadXAnimBinClip(Entry.path().string(), Clip) && BuildWraithAnimFromSeAnimClip(Clip, Anim);
        if (One) { SEAnim::ExportSEAnim(Anim, OutPath.string()); Ok++; }
        else { Failures += Entry.path().string() + "\n"; }
    }
    std::string Report = std::format("result={} total={} ok={} failed={}\n", (Ok == Total) ? "ok" : "partial", Total, Ok, Total - Ok);
    if (!Failures.empty()) Report += "failures:\n" + Failures;
    CliReport(Options, Report);
    return (Ok == Total) ? 0 : 5;
}


// Generates a flat N-gon disc (triangle fan, one bone, radial UVs) as a .XMODEL_EXPORT, via the same
// WraithModel -> CodXME::ExportXME writer used for every real exported model - references an existing
// alpha material by name rather than authoring a new asset.
int RunGenDisc(const CliOptions& Options)
{
    WraithModel Disc;

    auto& Root = Disc.AddBone();
    Root.TagName = "tag_origin";
    Root.BoneParent = -1;

    auto& Mat = Disc.AddMaterial();
    Mat.MaterialName = Options.GenDiscMaterial;
    Mat.DiffuseMapName = "";
    Mat.NormalMapName = "";
    Mat.SpecularMapName = "";

    auto& Sub = Disc.AddSubmesh();
    Sub.AddMaterial(0);

    const int Segments = Options.GenDiscSegments > 2 ? Options.GenDiscSegments : 32;
    const float Radius = Options.GenDiscRadius;
    const float InnerRadius = Options.GenDiscInnerRadius;
    const bool IsRing = InnerRadius > 0.0f && InnerRadius < Radius;
    const float Pi2 = 6.283185307f;

    if (!IsRing)
    {
        // Vertex 0 is the centre; ring vertices follow. Z is up in CoD's coordinate space, so
        // the disc lies flat in the XY plane, normal straight up, like a ground decal would.
        auto& CenterV = Sub.AddVertex();
        CenterV.Position = Vector3(0, 0, 0);
        CenterV.Normal = Vector3(0, 0, 1);
        CenterV.AddUVLayer(0.5f, 0.5f);
        CenterV.AddVertexWeight(0, 1.0f);
        CenterV.Color[0] = CenterV.Color[1] = CenterV.Color[2] = CenterV.Color[3] = 255;

        for (int i = 0; i < Segments; i++)
        {
            const float Angle = (Pi2 * (float)i) / (float)Segments;
            const float Cx = std::cos(Angle);
            const float Sx = std::sin(Angle);

            auto& V = Sub.AddVertex();
            V.Position = Vector3(Radius * Cx, Radius * Sx, 0);
            V.Normal = Vector3(0, 0, 1);
            // Radial UV: mesh edge maps to texture edge, mesh centre to texture centre, so a
            // round decal texture (like a crater/scorch mark) reads correctly across the disc.
            V.AddUVLayer(0.5f + 0.5f * Cx, 0.5f + 0.5f * Sx);
            V.AddVertexWeight(0, 1.0f);
            V.Color[0] = V.Color[1] = V.Color[2] = V.Color[3] = 255;
        }

        // Both windings, deliberately: the engine's front-face convention was never verified against
        // how these vertices were generated, and a one-sided disc facing away from the camera would
        // be silently invisible - doubling the triangle count is cheap insurance against that.
        for (int i = 0; i < Segments; i++)
        {
            const uint32_t A = 0;
            const uint32_t B = (uint32_t)(1 + i);
            const uint32_t C = (uint32_t)(1 + ((i + 1) % Segments));
            Sub.AddFace(A, B, C);
            Sub.AddFace(A, C, B);
        }
    }
    else
    {
        // Annulus (a ring, not a filled disc): an outer vertex ring then an inner ring, no centre
        // vertex - each segment a small quad spanning inner to outer edge. A boundary/range
        // indicator rather than a ground marker (e.g. tracing a radius without covering the floor).
        for (int i = 0; i < Segments; i++)
        {
            const float Angle = (Pi2 * (float)i) / (float)Segments;
            const float Cx = std::cos(Angle);
            const float Sx = std::sin(Angle);

            // Radial UV, same convention as the filled disc: texture centre maps to world centre,
            // texture edge to world radius Radius; the inner ring is pulled toward texture centre by
            // InnerRadius/Radius so a ring/donut icon samples correctly across the annulus.
            auto& Outer = Sub.AddVertex();
            Outer.Position = Vector3(Radius * Cx, Radius * Sx, 0);
            Outer.Normal = Vector3(0, 0, 1);
            Outer.AddUVLayer(0.5f + 0.5f * Cx, 0.5f + 0.5f * Sx);
            Outer.AddVertexWeight(0, 1.0f);
            Outer.Color[0] = Outer.Color[1] = Outer.Color[2] = Outer.Color[3] = 255;
        }
        const float InnerUVScale = InnerRadius / Radius;
        for (int i = 0; i < Segments; i++)
        {
            const float Angle = (Pi2 * (float)i) / (float)Segments;
            const float Cx = std::cos(Angle);
            const float Sx = std::sin(Angle);

            auto& Inner = Sub.AddVertex();
            Inner.Position = Vector3(InnerRadius * Cx, InnerRadius * Sx, 0);
            Inner.Normal = Vector3(0, 0, 1);
            Inner.AddUVLayer(0.5f + 0.5f * InnerUVScale * Cx, 0.5f + 0.5f * InnerUVScale * Sx);
            Inner.AddVertexWeight(0, 1.0f);
            Inner.Color[0] = Inner.Color[1] = Inner.Color[2] = Inner.Color[3] = 255;
        }

        for (int i = 0; i < Segments; i++)
        {
            const uint32_t OuterA = (uint32_t)i;
            const uint32_t OuterB = (uint32_t)((i + 1) % Segments);
            const uint32_t InnerA = (uint32_t)(Segments + i);
            const uint32_t InnerB = (uint32_t)(Segments + ((i + 1) % Segments));

            // Both windings again, same reasoning as the filled-disc path above.
            Sub.AddFace(OuterA, OuterB, InnerA);
            Sub.AddFace(OuterA, InnerA, OuterB);
            Sub.AddFace(InnerA, OuterB, InnerB);
            Sub.AddFace(InnerA, InnerB, OuterB);
        }
    }

    CodXME::ExportXME(Disc, Options.GenDisc);

    CliReport(Options, std::format(
        "result=ok\nout={}\nradius={:.2f}\ninner_radius={:.2f}\nring={}\nsegments={}\nmaterial={}\nverts={}\nfaces={}\n",
        Options.GenDisc.c_str(), Radius, InnerRadius, IsRing ? "yes" : "no", Segments,
        Options.GenDiscMaterial.c_str(), Disc.VertexCount(), Disc.FaceCount()));
    return 0;
}


// Diagnostic: dump a .semodel's bbox + skeleton (';'-separated for several parts).
int RunDumpModel(const CliOptions& Options)
{
    std::stringstream Ss(Options.DumpModel); std::string P; std::string Report;
    while (std::getline(Ss, P, ';'))
    {
        if (P.empty()) continue;
        Report += "== " + P + " ==\n" + DumpModelInfo(P) + "\n";
    }
    if (!Options.Out.empty())
    {
        std::ofstream O(Options.Out, std::ios::binary | std::ios::trunc);
        O.write(Report.data(), (std::streamsize)Report.size());
    }
    CliReport(Options, Report);
    return 0;
}


// Diagnostic: dump a .glb's PreviewGeometry stats via the exact same reader the UI uses
// (LoadPreviewGeometryGlb), to tell a real read bug apart from a rendering/framing bug.
int RunDumpGlb(const CliOptions& Options)
{
    PreviewGeometry Geo;
    const bool Ok = LoadPreviewGeometryGlb(Options.DumpGlb, Geo);
    std::string Report = std::format("== {} ==\nresult={}\n", Options.DumpGlb.c_str(), Ok ? "ok" : "error");
    if (Ok)
    {
        const size_t VertCount = Geo.Pos.size() / 3;
        float Mn[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, Mx[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (size_t v = 0; v < VertCount; v++)
            for (int k = 0; k < 3; k++)
            {
                const float C = Geo.Pos[v * 3 + k];
                Mn[k] = std::min(Mn[k], C); Mx[k] = std::max(Mx[k], C);
            }
        size_t UntexturedVerts = 0;
        for (int32_t M : Geo.VtxMat) if (M < 0) UntexturedVerts++;
        size_t DecodedTex = 0, EmptyTex = 0;
        for (auto& T : Geo.Materials) { if (T.Width > 0 && T.Height > 0 && !T.Rgba.empty()) DecodedTex++; else EmptyTex++; }

        Report += std::format(
            "verts={} tris={} materials={} (decoded={} empty={})\n"
            "bbox min=({:.1f},{:.1f},{:.1f}) max=({:.1f},{:.1f},{:.1f}) size=({:.1f},{:.1f},{:.1f})\n"
            "untextured verts (VtxMat<0)={} / {}\n",
            VertCount, Geo.Idx.size() / 3, Geo.Materials.size(), DecodedTex, EmptyTex,
            Mn[0], Mn[1], Mn[2], Mx[0], Mx[1], Mx[2], Mx[0]-Mn[0], Mx[1]-Mn[1], Mx[2]-Mn[2],
            UntexturedVerts, Geo.VtxMat.size());

        // Same mean-center / 90th-percentile-radius the renderer's camera framing uses
        // (see ModelPreviewGPU.cpp / ModelPreview.cpp RasterizeToRGBA) - printed here so a bad
        // camera position can be told apart from a bad read without touching the GUI.
        double SumX = 0, SumY = 0, SumZ = 0;
        for (size_t v = 0; v < VertCount; v++) { SumX += Geo.Pos[v*3]; SumY += Geo.Pos[v*3+1]; SumZ += Geo.Pos[v*3+2]; }
        const float Cx = (float)(SumX / VertCount), Cy = (float)(SumY / VertCount), Cz = (float)(SumZ / VertCount);
        std::vector<float> Dists(VertCount);
        float TrueMaxR = 0.0f;
        for (size_t v = 0; v < VertCount; v++)
        {
            const float dx = Geo.Pos[v*3]-Cx, dy = Geo.Pos[v*3+1]-Cy, dz = Geo.Pos[v*3+2]-Cz;
            Dists[v] = std::sqrt(dx*dx+dy*dy+dz*dz);
            TrueMaxR = std::max(TrueMaxR, Dists[v]);
        }
        const size_t P90i = (size_t)(Dists.size() * 0.90);
        std::nth_element(Dists.begin(), Dists.begin() + P90i, Dists.end());
        Report += std::format("mean-center=({:.1f},{:.1f},{:.1f}) p90-radius={:.1f} true-max-radius={:.1f}\n",
            Cx, Cy, Cz, Dists[P90i], TrueMaxR);
    }
    if (!Options.Out.empty())
    {
        std::ofstream O(Options.Out, std::ios::binary | std::ios::trunc);
        O.write(Report.data(), (std::streamsize)Report.size());
    }
    CliReport(Options, Report);
    return Ok ? 0 : 5;
}


// Diagnostic: headless PNG render of a map .glb (software rasterizer, no D3D device/window) - a
// fast way to sanity-check a map load without driving the interactive GUI.
int RunRenderGlbPng(const CliOptions& Options)
{
    WraithX::InitializeAPI(); // WIC, for the PNG encode
    const std::string OutPath = Options.Out.empty()
        ? (std::filesystem::path(Options.RenderGlbPng).stem().string() + ".png") : Options.Out;
    const bool Ok = RenderGlbToPNG(Options.RenderGlbPng, OutPath, 512);
    CliReport(Options, std::format("result={}\nout={}\n", Ok ? "ok" : "error", OutPath.c_str()));
    return Ok ? 0 : 5;
}


// Opens the modern Dear ImGui + D3D11 shell: dockable asset browser (filter + list) beside a 3D
// viewport. Blocks until the window is closed.
int RunUiMode(const CliOptions& Options)
{
    WraithX::InitializeAPI(); // WIC, for texture decode
    const bool Ok = RunImGuiShell(Options.Ui);
    CliReport(Options, std::format("result={}\n", Ok ? "ok" : "error"));
    return Ok ? 0 : 5;
}


// The shell hidden on a folder, its Assets grid (--list: the list) timed over --frames frames once
// every visible cell is drawn, the last frame saved to --out, the numbers beside it (.txt).
int RunUiGridShot(const CliOptions& Options)
{
    WraithX::InitializeAPI(); // WIC, for texture decode and the PNG
    GridShotOptions Shot;
    Shot.OutPng = Options.Out.empty() ? "ui-grid-shot.png" : Options.Out;
    Shot.Frames = std::max(1, Options.UiGridFrames);
    Shot.Cell = Options.UiGridCell;
    Shot.List = Options.UiGridList;
    Shot.AnimRig = Options.UiGridRig;
    Shot.Scroll = Options.UiGridScroll;
    Shot.Still = Options.UiGridStill;
    if (sscanf_s(Options.UiGridSize.c_str(), "%dx%d", &Shot.Width, &Shot.Height) != 2 || Shot.Width < 320 || Shot.Height < 240)
    {
        CliReport(Options, "result=error\nerror=--size is WxH\n");
        return 2;
    }
    static const char* Types[] = { "all", "model", "image", "audio", "anim", "map", "fx" };
    for (int i = 0; i < 7; i++)
        if (Options.Type == Types[i] || Options.Type == std::string(Types[i]) + "s") Shot.Type = i;
    const bool Ok = RunImGuiShell(Options.UiGridShot, &Shot);
    std::string Numbers;
    {
        std::filesystem::path Txt(Shot.OutPng);
        Txt.replace_extension(".txt");
        std::ifstream In(Txt);
        Numbers.assign(std::istreambuf_iterator<char>(In), std::istreambuf_iterator<char>());
    }
    CliReport(Options, std::format("result={}\n{}", Ok ? "ok" : "error", Numbers));
    return Ok ? 0 : 5;
}


// Opens an interactive Direct3D 11 window (raw Win32, DPI-crisp, MFC-free) to orbit/zoom the
// given .semodel part(s). Blocks until the window is closed.
int RunPreviewWindow(const CliOptions& Options)
{
    WraithX::InitializeAPI(); // WIC, for texture decode
    std::vector<std::string> Parts;
    { std::stringstream Ss(Options.PreviewWindow); std::string P;
      while (std::getline(Ss, P, ';')) if (!P.empty()) Parts.push_back(P); }
    const bool Ok = PreviewModelInteractive(Parts);
    CliReport(Options, std::format("result={}\nparts={}\n", Ok ? "ok" : "error", Parts.size()));
    return Ok ? 0 : 5;
}


// Renders one or more .semodel parts (';'-separated) to a shaded, textured PNG thumbnail via
// the headless software rasterizer. Multiple parts assemble into one shared scene.
int RunRenderModelCli(const CliOptions& Options)
{
    WraithX::InitializeAPI(); // WIC, for the PNG encode/decode
    std::vector<std::string> Parts;
    { std::stringstream Ss(Options.RenderModel); std::string P;
      while (std::getline(Ss, P, ';')) if (!P.empty()) Parts.push_back(P); }
    const std::string OutPath = Options.Out.empty() ? (Parts[0] + ".png") : Options.Out;
    const bool Ok = (Options.Turntable > 0) ? RenderModelTurntable(Parts, OutPath, 256, Options.Turntable)
                  : Options.Gpu             ? RenderModelGPU(Parts, OutPath, 512)
                  : (Parts.size() == 1)     ? RenderModelToPNG(Parts[0], OutPath, 512)
                                            : RenderModelsToPNG(Parts, OutPath, 512);
    CliReport(Options, std::format("result={}\nout={}\nparts={}\nrenderer={}\n",
        Ok ? "ok" : "error", OutPath.c_str(), Parts.size(),
        Options.Turntable > 0 ? "turntable" : Options.Gpu ? "gpu" : "software"));
    return Ok ? 0 : 5;
}

// ---- the subcommands (CliCommands.h)

void AddModelPreviewCommands(CliCommandBuilders& B)
{
    B["gltf-selftest"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("gltf-selftest", "Write a tiny GLB scene to verify the writer");
        Sub->add_option("outfile", Options.GltfSelfTest, "Where to write the test .glb")->required();
    };
    B["export-map-glb"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("export-map-glb", "Export a T5 map's GfxWorld to a Blender .glb");
        Sub->add_option("file", Options.ExportMapGlb, "Fast file (.ff) holding the GfxWorld")->required()->check(CLI::ExistingFile);
        Sub->add_flag("--map-no-tex", Options.MapNoTex, "Skip embedding textures (much smaller, geometry preview)");
    };
    B["cerberus-selftest"] = [](CLI::App& App, CliOptions& Options) {
        App.add_subcommand("cerberus-selftest", "Verify ported CRC32/Salsa20 (Cerberus crypto)")
            ->callback([&Options] { Options.CerberusSelfTest = true; });
    };
    B["seanim-selftest"] = [](CLI::App& App, CliOptions& Options) {
        App.add_subcommand("seanim-selftest", "Round-trip a synthetic .seanim + verify pose evaluation")
            ->callback([&Options] { Options.SeAnimSelfTest = true; });
    };
    B["cast-selftest"] = [](CLI::App& App, CliOptions& Options) {
        App.add_subcommand("cast-selftest", "Write a synthetic model/anim as .cast and SE, read both back (--out keeps them)")
            ->callback([&Options] { Options.CastSelfTest = true; });
    };
    B["make-test-seanim"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("make-test-seanim", "Write a spinning test .seanim (--out sets the path)");
        Sub->add_option("bonename", Options.MakeTestSeAnim, "Bone to animate")->required();
    };
    B["render-anim"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("render-anim", "Skinned PNG at one frame (--out sets the path)");
        Sub->add_option("parts", Options.RenderAnimModel, "\"<model>;<anim>;<frame>\"")->required();
    };
    B["xanimbin-dump"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("xanimbin-dump", "Parse a Mod Tools .xanim_bin, print the resolved record stream");
        Sub->add_option("file", Options.XAnimBinDump, ".xanim_bin file")->required()->check(CLI::ExistingFile);
    };
    B["xanimbin-selftest"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("xanimbin-selftest", "Assemble Layer 2, report bone/frame/notify counts");
        Sub->add_option("file", Options.XAnimBinSelfTest, ".xanim_bin file")->required()->check(CLI::ExistingFile);
    };
    B["xanimbin-to-seanim"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("xanimbin-to-seanim", "Convert one .xanim_bin to .seanim");
        Sub->add_option("infile", Options.XAnimBinToSeAnimIn, ".xanim_bin input")->required()->check(CLI::ExistingFile);
        Sub->add_option("outfile", Options.XAnimBinToSeAnimOut, ".seanim output")->required();
    };
    B["xanimbin-export-dir"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("xanimbin-export-dir",
            "Bulk .xanim_bin -> .seanim, mirroring the source tree so the result is browsable with `ui`");
        Sub->add_option("indir", Options.XAnimBinExportDirIn, "Source directory of .xanim_bin files")
            ->required()->check(CLI::ExistingDirectory);
        Sub->add_option("outdir", Options.XAnimBinExportDirOut, "Destination directory")->required();
    };
    B["gen-disc"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("gen-disc", "Write a flat N-gon disc (or ring) XMODEL_EXPORT");
        Sub->add_option("outfile", Options.GenDisc, "Where to write the .XMODEL_EXPORT")->required();
        Sub->add_option("--gen-disc-radius", Options.GenDiscRadius, "Outer radius in units (default 50)");
        Sub->add_option("--gen-disc-inner-radius", Options.GenDiscInnerRadius, ">0 makes an annulus (ring) instead of a filled disc");
        Sub->add_option("--gen-disc-segments", Options.GenDiscSegments, "Segment count (default 32)");
        Sub->add_option("--gen-disc-material", Options.GenDiscMaterial, "Existing alpha material to reference, no new asset needed");
    };
    B["dump-model"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("dump-model", "Print a .semodel's bones/bbox");
        Sub->add_option("file", Options.DumpModel, ".semodel file")->required()->check(CLI::ExistingFile);
    };
    B["dump-glb"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("dump-glb", "Print a .glb's PreviewGeometry stats");
        Sub->add_option("file", Options.DumpGlb, ".glb file")->required()->check(CLI::ExistingFile);
    };
    B["render-glb-png"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("render-glb-png", "Headless PNG render of a .glb (--out sets the path)");
        Sub->add_option("file", Options.RenderGlbPng, "Input .glb")->required()->check(CLI::ExistingFile);
    };
    B["ui"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("ui", "Open the ImGui + D3D11 asset browser shell on a folder of exported assets");
        Sub->add_option("dir", Options.Ui, "Folder of exported assets (empty starts with nothing loaded)");
    };
    B["ui-grid-shot"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("ui-grid-shot", "Open the asset browser hidden on a folder, time its Assets grid and save the frame (--out <png>, numbers beside)");
        Sub->add_option("dir", Options.UiGridShot, "Folder of exported assets")->required()->check(CLI::ExistingDirectory);
        Sub->add_option("--frames", Options.UiGridFrames, "Frames timed once the cells are drawn (default 120)");
        Sub->add_option("--cell", Options.UiGridCell, "Cell size in pixels (default: the setting)");
        Sub->add_option("--size", Options.UiGridSize, "Window size WxH (default 1920x1080)");
        Sub->add_flag("--list", Options.UiGridList, "Time the list view instead");
        Sub->add_option("--rig", Options.UiGridRig, "Model the anim cells play on");
        Sub->add_option("--scroll", Options.UiGridScroll, "Pixels a second the grid scrolls while timed");
        Sub->add_flag("--still", Options.UiGridStill, "Model cells hold the viewport's angle instead of turning (to compare the two)");
    };
    B["anim-rigs"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("anim-rigs", "Each anim of a folder with the model the browser plays it on and its score (AnimRigs.h)");
        Sub->add_option("dir", Options.AnimRigs, "Folder of exported assets")->required()->check(CLI::ExistingDirectory);
        Sub->add_flag("--no-fetch", Options.AnimRigsNoFetch, "Export no reference model, use the ones already kept");
    };
    B["preview-window"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("preview-window", "Open an interactive D3D11 orbit viewer");
        Sub->add_option("parts", Options.PreviewWindow, "Semicolon separated model part paths")->required();
    };
    B["render-model"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("render-model", "Render a model to PNG (software, --gpu, or --turntable N views)");
        Sub->add_option("file", Options.RenderModel, "Model file to render")->required()->check(CLI::ExistingFile);
        Sub->add_flag("--gpu", Options.Gpu, "Use the offscreen D3D11 renderer");
        Sub->add_option("--turntable", Options.Turntable, "Render N views around the model into one contact sheet");
    };
}
