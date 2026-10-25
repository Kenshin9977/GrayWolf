#include "stdafx.h"

#include "MaterialDescription.h"
#include "ExportNaming.h"     // Lower
#include "ZoneTableWalker.h"  // LoadProfile (shading.txt)
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace Materials
{
namespace
{
    std::map<std::string, const IAdapter*>& Registry()
    {
        static std::map<std::string, const IAdapter*> R;
        return R;
    }

    // The stem without a "-<hash>" tail or an extension, lower case.
    std::string Stem(const std::string& FileName)
    {
        std::string S = ExportNaming::Lower(std::filesystem::path(FileName).stem().string());
        const size_t Dash = S.rfind('-');
        if (Dash != std::string::npos && Dash > S.rfind('_')) S.resize(Dash);
        return S;
    }

    bool EndsWith(const std::string& S, const std::string& Tail)
    {
        return S.size() > Tail.size() && S.compare(S.size() - Tail.size(), Tail.size(), Tail) == 0;
    }

    // Suffix heuristics: good for older exports and games without an adapter.
    class DefaultAdapterImpl final : public IAdapter
    {
    public:
        const char* Name() const override { return "default"; }
        Description Describe(const Source& M) const override
        {
            Description D;
            D.Name = M.Name;
            for (const auto& Img : M.Images)
            {
                Role R = RoleFromName(Img.Path);
                if (R == Role::Unknown) R = Img.Hint;
                switch (R)
                {
                case Role::Albedo:    if (D.Albedo.Empty()) D.Albedo.Path = Img.Path; break;
                case Role::Normal:    if (D.Normal.Empty()) D.Normal.Path = Img.Path; break;
                case Role::Gloss:     if (D.Roughness.Empty()) D.Roughness = { Img.Path, 0, true }; break;
                case Role::Roughness: D.Roughness = { Img.Path, 0, false }; break;
                case Role::Specular:  if (D.Specular.Empty()) D.Specular.Path = Img.Path; break;
                case Role::Metalness: if (D.Metalness.Empty()) D.Metalness.Path = Img.Path; break;
                case Role::Occlusion: if (D.Occlusion.Empty()) D.Occlusion.Path = Img.Path; break;
                case Role::Emissive:  if (D.Emissive.Empty()) D.Emissive.Path = Img.Path; break;
                default: break;
                }
            }
            D.Alpha = LooksAlphaTested(M.Name) ? AlphaMode::Cutout : AlphaMode::Auto;
            return D;
        }
    };

    // colorMap, normalMap, specularMap (rgb the reflection colour, alpha the gloss). A white map
    // ("$white-rgb") is common: read as F0 it would be a mirror and take all the colour, so the
    // reflection is scaled to a dielectric-like strength and the colour kept.
    class LegacySpecularAdapterImpl final : public IAdapter
    {
    public:
        const char* Name() const override { return "legacy-specular"; }
        Description Describe(const Source& M) const override
        {
            Description D = DescribeBySemantic(M, {
                { 0xA0AB1041, Role::Albedo },
                { 0x59D30D0F, Role::Normal },
                { 0x34ECCCB3, Role::Specular },
                { 0x34ECCCB3, Role::Gloss, 3 },
            });
            // an exporter without semantics: the readers' hints and the names
            if (D.Albedo.Empty()) D = DefaultAdapter().Describe(M);
            if (!D.Specular.Empty() && D.Metalness.Empty())
            {
                D.LegacySpecular = true;
                D.SpecularScale = 0.2f;
                if (D.Roughness.Empty()) D.Roughness = { D.Specular.Path, 3, true };
            }
            return D;
        }
    };

    const char* AlphaName(AlphaMode A)
    {
        switch (A)
        {
        case AlphaMode::Cutout: return "cutout";
        case AlphaMode::Blend:  return "blend";
        case AlphaMode::Auto:   return "auto";
        default:                return "opaque";
        }
    }
    AlphaMode AlphaFromName(const std::string& S)
    {
        if (S == "cutout") return AlphaMode::Cutout;
        if (S == "blend") return AlphaMode::Blend;
        if (S == "auto") return AlphaMode::Auto;
        return AlphaMode::Opaque;
    }
    const char* ChannelNames = "rgba";
    double Rounded(float V) { return std::round((double)V * 1000.0) / 1000.0; }

    // Paths are written with forward slashes, the model file's doubled backslashes collapsed.
    std::string PortablePath(const std::string& P)
    {
        std::string Out;
        for (char C : P)
        {
            if (C == '\\') C = '/';
            if (C == '/' && !Out.empty() && Out.back() == '/') continue;
            Out.push_back(C);
        }
        return Out;
    }

    void PutMap(nlohmann::json& J, const char* Key, const Map& M, bool Channel)
    {
        if (M.Empty()) return;
        nlohmann::json O{ { "path", PortablePath(M.Path) } };
        if (Channel) O["channel"] = std::string(1, ChannelNames[M.Channel & 3]);
        if (M.Invert) O["invert"] = true;
        J[Key] = O;
    }
    void GetMap(const nlohmann::json& J, const char* Key, Map& M)
    {
        if (!J.contains(Key) || !J[Key].is_object()) return;
        const auto& O = J[Key];
        M.Path = O.value("path", std::string());
        const std::string C = O.value("channel", std::string("r"));
        const char* At = std::strchr(ChannelNames, C.empty() ? 'r' : C[0]);
        M.Channel = At ? (int)(At - ChannelNames) : 0;
        M.Invert = O.value("invert", false);
    }

    nlohmann::json Floats(const float* V, int N)
    {
        nlohmann::json A = nlohmann::json::array();
        for (int i = 0; i < N; i++) A.push_back(Rounded(V[i]));
        return A;
    }
    void GetFloats(const nlohmann::json& J, const char* Key, float* V, int N)
    {
        if (!J.contains(Key) || !J[Key].is_array()) return;
        for (int i = 0; i < N && i < (int)J[Key].size(); i++) V[i] = J[Key][i].get<float>();
    }

    nlohmann::json TintJson(const Gradient& G)
    {
        nlohmann::json T{ { "path", PortablePath(G.Image.Path) },
                          { "channel", G.Rgb ? std::string("rgb") : std::string(1, ChannelNames[G.Image.Channel & 3]) },
                          { "u", Rounded(G.U) } };
        if (!G.Offset.Empty())
            T["offset"] = { { "path", PortablePath(G.Offset.Path) }, { "channel", std::string(1, ChannelNames[G.Offset.Channel & 3]) },
                            { "scale", Rounded(G.OffsetScale) } };
        if (!G.Rgb) T["color"] = Floats(G.Color, 3);
        return T;
    }
    void TintFromJson(const nlohmann::json& J, Gradient& G)
    {
        G.Image.Path = J.value("path", std::string());
        const std::string C = J.value("channel", std::string("rgb"));
        G.Rgb = C == "rgb";
        const char* At = G.Rgb || C.empty() ? nullptr : std::strchr(ChannelNames, C[0]);
        G.Image.Channel = At ? (int)(At - ChannelNames) : 0;
        G.U = J.value("u", 0.5f);
        if (J.contains("offset"))
        {
            GetMap(J, "offset", G.Offset);
            G.OffsetScale = J["offset"].value("scale", 0.0f);
        }
        GetFloats(J, "color", G.Color, 3);
    }

    nlohmann::json LayerJson(const Layer& L)
    {
        nlohmann::json J = nlohmann::json::object();
        PutMap(J, "albedo", L.Albedo, false);
        PutMap(J, "normal", L.Normal, false);
        if (!L.Normal.Empty()) J["normal_scale"] = Rounded(L.NormalScale);
        PutMap(J, "roughness", L.Roughness, true);
        if (L.RoughnessValue >= 0) J["roughness_value"] = Rounded(L.RoughnessValue);
        J["uv_scale"] = Floats(L.UVScale, 2);
        if (L.UVRotation != 0) J["uv_rotation"] = Rounded(L.UVRotation);
        J["tint"] = Floats(L.Tint, 3);
        if (!L.Mask.Empty())
        {
            PutMap(J, "mask", L.Mask, true);
            J["mask"]["uv_scale"] = Floats(L.MaskUVScale, 2);
        }
        return J;
    }
    Layer LayerFromJson(const nlohmann::json& J)
    {
        Layer L;
        GetMap(J, "albedo", L.Albedo);
        GetMap(J, "normal", L.Normal);
        L.NormalScale = J.value("normal_scale", 1.0f);
        GetMap(J, "roughness", L.Roughness);
        L.RoughnessValue = J.value("roughness_value", -1.0f);
        GetFloats(J, "uv_scale", L.UVScale, 2);
        L.UVRotation = J.value("uv_rotation", 0.0f);
        GetFloats(J, "tint", L.Tint, 3);
        GetMap(J, "mask", L.Mask);
        if (J.contains("mask")) GetFloats(J["mask"], "uv_scale", L.MaskUVScale, 2);
        return L;
    }
}

const Source::Image* Source::BySemantic(uint32_t Semantic) const
{
    for (const auto& I : Images)
        if (I.Semantic == Semantic) return &I;
    return nullptr;
}

const Recipe* ShadingRecipe(const std::string& Profile, const std::string& Techset)
{
    static std::mutex Lock;
    static std::map<std::string, std::map<std::string, Recipe>> Tables;
    std::lock_guard<std::mutex> Guard(Lock);
    auto It = Tables.find(Profile);
    if (It == Tables.end())
    {
        auto& Rows = Tables[Profile];
        ZoneTables::Profile P;
        std::string Error;
        if (ZoneTables::LoadProfile(Profile, P, Error))
        {
            std::ifstream In(std::filesystem::path(P.Dir) / "shading.txt");
            std::string Line;
            while (std::getline(In, Line))
            {
                if (Line.empty() || Line[0] == '#') continue;
                std::stringstream Fields(Line);
                std::string Key, Part;
                std::getline(Fields, Key, '|');
                Recipe R;
                if (std::getline(Fields, Part, '|')) R["recipe"] = Part;
                while (std::getline(Fields, Part, '|'))
                    if (const size_t Eq = Part.find('='); Eq != std::string::npos) R[Part.substr(0, Eq)] = Part.substr(Eq + 1);
                Rows[Key] = std::move(R);
            }
        }
        It = Tables.find(Profile);
    }
    const auto Row = It->second.find(Techset);
    return Row == It->second.end() ? nullptr : &Row->second;
}

float RecipeFloat(const Recipe& R, const std::string& Field, const std::vector<uint8_t>& Block, int Component, float Default)
{
    const auto It = R.find(Field);
    if (It == R.end()) return Default;
    const size_t At = (size_t)std::stoull(It->second, nullptr, 0) + 4 * (size_t)Component;
    if (At + 4 > Block.size()) return Default;
    float V;
    std::memcpy(&V, Block.data() + At, 4);
    return std::isfinite(V) ? V : Default;
}

AdapterRegistration::AdapterRegistration(const char* Engine, const IAdapter& Adapter)
{
    Registry()[Engine] = &Adapter;
}

const IAdapter& DefaultAdapter()
{
    static const DefaultAdapterImpl Default;
    return Default;
}

const IAdapter& LegacySpecularAdapter()
{
    static const LegacySpecularAdapterImpl Legacy;
    return Legacy;
}

const IAdapter& AdapterFor(const std::string& Engine)
{
    const auto It = Registry().find(Engine);
    return It != Registry().end() ? *It->second : DefaultAdapter();
}

Role RoleFromName(const std::string& FileName)
{
    const std::string S = Stem(FileName);
    static const std::pair<const char*, Role> Suffixes[] = {
        { "_c", Role::Albedo }, { "_col", Role::Albedo }, { "_color", Role::Albedo }, { "_colour", Role::Albedo },
        { "_d", Role::Albedo }, { "_diffuse", Role::Albedo }, { "_albedo", Role::Albedo }, { "_alb", Role::Albedo },
        { "_n", Role::Normal }, { "_nml", Role::Normal }, { "_nrm", Role::Normal }, { "_normal", Role::Normal },
        { "_g", Role::Gloss }, { "_gls", Role::Gloss }, { "_gloss", Role::Gloss },
        { "_rough", Role::Roughness }, { "_roughness", Role::Roughness }, { "_rgh", Role::Roughness },
        { "_s", Role::Specular }, { "_spc", Role::Specular }, { "_spec", Role::Specular }, { "_specular", Role::Specular },
        { "_metal", Role::Metalness }, { "_metallic", Role::Metalness }, { "_metalness", Role::Metalness },
        { "_o", Role::Occlusion }, { "_ao", Role::Occlusion }, { "_occ", Role::Occlusion }, { "_occlusion", Role::Occlusion },
        { "_e", Role::Emissive }, { "_emi", Role::Emissive }, { "_emissive", Role::Emissive }, { "_glow", Role::Emissive },
    };
    for (const auto& [Tail, R] : Suffixes)
        if (EndsWith(S, Tail)) return R;
    return Role::Unknown;
}

bool LooksAlphaTested(const std::string& Name)
{
    const std::string S = ExportNaming::Lower(Name);
    static const char* Words[] = { "hair", "beard", "brow", "lash", "fur", "card", "foliage", "leaf",
                                   "leaves", "grass", "plant", "fern", "ivy", "branch", "decal", "alphatest",
                                   "cloud" };
    for (const char* W : Words)
        if (S.find(W) != std::string::npos) return true;
    return false;
}

Description DescribeBySemantic(const Source& M, const std::vector<SemanticRole>& Table)
{
    Description D;
    D.Name = M.Name;
    for (const auto& Row : Table)
        for (const auto& Img : M.Images)
        {
            if (Img.Semantic != Row.Semantic) continue;
            Map* Slot = nullptr;
            switch (Row.Use)
            {
            case Role::Albedo:    Slot = &D.Albedo; break;
            case Role::Normal:    Slot = &D.Normal; break;
            case Role::Gloss:     case Role::Roughness: Slot = &D.Roughness; break;
            case Role::Specular:  Slot = &D.Specular; break;
            case Role::Metalness: Slot = &D.Metalness; break;
            case Role::Occlusion: Slot = &D.Occlusion; break;
            case Role::Emissive:  Slot = &D.Emissive; break;
            case Role::Transmission: case Role::Thickness: Slot = &D.Transmission.Image; break;
            default: break;
            }
            if (Slot && Slot->Empty()) *Slot = { Img.Path, Row.Channel, Row.Use == Role::Gloss || Row.Use == Role::Thickness };
        }
    const bool Tested = LooksAlphaTested(M.Name) || (!D.Albedo.Empty() && LooksAlphaTested(std::filesystem::path(D.Albedo.Path).stem().string()));
    D.Alpha = Tested ? AlphaMode::Cutout : AlphaMode::Auto;
    return D;
}

void BaseColorFactor(const Description& D, float Rgb[3])
{
    for (int c = 0; c < 3; c++) Rgb[c] = D.Color.Empty() ? 1.0f : D.Color.Color[c];
}

std::string SidecarPath(const std::string& ModelPath)
{
    const std::filesystem::path P(ModelPath);
    return (P.parent_path() / (P.stem().string() + ".materials.json")).string();
}

namespace
{
    std::string SidecarText(const Sidecar& S);

    bool FileHolds(const std::string& Path, const std::string& Text)
    {
        std::error_code Ec;
        if (std::filesystem::file_size(Path, Ec) != Text.size() || Ec) return false;
        std::ifstream In(Path, std::ios::binary);
        std::string Old(Text.size(), '\0');
        return In.read(Old.data(), (std::streamsize)Old.size()) && Old == Text;
    }
}

bool SidecarHolds(const std::string& Path, const Sidecar& S)
{
    return FileHolds(Path, SidecarText(S));
}

bool WriteSidecar(const std::string& Path, const Sidecar& S)
{
    const std::string Text = SidecarText(S);
    if (FileHolds(Path, Text)) return true;   // already those bytes: not rewritten
    std::ofstream Out(Path, std::ios::binary | std::ios::trunc);
    if (!Out) return false;
    Out << Text;
    return (bool)Out;
}

namespace
{
std::string SidecarText(const Sidecar& S)
{
    nlohmann::json Mats = nlohmann::json::array();
    for (size_t i = 0; i < S.Materials.size(); i++)
    {
        const Description& D = S.Materials[i];
        nlohmann::json J{ { "name", D.Name }, { "alpha", AlphaName(D.Alpha) } };
        if (D.Alpha == AlphaMode::Cutout || D.Alpha == AlphaMode::Auto) J["alpha_cutoff"] = Rounded(D.AlphaCutoff);
        PutMap(J, "albedo", D.Albedo, false);
        PutMap(J, "normal", D.Normal, false);
        if (!D.Normal.Empty())
        {
            J["normal"]["encoding"] = D.NormalEnc == NormalEncoding::AG ? "ag" : D.NormalEnc == NormalEncoding::XYZ ? "xyz" : "xy";
            if (D.NormalFlipY) J["normal"]["flip_y"] = true;
        }
        PutMap(J, "roughness", D.Roughness, true);
        PutMap(J, "metalness", D.Metalness, true);
        PutMap(J, "specular", D.Specular, false);
        if (!D.Specular.Empty() && D.LegacySpecular)
        {
            J["specular"]["legacy"] = true;
            J["specular"]["scale"] = Rounded(D.SpecularScale);
        }
        PutMap(J, "occlusion", D.Occlusion, true);
        PutMap(J, "emissive", D.Emissive, false);
        if (!D.Emissive.Empty() && (D.EmissiveColor[0] != 1.0f || D.EmissiveColor[1] != 1.0f || D.EmissiveColor[2] != 1.0f))
            J["emissive"]["color"] = Floats(D.EmissiveColor, 3);
        if (!D.Transmission.Empty())
        {
            PutMap(J, "transmission", D.Transmission.Image, true);
            J["transmission"]["color"] = Floats(D.Transmission.Color, 3);
        }
        if (!D.AlphaMap.Empty())
        {
            PutMap(J, "alpha_map", D.AlphaMap.Image, true);
            J["alpha_map"]["low"] = Rounded(D.AlphaMap.Low);
            J["alpha_map"]["high"] = Rounded(D.AlphaMap.High);
            J["alpha_map"]["uv_scale"] = Floats(D.AlphaMap.UVScale, 2);
        }
        if (D.Roughness.Empty()) J["roughness_value"] = Rounded(D.RoughnessValue);
        if (!D.Color.Empty())
        {
            nlohmann::json C = nlohmann::json::object();
            PutMap(C, "mask", D.Color.Mask, true);
            C["color"] = Floats(D.Color.Color, 3);
            J["color_tint"] = C;
        }
        if (!D.Tint.Empty()) J["tint"] = TintJson(D.Tint);
        if (!D.Layers.empty())
        {
            nlohmann::json Ls = nlohmann::json::array();
            for (const Layer& L : D.Layers) Ls.push_back(LayerJson(L));
            J["layers"] = Ls;
        }
        if (D.Metalness.Empty() && D.Specular.Empty()) J["metalness_value"] = Rounded(D.MetalnessValue);
        if (i < S.Sources.size())
        {
            const Source& Src = S.Sources[i];
            if (!Src.Techset.empty()) J["techset"] = Src.Techset;
            if (Src.SortKey >= 0) J["sort_key"] = Src.SortKey;
            nlohmann::json Imgs = nlohmann::json::array();
            char Hex[16];
            for (const auto& Img : Src.Images)
            {
                std::snprintf(Hex, sizeof(Hex), "0x%08X", Img.Semantic);
                Imgs.push_back({ { "path", PortablePath(Img.Path) }, { "semantic", Hex } });
            }
            J["sources"] = Imgs;
        }
        Mats.push_back(J);
    }
    const nlohmann::json Root{ { "format", "greyhound-materials" }, { "version", 1 },
                               { "game", S.Game }, { "adapter", S.Adapter }, { "materials", Mats } };
    // names are raw game strings: bytes that are not UTF-8 become U+FFFD instead of throwing
    return Root.dump(1, '\t', false, nlohmann::json::error_handler_t::replace);
}
}

bool ReadSidecar(const std::string& Path, Sidecar& S)
{
    std::ifstream In(Path, std::ios::binary);
    if (!In) return false;
    try
    {
        const nlohmann::json Root = nlohmann::json::parse(In);
        if (Root.value("format", std::string()) != "greyhound-materials") return false;
        S.Game = Root.value("game", std::string());
        S.Adapter = Root.value("adapter", std::string());
        for (const auto& J : Root.value("materials", nlohmann::json::array()))
        {
            Description D;
            D.Name = J.value("name", std::string());
            D.Alpha = AlphaFromName(J.value("alpha", std::string("opaque")));
            D.AlphaCutoff = J.value("alpha_cutoff", 0.5f);
            GetMap(J, "albedo", D.Albedo);
            GetMap(J, "normal", D.Normal);
            if (J.contains("normal"))
            {
                const std::string E = J["normal"].value("encoding", std::string("xy"));
                D.NormalEnc = E == "ag" ? NormalEncoding::AG : E == "xyz" ? NormalEncoding::XYZ : NormalEncoding::XY;
                D.NormalFlipY = J["normal"].value("flip_y", false);
            }
            GetMap(J, "roughness", D.Roughness);
            GetMap(J, "metalness", D.Metalness);
            GetMap(J, "specular", D.Specular);
            if (J.contains("specular") && J["specular"].is_object())
            {
                D.LegacySpecular = J["specular"].value("legacy", false);
                D.SpecularScale = J["specular"].value("scale", 1.0f);
            }
            GetMap(J, "occlusion", D.Occlusion);
            GetMap(J, "emissive", D.Emissive);
            if (J.contains("emissive") && J["emissive"].is_object()) GetFloats(J["emissive"], "color", D.EmissiveColor, 3);
            GetMap(J, "transmission", D.Transmission.Image);
            if (J.contains("transmission") && J["transmission"].is_object()) GetFloats(J["transmission"], "color", D.Transmission.Color, 3);
            GetMap(J, "alpha_map", D.AlphaMap.Image);
            if (J.contains("alpha_map") && J["alpha_map"].is_object())
            {
                D.AlphaMap.Low = J["alpha_map"].value("low", 0.0f);
                D.AlphaMap.High = J["alpha_map"].value("high", 1.0f);
                GetFloats(J["alpha_map"], "uv_scale", D.AlphaMap.UVScale, 2);
            }
            D.RoughnessValue = J.value("roughness_value", D.RoughnessValue);
            D.MetalnessValue = J.value("metalness_value", D.MetalnessValue);
            if (J.contains("tint") && J["tint"].is_object()) TintFromJson(J["tint"], D.Tint);
            if (J.contains("color_tint") && J["color_tint"].is_object())
            {
                GetMap(J["color_tint"], "mask", D.Color.Mask);
                GetFloats(J["color_tint"], "color", D.Color.Color, 3);
            }
            for (const auto& L : J.value("layers", nlohmann::json::array())) D.Layers.push_back(LayerFromJson(L));
            S.Materials.push_back(std::move(D));
        }
        return true;
    }
    catch (...)
    {
        return false;
    }
}
}
