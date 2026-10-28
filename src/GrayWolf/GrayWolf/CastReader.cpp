#include "stdafx.h"
#include "ByteView.h"

#include "CastReader.h"
#include "SeAnimReader.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <unordered_map>

namespace CastFile
{
namespace
{
    constexpr uint32_t Magic = 0x74736163;   // "cast"
    constexpr uint16_t TypeV2 = 0x7632, TypeV3 = 0x7633, TypeV4 = 0x7634;

    // An element's size in bytes; 0 for a string (null terminated) and for an unknown type
    size_t ElementSize(uint16_t Type)
    {
        switch (Type)
        {
        case 'b': return 1;
        case 'h': return 2;
        case 'i': case 'f': return 4;
        case 'l': case 'd': return 8;
        case TypeV2: return 8;
        case TypeV3: return 12;
        case TypeV4: return 16;
        default: return 0;
        }
    }

    template <typename T>
    bool Take(ByteView B, size_t& O, T& V)
    {
        if (O + sizeof(T) > B.size()) return false;
        std::memcpy(&V, B.data() + O, sizeof(T));
        O += sizeof(T);
        return true;
    }

    // Depth caps a malformed file's recursion
    bool ParseNode(ByteView B, size_t& O, Node& Out, int Depth)
    {
        uint32_t Size = 0, Props = 0, Children = 0;
        const size_t Start = O;
        if (Depth > 32 || !Take(B, O, Out.Id) || !Take(B, O, Size) || !Take(B, O, Out.Hash) || !Take(B, O, Props) ||
            !Take(B, O, Children))
            return false;
        if (Start + Size > B.size() || Props > Size || Children > Size) return false;
        for (uint32_t P = 0; P < Props; P++)
        {
            Property Prop;
            uint16_t NameSize = 0;
            if (!Take(B, O, Prop.Type) || !Take(B, O, NameSize) || !Take(B, O, Prop.Count) || O + NameSize > B.size())
                return false;
            std::string Name((const char*)B.data() + O, NameSize);
            O += NameSize;
            Prop.Data = B.data() + O;
            if (Prop.Type == 's')
            {
                const void* End = std::memchr(B.data() + O, 0, B.size() - O);
                if (End == nullptr) return false;
                Prop.Bytes = (const uint8_t*)End - (B.data() + O) + 1;
            }
            else
            {
                const size_t Element = ElementSize(Prop.Type);
                if (Element == 0 || (uint64_t)Element * Prop.Count > B.size() - O) return false;
                Prop.Bytes = Element * Prop.Count;
            }
            O += Prop.Bytes;
            Out.Properties[Name] = Prop;
        }
        Out.Children.resize(Children);
        for (auto& Child : Out.Children)
            if (!ParseNode(B, O, Child, Depth + 1)) return false;
        return true;
    }

    // Streaming over the node headers, for what a file holds without reading its buffers
    struct HeaderWalk
    {
        std::ifstream In;
        uint64_t Size = 0;

        explicit HeaderWalk(const std::string& Path) : In(Path, std::ios::binary | std::ios::ate)
        {
            if (In) { Size = (uint64_t)In.tellg(); In.seekg(0); }
        }
        template <typename T> bool Read(T& V) { return (bool)In.read((char*)&V, sizeof(T)); }

        struct Header { uint32_t Id = 0, Size = 0, Props = 0, Children = 0; uint64_t Start = 0; };

        // A node's header, its properties skipped: the stream is at its first child
        bool Open(Header& H)
        {
            uint64_t Hash = 0;
            H.Start = (uint64_t)In.tellg();
            if (!Read(H.Id) || !Read(H.Size) || !Read(Hash) || !Read(H.Props) || !Read(H.Children)) return false;
            for (uint32_t P = 0; P < H.Props; P++)
            {
                uint16_t Type = 0, NameSize = 0;
                uint32_t Count = 0;
                if (!Read(Type) || !Read(NameSize) || !Read(Count)) return false;
                In.seekg(NameSize, std::ios::cur);
                if (Type == 's')
                {
                    char C = 1;
                    while (C != 0 && In.get(C)) {}
                }
                else
                    In.seekg((std::streamoff)(ElementSize(Type) * Count), std::ios::cur);
                if (!In) return false;
            }
            return true;
        }
        bool Skip(const Header& H) { In.seekg((std::streamoff)(H.Start + H.Size)); return (bool)In && H.Start + H.Size <= Size; }

