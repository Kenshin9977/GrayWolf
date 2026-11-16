#include "stdafx.h"

#include "XAnimBinExport.h"

#include "MemoryWriter.h"
#include "WraithAnim.h"
#include "XBin.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace
{
    struct Quat { double X = 0, Y = 0, Z = 0, W = 1; };
    struct Vec { double X = 0, Y = 0, Z = 0; };

    Quat Mul(const Quat& A, const Quat& B)
    {
        return { A.W * B.X + A.X * B.W + A.Y * B.Z - A.Z * B.Y,
                 A.W * B.Y - A.X * B.Z + A.Y * B.W + A.Z * B.X,
                 A.W * B.Z + A.X * B.Y - A.Y * B.X + A.Z * B.W,
                 A.W * B.W - A.X * B.X - A.Y * B.Y - A.Z * B.Z };
    }
    Quat Conj(const Quat& Q) { return { -Q.X, -Q.Y, -Q.Z, Q.W }; }
    Quat Normalized(const Quat& Q)
    {
        const double L = std::sqrt(Q.X * Q.X + Q.Y * Q.Y + Q.Z * Q.Z + Q.W * Q.W);
        if (!(L > 1e-12)) return {};
        return { Q.X / L, Q.Y / L, Q.Z / L, Q.W / L };
    }
    Vec Rotate(const Quat& Q, const Vec& V)
    {
        const Quat P = Mul(Mul(Q, { V.X, V.Y, V.Z, 0 }), Conj(Q));
        return { P.X, P.Y, P.Z };
    }
    Vec Add(const Vec& A, const Vec& B) { return { A.X + B.X, A.Y + B.Y, A.Z + B.Z }; }
    Vec Sub(const Vec& A, const Vec& B) { return { A.X - B.X, A.Y - B.Y, A.Z - B.Z }; }

    Quat Slerp(const Quat& A, Quat B, double T)
    {
        double D = A.X * B.X + A.Y * B.Y + A.Z * B.Z + A.W * B.W;
        if (D < 0) { B = { -B.X, -B.Y, -B.Z, -B.W }; D = -D; }
        double Wa = 1 - T, Wb = T;
        if (D < 0.9995)
        {
            const double Theta = std::acos(D), S = std::sin(Theta);
            Wa = std::sin((1 - T) * Theta) / S;
            Wb = std::sin(T * Theta) / S;
        }
        return Normalized({ A.X * Wa + B.X * Wb, A.Y * Wa + B.Y * Wb, A.Z * Wa + B.Z * Wb, A.W * Wa + B.W * Wb });
    }

    std::string Lower(std::string S)
    {
        std::transform(S.begin(), S.end(), S.begin(), [](unsigned char C) { return (char)std::tolower(C); });
        return S;
    }

    // One channel's keys, sorted by frame: a frame between two keys interpolated, before the first
    // or past the last held
    template <class T, class V, class Lerp>
    V Sample(const std::vector<WraithAnimFrame<T>>& Keys, uint32_t Frame, V (*Convert)(const T&), Lerp Blend)
    {
        auto It = std::lower_bound(Keys.begin(), Keys.end(), Frame,
                                   [](const WraithAnimFrame<T>& K, uint32_t F) { return K.Frame < F; });
        if (It == Keys.end()) return Convert(Keys.back().Value);
        if (It->Frame == Frame || It == Keys.begin()) return Convert(It->Value);
        const auto& Prev = *(It - 1);
        const double T = double(Frame - Prev.Frame) / double(It->Frame - Prev.Frame);
        return Blend(Convert(Prev.Value), Convert(It->Value), T);
    }
    Vec ToVec(const Vector3& V) { return { V.X, V.Y, V.Z }; }
    Quat ToQuat(const Quaternion& Q) { return Normalized({ Q.X, Q.Y, Q.Z, Q.W }); }

    template <class T>
    std::vector<WraithAnimFrame<T>> Sorted(std::vector<WraithAnimFrame<T>> Keys)
    {
        std::stable_sort(Keys.begin(), Keys.end(), [](const auto& A, const auto& B) { return A.Frame < B.Frame; });
        return Keys;
    }

    void WriteAxes(MemoryWriter& Writer, const char* Keyword, const Vec& Axis)
    {
        XBin::WriteField(Writer, Keyword, XAnimBinValueType::Int16Nx3);
        Writer.Write(XBin::Int16N((float)Axis.X));
        Writer.Write(XBin::Int16N((float)Axis.Y));
        Writer.Write(XBin::Int16N((float)Axis.Z));
    }
}

