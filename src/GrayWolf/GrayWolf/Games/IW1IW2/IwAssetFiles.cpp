#include "stdafx.h"

#include "IwAssetFiles.h"

#include <cmath>
#include <cstring>
#include <set>
#include <stdexcept>

namespace IW12
{
namespace
{
    // Bounds-checked little-endian reads; a read past the end throws with the offset.
    class Reader
    {
    public:
        Reader(const uint8_t* Data, size_t Size) : P(Data), N(Size) {}

        template <class T> T Get()
        {
            Need(sizeof(T));
            T V;
            std::memcpy(&V, P + Pos, sizeof(T));
            Pos += sizeof(T);
            return V;
        }
        void Vec(float* Out, int Count)
        {
            for (int I = 0; I < Count; I++) Out[I] = Get<float>();
        }
        std::string Str()
        {
            const size_t Start = Pos;
            while (true)
            {
                Need(1);
                if (P[Pos++] == 0) break;
            }
            return std::string(reinterpret_cast<const char*>(P + Start), Pos - Start - 1);
        }
        void Skip(size_t Count)
        {
            Need(Count);
            Pos += Count;
        }
        size_t Tell() const { return Pos; }
        size_t Left() const { return N - Pos; }
        void Seek(size_t To)
        {
            if (To > N) Fail("seek past the end");
            Pos = To;
        }
        [[noreturn]] void Fail(const std::string& What) const
        {
            throw std::runtime_error(What + " at " + std::to_string(Pos));
        }
        void End(const char* What) const
        {
            if (Pos != N) Fail(std::string(What) + ": " + std::to_string(N - Pos) + " bytes left");
        }

    private:
        void Need(size_t Count) const
        {
            if (Count > N - Pos) Fail("read of " + std::to_string(Count) + " past the end (" + std::to_string(N) + ")");
        }
        const uint8_t* P;
        size_t N, Pos = 0;
    };

    template <class F>
    bool Guard(F Body, std::string& Error)
    {
        try
        {
            Body();
            return true;
        }
        catch (const std::exception& E)
        {
            Error = E.what();
            return false;
        }
    }

    uint16_t Version(Reader& R, std::initializer_list<uint16_t> Known)
    {
        const uint16_t V = R.Get<uint16_t>();
        for (uint16_t K : Known)
            if (V == K) return V;
        R.Fail("version " + std::to_string(V));
    }

