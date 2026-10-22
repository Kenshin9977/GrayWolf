#include "stdafx.h"

#include "Games/Treyarch/TreyarchMaps.h"

#include "ExportNaming.h"
#include "Games/Common/MeshDecode.h"
#include "Games/IW/IwctxReaders.h"

#include <cstdio>
#include <cstring>

namespace Treyarch
{
using ExportNaming::At;

Maps::WorldBuilder::Fetch WorldVertices(const std::vector<uint8_t>& Img, int64_t Positions, int64_t Data, uint32_t Count)
{
    return [&Img, Positions, Data, Count](uint32_t I, Maps::Vertex& V) {
        if (I >= Count || Positions < 0 || Data < 0) return false;
        for (int K = 0; K < 3; K++) V.Pos[K] = At<float>(Img, Positions + (int64_t)I * 12 + K * 4);
        const int64_t D = Data + (int64_t)I * 20;
        const uint32_t Color = At<uint32_t>(Img, D);
        std::memcpy(V.Color, &Color, 4);
        V.Uv[0] = At<float>(Img, D + 4);
        V.Uv[1] = At<float>(Img, D + 8);
        const Vector3 Normal = MeshDecode::UnpackSigned10((int32_t)At<uint32_t>(Img, D + 12));
        V.Normal[0] = Normal.X, V.Normal[1] = Normal.Y, V.Normal[2] = Normal.Z;
        return true;
    };
}

void DynamicEntities(const std::vector<uint8_t>& Img, int64_t List, uint32_t Count, int64_t Stride, const std::string& Prefix,
                     const std::function<std::string(uint64_t)>& ModelAt, Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models)
{
    for (uint32_t K = 0; List >= 0 && K < Count; K++)
    {
        const int64_t E = List + (int64_t)K * Stride;
        const uint64_t Model = At<uint64_t>(Img, E + 32);
        const std::string Name = ModelAt(Model);
        if (Name.empty()) continue;
        Maps::Instance In;
        In.Name = Prefix + std::to_string(K);
        In.Model = Name;
        float Q[4];
        for (int J = 0; J < 4; J++) Q[J] = At<float>(Img, E + J * 4);
        Maps::AxisFromQuat(Q, In.Axis);
        for (int J = 0; J < 3; J++) In.Origin[J] = At<float>(Img, E + 16 + J * 4);
        In.Destroyed = ModelAt(At<uint64_t>(Img, E + 40));
        Scene.DynamicInstances.push_back(In);
        Models.emplace(Name, Model);
    }
}

void Lights(const std::vector<uint8_t>& Img, int64_t List, uint32_t Count, const LightFields& F, Maps::MapScene& Scene)
{
    for (uint32_t K = 0; List >= 0 && K < Count; K++)
    {
        const int64_t L = List + (int64_t)K * F.Size;
        const uint8_t Type = At<uint8_t>(Img, L + 64);
        if (Type != 1 && Type != 2 && Type != 4) continue;
        Maps::Light Out;
        Out.Kind = Type == 1 ? Maps::Light::Type::Sun : Type == 2 ? Maps::Light::Type::Spot : Maps::Light::Type::Point;
        for (int J = 0; J < 3; J++)
        {
            Out.Origin[J] = At<float>(Img, L + F.Origin + J * 4);
            Out.Dir[J] = At<float>(Img, L + F.Dir + J * 4);
            Out.Color[J] = At<float>(Img, L + F.Color + J * 4);
        }
        Out.CosHalfFovOuter = At<float>(Img, L + F.CosHalfFov);
        Out.Radius = At<float>(Img, L + F.Radius);
        Scene.Lights.push_back(Out);
    }
}

std::string EntityText(const std::vector<uint8_t>& Img, int64_t Array, uint64_t Count, const std::function<int64_t(uint64_t)>& Off,
                       const std::function<std::string(uint64_t)>& String, const std::function<std::string(uint64_t)>& HashName,
                       EntityCounts& N, const std::function<std::string(uint64_t Entity, int32_t ModelIndex)>& Model)
{
    constexpr int64_t EntSize = 0x30, KvpSize = 0x20;
    std::string S;
    char Buf[96];
    for (uint64_t E = 0; Array >= 0 && E < Count; E++)
    {
        const int64_t En = Array + (int64_t)E * EntSize;
        const int64_t Pairs = Off(At<uint64_t>(Img, En + 8));
        const uint32_t Pc = At<uint32_t>(Img, En);
        S += "{\n";
        bool HasModel = false;
        for (uint32_t K = 0; Pairs >= 0 && K < Pc; K++)
        {
            const int64_t P = Pairs + (int64_t)K * KvpSize;
            const std::string Key = String(At<uint64_t>(Img, P));
            HasModel |= Key == "model";
            N.Keys++;
            N.KeysNamed += !Key.empty();
            std::string Value;
            switch (At<int16_t>(Img, P + 0x18))
            {
            case 2: Value = String(At<uint64_t>(Img, P + 8)); break;
            case 3:
                std::snprintf(Buf, sizeof(Buf), "%g %g %g", At<float>(Img, P + 8), At<float>(Img, P + 12), At<float>(Img, P + 16));
                Value = Buf;
                break;
            case 4:
            {
                N.Hashes++;
                const uint64_t Hash = At<uint64_t>(Img, P + 8) & 0x0FFFFFFFFFFFFFFFull;
                Value = HashName(Hash);
                if (!Value.empty()) N.HashesNamed++;
                else
                {
                    std::snprintf(Buf, sizeof(Buf), "#%llx", (unsigned long long)Hash);
                    Value = Buf;
                }
                break;
            }
            case 5: std::snprintf(Buf, sizeof(Buf), "%g", At<float>(Img, P + 8)); Value = Buf; break;
            case 6: Value = std::to_string(At<uint32_t>(Img, P + 8)); break;
            default: break;
            }
            S += "\"" + Key + "\" \"" + Value + "\"\n";
        }
        if (Model && !HasModel)
            if (const std::string M = Model(E, At<int32_t>(Img, En + 0x10)); !M.empty()) S += "\"model\" \"" + M + "\"\n";
        S += "}\n";
    }
    return S;
}
}

void Treyarch::ExportMapEnts(const DiskGame::Context& C, DiskGame::Totals& T, const std::string& Zone, const std::string& Text)
{
    std::vector<Maps::Entity> Parsed;
    if (Text.empty()) return;
    if (!Maps::ParseEntities(Text, Parsed))
    {
        T.Fail(Zone, DiskGame::Kind::MapEnts, Zone, "entity text not parsed");
        return;
    }
    std::vector<IwctxReaders::MapEntity> Entities;
    for (const auto& E : Parsed) Entities.emplace_back(E.begin(), E.end());
    IwctxReaders::ExportMapEnts(C, T, Zone, Entities);
}