bool XAnimBin::ExportXAnimBin(const WraithAnim& Animation, const XAnimBinRig& Rig, const std::string& FileName,
                               XAnimBinStats* Stats)
{
    const size_t Count = Rig.Bones.size();
    if (Count == 0 || Count > 0xFFFF) return false;

    // the rig's bind pose, local to each parent
    std::vector<Quat> BindWorldRot(Count), BindRot(Count);
    std::vector<Vec> BindWorldPos(Count), BindPos(Count);
    for (size_t I = 0; I < Count; I++)
    {
        const auto& B = Rig.Bones[I];
        BindWorldRot[I] = Normalized({ B.Rotation[0], B.Rotation[1], B.Rotation[2], B.Rotation[3] });
        BindWorldPos[I] = { B.Position[0], B.Position[1], B.Position[2] };
    }
    for (size_t I = 0; I < Count; I++)
    {
        const int32_t P = Rig.Bones[I].Parent;
        if (P >= (int32_t)I) return false;   // a parent after its child: not a skeleton the writer can walk
        if (P < 0) { BindRot[I] = BindWorldRot[I]; BindPos[I] = BindWorldPos[I]; continue; }
        const Quat Inv = Conj(BindWorldRot[P]);
        BindRot[I] = Normalized(Mul(Inv, BindWorldRot[I]));
        BindPos[I] = Rotate(Inv, Sub(BindWorldPos[I], BindWorldPos[P]));
    }

    // the anim's channels by the rig's bone, names told apart without case
    std::unordered_map<std::string, size_t> RigIndex;
    for (size_t I = 0; I < Count; I++) RigIndex.emplace(Lower(Rig.Bones[I].Name), I);

    struct Channels
    {
        std::vector<WraithAnimFrame<Quaternion>> Rot;
        std::vector<WraithAnimFrame<Vector3>> Pos;
        bool Additive = false;       // its rotation keys a change from the bind
        bool OffsetsBind = true;     // its translation keys an offset from the bind (all but an absolute non-root bone)
    };
    std::vector<Channels> Bone(Count);
    // the parts: the anim's bones the rig has and their parents, in the rig's order, as the stock bins
    // (their parts' parents are parts too). A root the anim does not key is not one: the poses are in
    // its frame, the root at the origin (the stock view anims' tag_view, whatever the model's bind).
    std::vector<int32_t> PartOf(Count, -1);
    std::vector<bool> Keyed(Count, false);
    XAnimBinStats Local;
    for (const auto& Name : Animation.Bones())
        if (const auto It = RigIndex.find(Lower(Name)); It != RigIndex.end()) Keyed[It->second] = true;
    for (const auto& Name : Animation.Bones())
    {
        const auto It = RigIndex.find(Lower(Name));
        if (It == RigIndex.end()) { Local.Unmatched.push_back(Name); continue; }
        for (int32_t B = (int32_t)It->second; B >= 0 && PartOf[B] < 0; B = Rig.Bones[B].Parent)
            if (Rig.Bones[B].Parent >= 0 || Keyed[B]) PartOf[B] = 0;
        auto& C = Bone[It->second];
        if (auto R = Animation.AnimationRotationKeys.find(Name); R != Animation.AnimationRotationKeys.end()) C.Rot = Sorted(R->second);
        if (auto T = Animation.AnimationPositionKeys.find(Name); T != Animation.AnimationPositionKeys.end()) C.Pos = Sorted(T->second);
        auto Type = Animation.AnimType;
        if (auto M = Animation.AnimationBoneModifiers.find(Name); M != Animation.AnimationBoneModifiers.end()) Type = M->second;
        C.Additive = Type == WraithAnimationType::Additive;
        C.OffsetsBind = Type != WraithAnimationType::Absolute || Rig.Bones[It->second].Parent < 0;
    }
    std::sort(Local.Unmatched.begin(), Local.Unmatched.end());
    std::vector<size_t> Parts;
    for (size_t I = 0; I < Count; I++)
        if (PartOf[I] == 0) { PartOf[I] = (int32_t)Parts.size(); Parts.push_back(I); }
    if (Parts.empty()) return false;   // none of the anim's bones is the rig's
    for (size_t I = 0; I < Count; I++)
        if (Rig.Bones[I].Parent < 0 && !Keyed[I]) { BindRot[I] = Quat{}; BindPos[I] = Vec{}; }

    const uint32_t Frames = std::max<uint32_t>(1, Animation.FrameCount());

    MemoryWriter Writer(0x100000);
    XBin::WriteField(Writer, "//", XAnimBinValueType::String);
    XBin::WriteStringAligned(Writer, "Exported by GrayWolf: " + Animation.AssetName);
    if (!Rig.Source.empty())
    {
        XBin::WriteField(Writer, "//", XAnimBinValueType::String);
        XBin::WriteStringAligned(Writer, "Skeleton: " + Rig.Source);
    }
    XBin::WriteField(Writer, "ANIMATION", XAnimBinValueType::Marker);
    XBin::WriteField(Writer, "VERSION", XAnimBinValueType::UInt16);
    Writer.Write<uint16_t>(4);
    XBin::WriteField(Writer, "NUMPARTS", XAnimBinValueType::UInt16);
    Writer.Write<uint16_t>((uint16_t)Parts.size());
    for (size_t P = 0; P < Parts.size(); P++)
    {
        XBin::WriteField(Writer, "PART", XAnimBinValueType::UInt16String);
        Writer.Write<uint16_t>((uint16_t)P);
        XBin::WriteStringAligned(Writer, Rig.Bones[Parts[P]].Name);
    }
    XBin::WriteField(Writer, "FRAMERATE", XAnimBinValueType::UInt16);
    Writer.Write<uint16_t>((uint16_t)std::clamp(std::lround(Animation.FrameRate), 1L, 0xFFFFL));
    XBin::WriteField(Writer, "NUMFRAMES", XAnimBinValueType::Raw32_2);
    Writer.Write<uint32_t>(Frames);

    const auto LerpVec = [](const Vec& A, const Vec& B, double T) {
        return Vec{ A.X + (B.X - A.X) * T, A.Y + (B.Y - A.Y) * T, A.Z + (B.Z - A.Z) * T };
    };
    std::vector<Quat> WorldRot(Count);
    std::vector<Vec> WorldPos(Count);
    for (uint32_t F = 0; F < Frames; F++)
    {
        XBin::WriteField(Writer, "FRAME", XAnimBinValueType::Raw32);
        Writer.Write<uint32_t>(F);
        for (size_t I = 0; I < Count; I++)
        {
            const auto& C = Bone[I];
            Quat Rot = BindRot[I];
            Vec Pos = BindPos[I];
            if (!C.Rot.empty())
            {
                const Quat Key = Sample(C.Rot, F, &ToQuat, Slerp);
                Rot = C.Additive ? Normalized(Mul(BindRot[I], Key)) : Key;
            }
            if (!C.Pos.empty())
            {
                const Vec Key = Sample(C.Pos, F, &ToVec, LerpVec);
                Pos = C.OffsetsBind ? Add(BindPos[I], Key) : Key;
            }
            const int32_t P = Rig.Bones[I].Parent;
            if (P < 0) { WorldRot[I] = Rot; WorldPos[I] = Pos; }
            else
            {
                WorldRot[I] = Normalized(Mul(WorldRot[P], Rot));
                WorldPos[I] = Add(WorldPos[P], Rotate(WorldRot[P], Pos));
            }
            if (PartOf[I] < 0) continue;   // a bone the anim leaves at its bind, posed for its children only

            XBin::WriteField(Writer, "PART", XAnimBinValueType::UInt16);
            Writer.Write<uint16_t>((uint16_t)PartOf[I]);
            XBin::WriteField(Writer, "OFFSET", XAnimBinValueType::Raw32x3);
            Writer.Write((float)WorldPos[I].X);
            Writer.Write((float)WorldPos[I].Y);
            Writer.Write((float)WorldPos[I].Z);
            // the rotation's axes in model space: the matrix's columns, X/Y/Z as XANIM_EXPORT names them
            WriteAxes(Writer, "X", Rotate(WorldRot[I], { 1, 0, 0 }));
            WriteAxes(Writer, "Y", Rotate(WorldRot[I], { 0, 1, 0 }));
            WriteAxes(Writer, "Z", Rotate(WorldRot[I], { 0, 0, 1 }));
        }
    }

    // the notetracks, by frame then name: NUMKEYS, then each key's frame and name (the stock files'
    // flat form); "end" and "loop_end" are the linker's own (the stock sources have neither)
    std::vector<std::pair<uint32_t, std::string>> Notes;
    for (const auto& [Name, Keys] : Animation.AnimationNotetracks)
    {
        if (Name == "end" || Name == "loop_end") continue;
        for (const auto Frame : Keys) Notes.emplace_back(Frame, Name);
    }
    std::sort(Notes.begin(), Notes.end());
    if (Notes.size() > 0xFFFF) Notes.resize(0xFFFF);
    XBin::WriteField(Writer, "NUMKEYS", XAnimBinValueType::UInt16);
    Writer.Write<uint16_t>((uint16_t)Notes.size());
    for (const auto& [Frame, Name] : Notes)
    {
        XBin::WriteField(Writer, "FRAME", XAnimBinValueType::UInt32String);
        Writer.Write<uint32_t>(Frame);
        XBin::WriteStringAligned(Writer, Name);
    }

    Local.Parts = (uint32_t)Parts.size();
    Local.Frames = Frames;
    Local.Notes = (uint32_t)Notes.size();
    if (Stats) *Stats = std::move(Local);
    return XBin::SaveContainer(FileName, Writer.GetCurrentStream(), (uint32_t)Writer.GetPosition());
}