    // A key list: its count, then its frame numbers unless it holds one key or every frame (u8,
    // u16 when the animation has more than 256 frames), then the keys.
    void ReadTrack(Reader& R, XAnimFile::Track& T, uint32_t Frames, bool Rotation, bool Simple, bool Flip)
    {
        const uint16_t Count = R.Get<uint16_t>();
        if (Count == 0) return;
        T.Frames.resize(Count);
        if (Count == 1 || Count == Frames)
            for (uint16_t I = 0; I < Count; I++) T.Frames[I] = I;
        else
            for (uint16_t I = 0; I < Count; I++) T.Frames[I] = Frames > 256 ? R.Get<uint16_t>() : R.Get<uint8_t>();
        T.Keys.resize(Count);
        for (uint16_t I = 0; I < Count; I++)
        {
            if (Rotation)
            {
                int16_t X = 0, Y = 0, Z;
                if (!Simple)
                {
                    X = R.Get<int16_t>();
                    Y = R.Get<int16_t>();
                }
                Z = R.Get<int16_t>();
                T.Keys[I] = Quat(X, Y, Z, Flip);
            }
            else
            {
                float V[3];
                R.Vec(V, 3);
                T.Keys[I] = { V[0], V[1], V[2], 0 };
            }
        }
    }
}

std::array<float, 4> Quat(int16_t X, int16_t Y, int16_t Z, bool Flip)
{
    const float Fx = X / 32767.0f, Fy = Y / 32767.0f, Fz = Z / 32767.0f;
    const float W2 = 1.0f - Fx * Fx - Fy * Fy - Fz * Fz;
    float W = W2 > 0 ? std::sqrt(W2) : 0.0f;
    if (Flip) W = -W;
    return { Fx, Fy, Fz, W };
}

bool ParseXModel(const uint8_t* Data, size_t Size, XModelFile& Out, std::string& Error)
{
    return Guard([&] {
        Reader R(Data, Size);
        Out = XModelFile();
        Out.Version = Version(R, { 14, 20 });
        if (Out.Version == 20) R.Skip(1);   // flags
        R.Skip(24);                          // bounds
        const int LodSlots = Out.Version == 14 ? 3 : 4;
        for (int I = 0; I < LodSlots; I++)
        {
            XModelFile::Lod L;
            L.Distance = R.Get<float>();
            L.Name = R.Str();
            if (!L.Name.empty()) Out.Lods.push_back(std::move(L));
        }
        Out.CollisionLod = R.Get<int32_t>();
        const int32_t CollSurfs = R.Get<int32_t>();
        for (int32_t S = 0; S < CollSurfs; S++)
        {
            const int32_t Tris = R.Get<int32_t>();
            if (Tris < 0) R.Fail("negative collision triangle count");
            R.Skip((size_t)Tris * 48 + 36);   // planes and svec/tvec; bounds, bone, contents, flags
        }
        for (auto& L : Out.Lods)
        {
            const uint16_t Count = R.Get<uint16_t>();
            for (uint16_t M = 0; M < Count; M++) L.Materials.push_back(R.Str());
        }
        Out.Trailing = R.Left();
        if (Out.Version == 14) R.End("xmodel");
    }, Error);
}

bool ParseXModelParts(const uint8_t* Data, size_t Size, XModelPartsFile& Out, std::string& Error)
{
    return Guard([&] {
        Reader R(Data, Size);
        Out = XModelPartsFile();
        Out.Version = Version(R, { 14, 20 });
        const uint16_t Others = R.Get<uint16_t>();
        Out.Roots = R.Get<uint16_t>();
        const size_t Total = (size_t)Others + Out.Roots;
        Out.Bones.resize(Total);
        for (size_t I = Out.Roots; I < Total; I++)
        {
            auto& B = Out.Bones[I];
            B.Parent = R.Get<uint8_t>();
            if (B.Parent >= (int)I) R.Fail("bone " + std::to_string(I) + " has parent " + std::to_string(B.Parent));
            R.Vec(B.Position, 3);
            for (auto& C : B.Rotation) C = R.Get<int16_t>();
        }
        for (auto& B : Out.Bones)
        {
            B.Name = R.Str();
            if (Out.Version == 14) R.Skip(24);   // bounds
        }
        R.Skip(Total);   // part classification
        R.End("xmodelparts");
    }, Error);
}

bool ParseXModelSurfs(const uint8_t* Data, size_t Size, XModelSurfsFile& Out, std::string& Error)
{
    return Guard([&] {
        Reader R(Data, Size);
        Out = XModelSurfsFile();
        Out.Version = Version(R, { 14, 20 });
        const uint16_t Count = R.Get<uint16_t>();
        Out.Surfaces.resize(Count);
        for (auto& S : Out.Surfaces)
        {
            R.Skip(1);   // tile mode
            const uint16_t Verts = R.Get<uint16_t>();
            S.HeaderFaces = R.Get<uint16_t>();
            S.Vertices.resize(Verts);
            if (Out.Version == 14)
            {
                const uint16_t Strips = R.Get<uint16_t>();
                const uint16_t Bone = R.Get<uint16_t>();
                const bool Weighted = Bone == 0xFFFF;
                if (Weighted) R.Skip(4);   // the extra influence count, and a count of ours unknown
                // triangle strips: the odd triangles swap their first two corners; degenerate
                // ones (a repeated index) are not drawn and not counted
                for (uint16_t K = 0; K < Strips; K++)
                {
                    const uint8_t N = R.Get<uint8_t>();
                    std::vector<uint16_t> I(N);
                    for (auto& V : I) V = R.Get<uint16_t>();
                    for (int J = 0; J + 2 < N; J++)
                    {
                        std::array<uint16_t, 3> F = { I[J], I[J + 1], I[J + 2] };
                        if (J & 1) std::swap(F[0], F[1]);
                        if (F[0] != F[1] && F[1] != F[2] && F[0] != F[2]) S.Faces.push_back(F);
                    }
                }
                std::vector<uint16_t> Extra(Verts, 0);
                for (uint16_t V = 0; V < Verts; V++)
                {
                    auto& X = S.Vertices[V];
                    R.Vec(X.Normal, 3);
                    R.Vec(X.UV, 2);
                    XModelSurfsFile::Influence First;
                    First.Bone = Bone;
                    if (Weighted)
                    {
                        Extra[V] = R.Get<uint16_t>();
                        First.Bone = R.Get<uint16_t>();
                    }
                    R.Vec(First.Offset, 3);
                    if (Extra[V]) First.Weight = R.Get<float>();
                    X.Influences.push_back(First);
                }
                for (uint16_t V = 0; V < Verts; V++)
                    for (uint16_t K = 0; K < Extra[V]; K++)
                    {
                        XModelSurfsFile::Influence I;
                        I.Bone = R.Get<uint16_t>();
                        R.Vec(I.Offset, 3);
                        I.Weight = R.Get<float>();
                        S.Vertices[V].Influences.push_back(I);
                    }
            }
            else
            {
                const uint16_t Bone = R.Get<uint16_t>();
                const bool Weighted = Bone == 0xFFFF;
                if (Weighted) R.Skip(2);
                for (auto& X : S.Vertices)
                {
                    R.Vec(X.Normal, 3);
                    for (auto& C : X.Color) C = R.Get<uint8_t>();
                    R.Vec(X.UV, 2);
                    R.Skip(24);   // binormal, tangent
                    XModelSurfsFile::Influence First;
                    First.Bone = Bone;
                    uint8_t Extra = 0;
                    if (Weighted)
                    {
                        Extra = R.Get<uint8_t>();
                        First.Bone = R.Get<uint16_t>();
                    }
                    R.Vec(First.Offset, 3);
                    X.Influences.push_back(First);
                    if (Extra)
                    {
                        R.Skip(1);
                        float Rest = 1.0f;
                        for (uint8_t K = 0; K < Extra; K++)
                        {
                            XModelSurfsFile::Influence I;
                            I.Bone = R.Get<uint16_t>();
                            R.Vec(I.Offset, 3);
                            I.Weight = R.Get<uint16_t>() / 65535.0f;
                            Rest -= I.Weight;
                            X.Influences.push_back(I);
                        }
                        X.Influences[0].Weight = Rest;
                    }
                }
                S.Faces.resize(S.HeaderFaces);
                for (auto& F : S.Faces)
                    for (auto& V : F) V = R.Get<uint16_t>();
            }
            for (const auto& F : S.Faces)
                for (uint16_t V : F)
                    if (V >= Verts) R.Fail("face index " + std::to_string(V) + " of " + std::to_string(Verts));
        }
        R.End("xmodelsurfs");
    }, Error);
}

bool ParseXAnim(const uint8_t* Data, size_t Size, XAnimFile& Out, std::string& Error)
{
    return Guard([&] {
        Reader R(Data, Size);
        Out = XAnimFile();
        Out.Version = Version(R, { 14 });
        Out.FrameCount = R.Get<uint16_t>();
        const uint16_t Parts = R.Get<uint16_t>();
        Out.Flags = R.Get<uint8_t>();
        Out.FrameRate = R.Get<uint16_t>();
        const uint32_t Frames = Out.Frames();   // a looping animation's extra frame counts here
        if (Out.Delta())
        {
            Out.DeltaPart.Name = "tag_origin";
            Out.DeltaPart.Simple = true;
            ReadTrack(R, Out.DeltaPart.Rotations, Frames, true, true, false);
            ReadTrack(R, Out.DeltaPart.Translations, Frames, false, false, false);
        }
        const size_t BitBytes = Parts == 0 ? 0 : ((Parts - 1) >> 3) + 1;   // a delta-only animation has none
        std::vector<uint8_t> Flip(BitBytes), Simple(BitBytes);
        for (auto& B : Flip) B = R.Get<uint8_t>();
        for (auto& B : Simple) B = R.Get<uint8_t>();
        Out.Parts.resize(Parts);
        for (auto& P : Out.Parts) P.Name = R.Str();
        for (uint16_t I = 0; I < Parts; I++)
        {
            auto& P = Out.Parts[I];
            P.Flip = (Flip[I >> 3] >> (I & 7)) & 1;
            P.Simple = (Simple[I >> 3] >> (I & 7)) & 1;
            ReadTrack(R, P.Rotations, Frames, true, P.Simple, P.Flip);
            ReadTrack(R, P.Translations, Frames, false, false, false);
        }
        const uint8_t Notes = R.Get<uint8_t>();
        for (uint8_t I = 0; I < Notes; I++)
        {
            std::string Name = R.Str();
            Out.Notes.emplace_back(std::move(Name), R.Get<uint16_t>());
        }
        R.End("xanim");
    }, Error);
}

bool ParseMaterial(const uint8_t* Data, size_t Size, MaterialFile& Out, std::string& Error)
{
    return Guard([&] {
        Reader R(Data, Size);
        Out = MaterialFile();
        // what the pool holds: a string or a water block per offset; together they must cover it
        std::set<std::pair<size_t, size_t>> Pool;
        const auto StrAt = [&](uint32_t Offset) {
            const size_t Back = R.Tell();
            R.Seek(Offset);
            std::string S = R.Str();
            Pool.insert({ Offset, R.Tell() });
            R.Seek(Back);
            return S;
        };
        const uint32_t NameAt = R.Get<uint32_t>();
        const uint32_t RefImageAt = R.Get<uint32_t>();
        R.Seek(0xe);
        Out.AtlasRows = R.Get<uint8_t>();
        Out.AtlasCols = R.Get<uint8_t>();
        R.Seek(0x34);
        const uint8_t Textures = R.Get<uint8_t>();
        R.Skip(1);
        const uint16_t Constants = R.Get<uint16_t>();
        const uint32_t TechsetAt = R.Get<uint32_t>();
        const uint32_t TexturesAt = R.Get<uint32_t>();
        const uint32_t ConstantsAt = R.Get<uint32_t>();
        Out.Name = StrAt(NameAt);
        StrAt(RefImageAt);
        Out.Techset = StrAt(TechsetAt);
        size_t HeaderEnd = R.Tell();
        R.Seek(TexturesAt);
        for (uint8_t T = 0; T < Textures; T++)
        {
            const uint32_t SamplerAt = R.Get<uint32_t>();
            R.Skip(1);   // sampler state
            const uint8_t Semantic = R.Get<uint8_t>();
            R.Skip(2);
            const uint32_t ImageAt = R.Get<uint32_t>();
            const std::string Sampler = StrAt(SamplerAt);
            if (Semantic == 5)   // water: a 32-byte block of wave parameters, no image file
            {
                if ((size_t)ImageAt + 32 > Size) R.Fail("water block past the end");
                Pool.insert({ ImageAt, ImageAt + 32 });
                continue;
            }
            Out.Textures.emplace_back(Sampler, StrAt(ImageAt));
        }
        HeaderEnd = std::max(HeaderEnd, R.Tell());
        R.Seek(ConstantsAt);
        for (uint16_t K = 0; K < Constants; K++)
        {
            StrAt(R.Get<uint32_t>());
            R.Skip(16);
        }
        HeaderEnd = std::max(HeaderEnd, R.Tell());
        // the pool runs from the header's end to the file's, every byte referenced
        size_t At = HeaderEnd;
        for (const auto& [From, To] : Pool)
        {
            if (From > At) R.Fail("unreferenced bytes at " + std::to_string(At));
            At = std::max(At, To);
        }
        if (At != Size) R.Fail("material: " + std::to_string(Size - At) + " bytes left");
    }, Error);
}
}
