#include "stdafx.h"

#include "Vfx.h"

#include "ExportNaming.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace Vfx
{
namespace
{
    using Json = nlohmann::ordered_json;
    using ExportNaming::At;

    constexpr int MaxCount = 1 << 16;      // a count beyond is not one (a wrong layout)
    constexpr int MaxKeys = 4096;

    const char* const GroupNames[] = { "init", "update", "test" };

    std::string Hex(uint64_t V, int Digits)
    {
        char B[24];
        std::snprintf(B, sizeof(B), "0x%0*llx", Digits, (unsigned long long)V);
        return B;
    }

    // a float as JSON: a NaN or infinity (an uninitialised field) as null
    Json Num(float F) { return std::isfinite(F) ? Json(F) : Json(nullptr); }

    struct Reader
    {
        const Treyarch::EffectZone& Z;
        const Layout& L;
        Counts& C;
        const std::function<std::string(uint64_t)>& Outside;

        const std::vector<uint8_t>& Image() const { return *Z.Image; }
        bool Holds(int64_t O, int64_t N) const { return O >= 0 && N >= 0 && (uint64_t)(O + N) <= Image().size(); }
        uint64_t Ptr(int64_t O) const { return Holds(O, 8) ? At<uint64_t>(Image(), O) : 0; }
        template <class T> T Get(int64_t O) const { return Holds(O, sizeof(T)) ? At<T>(Image(), O) : T{}; }

        // an asset's name through its header (its first field the name pointer; "," a reference)
        std::string Asset(uint64_t Address) const
        {
            if (Z.Off(Address) < 0) return Address && Outside ? Outside(Address) : std::string();
            return Z.Asset(Address);
        }

        // ScaleAt: where its scale is (-1: the curve's +0xc, -2: none, 1)
        Json Curve(int64_t O, int64_t ScaleAt = -1) const
        {
            const uint64_t Keys = Ptr(O);
            const int32_t N = Get<int32_t>(O + 8);
            Json J{ { "scale", ScaleAt == -2 ? Json(1.0f) : Num(Get<float>(ScaleAt >= 0 ? ScaleAt : O + 12)) } };
            Json K = Json::array();
            const int64_t At = Z.Off(Keys);
            if (N > 0 && N <= MaxKeys && Holds(At, (int64_t)N * 16))
                for (int32_t I = 0; I < N; I++) K.push_back(Json::array({ Num(Get<float>(At + I * 16)), Num(Get<float>(At + I * 16 + 4)) }));
            J["keys"] = std::move(K);
            return J;
        }

        Json Value(int64_t O, FieldType T) const
        {
            switch (T)
            {
            case FieldType::U8: return Get<uint8_t>(O);
            case FieldType::Bool: return Get<uint8_t>(O) != 0;
            case FieldType::U16: return Get<uint16_t>(O);
            case FieldType::U32: return Get<uint32_t>(O);
            case FieldType::I32: return Get<int32_t>(O);
            case FieldType::F32: return Num(Get<float>(O));
            case FieldType::Hex32: return Hex(Get<uint32_t>(O), 8);
            case FieldType::Hex64: return Hex(Get<uint64_t>(O), 16);
            case FieldType::Range: return Json::array({ Num(Get<float>(O)), Num(Get<float>(O + 4)) });
            case FieldType::IntRange: return Json::array({ Get<int32_t>(O), Get<int32_t>(O + 4) });
            case FieldType::Vec4:
                return Json::array({ Num(Get<float>(O)), Num(Get<float>(O + 4)), Num(Get<float>(O + 8)), Num(Get<float>(O + 12)) });
            case FieldType::Curve: return Curve(O);
            case FieldType::CurveArray: break;   // CurvesOf
            }
            return nullptr;
        }
        static int64_t Width(FieldType T)
        {
            switch (T)
            {
            case FieldType::U8: case FieldType::Bool: return 1;
            case FieldType::U16: return 2;
            case FieldType::Hex64: case FieldType::Range: case FieldType::IntRange: return 8;
            case FieldType::Vec4: case FieldType::Curve: return 16;
            default: return 4;
            }
        }
        // a field's curves (Curve: Count in place; CurveArray: Count behind a pointer, scales apart)
        void CurvesOf(int64_t Base, const Field& F, Json& Out) const
        {
            if (F.Type == FieldType::Curve)
                for (int I = 0; I < F.Count; I++) Out.push_back(Curve(Base + F.Offset + I * 16));
            if (F.Type != FieldType::CurveArray) return;
            const int64_t Arr = Z.Off(Ptr(Base + F.Offset));
            for (int I = 0; Arr >= 0 && I < F.Count; I++)
                Out.push_back(Curve(Arr + I * 16, F.Scale >= 0 ? Base + F.Scale + I * 4 : -2));
        }
        void Fields(Json& Out, int64_t Base, const std::vector<Field>& Fs) const
        {
            for (const Field& Fd : Fs)
            {
                Field F = Fd;
                if (F.Via >= 0)
                {
                    // through the pointer the parent holds there
                    const int64_t To = Z.Off(Ptr(Base + F.Via));
                    if (To < 0)
                    {
                        Out[F.Name] = nullptr;
                        continue;
                    }
                    F.Offset += To - Base;
                }
                if (F.Type == FieldType::CurveArray)
                {
                    Json A = Json::array();
                    CurvesOf(Base, F, A);
                    Out[F.Name] = std::move(A);
                    continue;
                }
                if (F.Count <= 1)
                {
                    Out[F.Name] = Value(Base + F.Offset, F.Type);
                    continue;
                }
                Json A = Json::array();
                for (int I = 0; I < F.Count; I++) A.push_back(Value(Base + F.Offset + I * Width(F.Type), F.Type));
                Out[F.Name] = std::move(A);
            }
        }

        // one linked asset entry, by what the module's list holds
        Json Linked(AssetKind K, int64_t E) const
        {
            const auto Material = [&](uint64_t A) {
                Json V;
                const std::string N = Asset(A);
                if (N.empty()) return V;
                V["material"] = N;
                // a name-only header (",name"): defined in another zone, its atlas and techset there
                const int64_t O = Z.Off(A);
                const std::string Raw = O >= 0 ? Z.String(Ptr(O)) : std::string();
                const bool Reference = !Raw.empty() && Raw[0] == ',';
                if (!Reference && O >= 0 && L.MaterialAtlasRows >= 0 && L.MaterialAtlasCols >= 0)
                    V["atlas"] = Json::array({ Get<uint8_t>(O + L.MaterialAtlasRows), Get<uint8_t>(O + L.MaterialAtlasCols) });
                // defined in another zone (a name-only header here): pulled from there
                Treyarch::EffectZone::MaterialInfo M;
                if (Z.AddMaterial(V, N, A, Reference, &M))
                    for (const auto& I : M.Images)
                        if (!I.Name.empty() && I.Name[0] != '$') C.Images.insert(I.Name);
                return V;
            };
            switch (K)
            {
            case AssetKind::Material: return Material(Ptr(E));
            case AssetKind::Decal:
            {
                // three materials (ParticleMarkVisuals)
                Json A = Json::array();
                for (int I = 0; I < 3; I++)
                    if (Json V = Material(Ptr(E + 8 * I)); !V.is_null()) A.push_back(std::move(V));
                return Json{ { "decal", std::move(A) } };
            }
            case AssetKind::Sound: return Json{ { "sound", Z.String(Ptr(E)) } };
            case AssetKind::Physics: return Json{ { "physicsPipeline", Asset(Ptr(E)) }, { "physicsShape", Asset(Ptr(E + 8)) } };
            default: break;
            }
            const char* Key = K == AssetKind::Model ? "model" : K == AssetKind::Effect ? "effect" : K == AssetKind::VectorField ? "vectorField"
                            : K == AssetKind::LightDef ? "lightDef" : K == AssetKind::ParticleSim ? "particleSim" : "asset";
            return Json{ { Key, Asset(Ptr(E)) } };
        }

        Json ModuleJson(int64_t M) const
        {
            const int Id = Get<uint16_t>(M + L.ModuleType);
            int64_t D = M + L.ModuleData;
            if (L.ModuleDataPointer) D = Z.Off(Ptr(M + L.ModuleData));
            const Module* T = L.Find(Id);
            Json J{ { "module", T ? T->Name : "unknown" }, { "id", Id } };
            C.Modules++;
            if (T == nullptr || D < 0)
            {
                C.Unknown++;
                C.UnknownIds[Id]++;
                // its bytes as they are, for a later reading
                const int64_t N = L.ModuleDataPointer ? 0x40 : L.ModuleSize - L.ModuleData;
                std::string Raw;
                for (int64_t I = 0; D >= 0 && I < N && Holds(D + I, 1); I++)
                {
                    char B[4];
                    std::snprintf(B, sizeof(B), "%02x", Image()[(size_t)(D + I)]);
                    Raw += B;
                }
                J["raw"] = Raw;
                return J;
            }
            J["flags"] = Hex(Get<uint32_t>(D + L.ModuleFlags), 8);
            // the module's curves gathered, in field order
            Json Curves = Json::array();
            std::vector<Field> Plain;
            for (const Field& F : T->Fields)
                if (F.Type == FieldType::Curve || F.Type == FieldType::CurveArray) CurvesOf(D, F, Curves);
                else Plain.push_back(F);
            Fields(J, D, Plain);
            if (!Curves.empty()) J["curves"] = std::move(Curves);
            if (T->AssetList >= 0)
            {
                Json A = Json::array();
                const int64_t List = Z.Off(Ptr(D + T->AssetList));
                const int32_t N = Get<int32_t>(D + T->AssetList + 8);
                for (int32_t I = 0; List >= 0 && N > 0 && N < MaxCount && I < N && Holds(List + I * L.AssetSize, L.AssetSize); I++)
                    if (Json V = Linked(T->Assets, List + I * L.AssetSize); !V.is_null()) A.push_back(std::move(V));
                J["assets"] = std::move(A);
            }
            return J;
        }

        bool State(int64_t S, Json& Out) const
        {
            const uint32_t Type = Get<uint32_t>(S + L.StateElementType);
            // an id the game does not name: the number itself
            const bool Named = Type < L.ElementTypes.size() && L.ElementTypes[Type] && *L.ElementTypes[Type];
            Out["elementType"] = Named ? Json(L.ElementTypes[Type]) : Json(Type);
            Fields(Out, S, L.StateFields);
            const int64_t Groups = Z.Off(Ptr(S + L.StateGroups));
            if (Groups < 0) return false;
            for (int G = 0; G < L.GroupCount && G < 3; G++)
            {
                const int64_t Gp = Groups + G * L.GroupSize;
                const int32_t N = Get<int32_t>(Gp + L.GroupModuleCount);
                Json Mods = Json::array();
                const int64_t Arr = Z.Off(Ptr(Gp + L.GroupModules));
                if (N < 0 || N > MaxCount || (N > 0 && !Holds(Arr, (int64_t)N * L.ModuleSize))) return false;
                for (int32_t I = 0; I < N; I++) Mods.push_back(ModuleJson(Arr + I * L.ModuleSize));
                Out[GroupNames[G]] = std::move(Mods);
            }
            C.States++;
            return true;
        }
    };
}

const Module* Layout::Find(int Id) const
{
    for (const Module& M : Modules)
        if (M.Id == Id) return &M;
    return nullptr;
}

void AddAtlas(nlohmann::ordered_json& J, const Layout& L, const std::vector<uint8_t>& Image, int64_t O, int64_t Size)
{
    if (O >= 0 && (uint64_t)O + Size <= Image.size())
        J["atlas"] = { At<uint8_t>(Image, O + L.MaterialAtlasRows), At<uint8_t>(Image, O + L.MaterialAtlasCols) };
}

bool ReadSystem(const Treyarch::EffectZone& Z, const Layout& L, int64_t Pos, const std::string& Name, std::string& Out, Counts* Count,
                const std::function<std::string(uint64_t)>& Outside)
{
    Counts Local;
    Reader R{ Z, L, Count ? *Count : Local, Outside };
    Json J{ { "format", "vfx" }, { "game", L.Game }, { "name", Name } };
    R.Fields(J, Pos, L.SystemFields);
    const int32_t N = R.Get<int32_t>(Pos + L.SystemEmitterCount);
    const int64_t Emitters = Z.Off(R.Ptr(Pos + L.SystemEmitters));
    if (N < 0 || N > MaxCount || (N > 0 && !R.Holds(Emitters, (int64_t)N * L.EmitterSize))) return false;
    Json Es = Json::array();
    for (int32_t I = 0; I < N; I++)
    {
        const int64_t E = Emitters + I * L.EmitterSize;
        Json Ej;
        R.Fields(Ej, E, L.EmitterFields);
        const int32_t Ns = R.Get<int32_t>(E + L.EmitterStateCount);
        const int64_t States = Z.Off(R.Ptr(E + L.EmitterStates));
        if (Ns < 0 || Ns > MaxCount || (Ns > 0 && !R.Holds(States, (int64_t)Ns * L.StateSize))) return false;
        Json Ss = Json::array();
        for (int32_t S = 0; S < Ns; S++)
        {
            Json Sj;
            if (!R.State(States + S * L.StateSize, Sj)) return false;
            Ss.push_back(std::move(Sj));
        }
        Ej["states"] = std::move(Ss);
        Es.push_back(std::move(Ej));
        R.C.Emitters++;
    }
    J["emitters"] = std::move(Es);
    Out = J.dump(1, '\t', false, Json::error_handler_t::replace) + "\n";
    return true;
}
}