        // Positions the stream at the first Model or Animation node under the roots
        bool FindAsset(Header& Out)
        {
            uint32_t Head[4] = {};
            if (!In.read((char*)Head, sizeof(Head)) || Head[0] != Magic) return false;
            for (uint32_t R = 0; R < Head[2]; R++)
            {
                Header Root;
                if (!Open(Root)) return false;
                for (uint32_t C = 0; C < Root.Children; C++)
                {
                    Header Child;
                    if (!Open(Child)) return false;
                    if (Child.Id == IdModel || Child.Id == IdAnimation) { Out = Child; return true; }
                    if (!Skip(Child)) return false;
                }
                if (!Skip(Root)) return false;
            }
            return false;
        }
    };
}

uint64_t Property::Integer(size_t I) const
{
    const size_t Element = ElementSize(Type);
    if (Element == 0 || (I + 1) * Element > Bytes) return 0;
    const uint8_t* P = Data + I * Element;
    switch (Type)
    {
    case 'b': return *P;
    case 'h': { uint16_t V; std::memcpy(&V, P, 2); return V; }
    case 'i': { uint32_t V; std::memcpy(&V, P, 4); return V; }
    case 'l': { uint64_t V; std::memcpy(&V, P, 8); return V; }
    default: return 0;
    }
}

size_t Property::Floats() const
{
    return (Type == 'f' || Type == TypeV2 || Type == TypeV3 || Type == TypeV4) ? Bytes / 4 : 0;
}

float Property::Float(size_t I) const
{
    if (I >= Floats()) return 0.0f;
    float V;
    std::memcpy(&V, Data + I * 4, 4);
    return V;
}

std::string Property::String() const
{
    return (Type == 's' && Bytes > 0) ? std::string((const char*)Data, Bytes - 1) : std::string();
}

const Property* Node::Find(const std::string& Name) const
{
    const auto It = Properties.find(Name);
    return It == Properties.end() ? nullptr : &It->second;
}

std::string Node::String(const std::string& Name) const
{
    const Property* P = Find(Name);
    return P ? P->String() : std::string();
}

uint64_t Node::Integer(const std::string& Name, uint64_t Default) const
{
    const Property* P = Find(Name);
    return (P && P->Count > 0 && ElementSize(P->Type) > 0 && P->Floats() == 0) ? P->Integer(0) : Default;
}

bool Parse(ByteView Bytes, std::vector<Node>& Roots)
{
    size_t O = 0;
    uint32_t Head[4] = {};
    for (auto& H : Head)
        if (!Take(Bytes, O, H)) return false;
    if (Head[0] != Magic) return false;
    Roots.assign(std::min<uint32_t>(Head[2], 64), Node{});
    for (auto& Root : Roots)
        if (!ParseNode(Bytes, O, Root, 0)) return false;
    return true;
}

Kind FileKind(const std::string& Path)
{
    HeaderWalk W(Path);
    HeaderWalk::Header Asset;
    if (!W.In || !W.FindAsset(Asset)) return Kind::None;
    return Asset.Id == IdModel ? Kind::Model : Kind::Animation;
}

bool ModelCounts(const std::string& Path, uint32_t& Bones, uint32_t& Meshes, uint32_t& Materials)
{
    Bones = Meshes = Materials = 0;
    HeaderWalk W(Path);
    HeaderWalk::Header Model;
    if (!W.In || !W.FindAsset(Model) || Model.Id != IdModel) return false;
    for (uint32_t C = 0; C < Model.Children; C++)
    {
        HeaderWalk::Header Child;
        if (!W.Open(Child)) return false;
        if (Child.Id == IdMesh) Meshes++;
        else if (Child.Id == IdMaterial) Materials++;
        else if (Child.Id == IdSkeleton)
            for (uint32_t B = 0; B < Child.Children; B++)
            {
                HeaderWalk::Header Bone;
                if (!W.Open(Bone)) return false;
                if (Bone.Id == IdBone) Bones++;
                if (!W.Skip(Bone)) return false;
            }
        if (!W.Skip(Child)) return false;
    }
    return true;
}
}

namespace
{
    SeAnimBoneType ModeType(const std::string& Mode)
    {
        if (Mode == "additive") return SeAnimBoneType::Additive;
        if (Mode == "relative") return SeAnimBoneType::Relative;
        return SeAnimBoneType::Absolute;
    }

    // One float channel (a tx curve...): its keys by frame
    using Channel = std::vector<std::pair<uint32_t, float>>;

    // The channel at Frame: its key there, else linear between its neighbours, else the nearest
    float Evaluate(const Channel& C, uint32_t Frame, float Default)
    {
        if (C.empty()) return Default;
        auto It = std::lower_bound(C.begin(), C.end(), Frame, [](const auto& K, uint32_t F) { return K.first < F; });
        if (It == C.end()) return C.back().second;
        if (It->first == Frame || It == C.begin()) return It->second;
        const auto& [F1, V1] = *It;
        const auto& [F0, V0] = *(It - 1);
        return V0 + (V1 - V0) * (float)(Frame - F0) / (float)(F1 - F0);
    }

    // x, y and z channels into keys at every frame any of them keys
    std::vector<SeAnimKeyVec3> Merge(Channel (&C)[3], float Default)
    {
        std::vector<uint32_t> Frames;
        for (auto& Ch : C)
        {
            std::stable_sort(Ch.begin(), Ch.end(), [](const auto& A, const auto& B) { return A.first < B.first; });
            for (const auto& K : Ch) Frames.push_back(K.first);
        }
        std::sort(Frames.begin(), Frames.end());
        Frames.erase(std::unique(Frames.begin(), Frames.end()), Frames.end());
        std::vector<SeAnimKeyVec3> Keys;
        for (uint32_t F : Frames)
            Keys.push_back({ F, { Evaluate(C[0], F, Default), Evaluate(C[1], F, Default), Evaluate(C[2], F, Default) } });
        return Keys;
    }
}

bool ReadCastAnim(const std::string& Path, SeAnimClip& Out)
{
    std::vector<uint8_t> Bytes;
    {
        std::ifstream In(Path, std::ios::binary);
        if (!In) return false;
        Bytes.assign(std::istreambuf_iterator<char>(In), std::istreambuf_iterator<char>());
    }
    std::vector<CastFile::Node> Roots;
    if (!CastFile::Parse(Bytes, Roots)) return false;
    const CastFile::Node* Anim = nullptr;
    for (const auto& Root : Roots)
        for (const auto& Child : Root.Children)
            if (Anim == nullptr && Child.Id == CastFile::IdAnimation) Anim = &Child;
    if (Anim == nullptr) return false;

    Out = SeAnimClip{};
    if (const auto* Fr = Anim->Find("fr"); Fr != nullptr && Fr->Floats() > 0) Out.FrameRate = Fr->Float(0);
    Out.Looping = Anim->Integer("lo") != 0;

    struct Track { Channel T[3], S[3]; std::vector<SeAnimKeyQuat> R; std::string Mode; bool Override = false; };
    std::vector<std::string> Order;   // bones in file order
    std::unordered_map<std::string, Track> Tracks;
    const auto Get = [&](const std::string& Bone) -> Track& {
        auto [It, New] = Tracks.try_emplace(Bone);
        if (New) Order.push_back(Bone);
        return It->second;
    };
    uint32_t Last = 0;
    bool Typed = false;
    for (const auto& C : Anim->Children)
    {
        if (C.Id == CastFile::IdCurve)
        {
            const std::string Bone = C.String("nn"), Key = C.String("kp"), Mode = C.String("m");
            const auto* Kb = C.Find("kb");
            const auto* Kv = C.Find("kv");
            if (Kb == nullptr || Kv == nullptr || Key == "bs" || Key == "vb") continue;
            Track& T = Get(Bone);
            const size_t Width = (Key == "rq") ? 4 : 1;
            for (uint32_t K = 0; K < Kb->Count && (K + 1) * Width <= Kv->Floats(); K++)
            {
                const uint32_t Frame = (uint32_t)Kb->Integer(K);
                Last = std::max(Last, Frame);
                if (Key == "rq")
                    T.R.push_back({ Frame, { Kv->Float(K * 4), Kv->Float(K * 4 + 1), Kv->Float(K * 4 + 2), Kv->Float(K * 4 + 3) } });
                else if (Key.size() == 2 && (Key[0] == 't' || Key[0] == 's') && Key[1] >= 'x' && Key[1] <= 'z')
                    (Key[0] == 't' ? T.T : T.S)[Key[1] - 'x'].push_back({ Frame, Kv->Float(K) });
            }
            if (Key[0] == 't' && !T.Override)
            {
                T.Mode = Mode;
                if (!Typed) { Out.Type = ModeType(Mode); Typed = true; }
            }
        }
        else if (C.Id == CastFile::IdCurveModeOverride)
        {
            Track& T = Get(C.String("nn"));
            T.Mode = C.String("m");
            T.Override = true;
        }
        else if (C.Id == CastFile::IdNotificationTrack)
        {
            auto& Frames = Out.Notifications[C.String("n")];
            if (const auto* Kb = C.Find("kb"))
                for (uint32_t K = 0; K < Kb->Count; K++)
                {
                    Frames.push_back((uint32_t)Kb->Integer(K));
                    Last = std::max(Last, Frames.back());
                }
        }
    }

    for (const auto& Bone : Order)
    {
        Track& T = Tracks[Bone];
        SeAnimBoneTrack B;
        B.Name = Bone;
        if (!T.Mode.empty())
        {
            B.TypeOverride = ModeType(T.Mode);
            B.HasTypeOverride = B.TypeOverride != Out.Type;
        }
        std::stable_sort(T.R.begin(), T.R.end(), [](const auto& A, const auto& C) { return A.Frame < C.Frame; });
        B.RotKeys = std::move(T.R);
        B.PosKeys = Merge(T.T, 0.0f);
        B.ScaleKeys = Merge(T.S, 1.0f);
        Out.Bones.push_back(std::move(B));
    }
    Out.FrameCount = Last + 1;
    return true;
}

bool ReadAnimClip(const std::string& Path, SeAnimClip& Out)
{
    char Head[4] = {};
    std::ifstream In(Path, std::ios::binary);
    if (!In.read(Head, sizeof(Head))) return false;
    In.close();
    return std::memcmp(Head, "cast", 4) == 0 ? ReadCastAnim(Path, Out) : ReadSeAnim(Path, Out);
}
