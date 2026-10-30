#include "stdafx.h"

// DXIL reflection (DxilReflection.h): DXC's container reflection and disassembly, else the
// container's own PSV0 / ISG1 / OSG1 parts.

#include "DxilReflection.h"

#include <d3d12.h>
#include <d3d12shader.h>
#include <wrl/client.h>
#include "dxc/Support/dxcapi.use.h"
#include "dxc/DxilContainer/DxilContainer.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <string_view>
#include <unordered_map>

#pragma comment(lib, "dxguid.lib")

namespace DxilReflection
{
    namespace
    {
        namespace fs = std::filesystem;
        using Json = nlohmann::ordered_json;
        using Microsoft::WRL::ComPtr;

        template <class T>
        T At(const uint8_t* B, size_t Size, size_t Pos)
        {
            T V{};
            if (Pos + sizeof(T) <= Size) std::memcpy(&V, B + Pos, sizeof(T));
            return V;
        }

        // ---- dxcompiler.dll ------------------------------------------------------------------

        struct Dll
        {
            dxc::DxcDllSupport Support;
            std::string From;
            bool Ok = false;
        };
        // Beside GrayWolf.exe, else the newest Windows 10/11 SDK's x64 one (every Visual Studio
        // install with the C++ workload has one), else the DLL search path; loaded once
        Dll& Dxc()
        {
            static Dll D;
            static std::once_flag Once;
            std::call_once(Once, [] {
                std::vector<fs::path> Candidates;
                wchar_t Exe[MAX_PATH] = {};
                if (GetModuleFileNameW(nullptr, Exe, MAX_PATH) != 0) Candidates.push_back(fs::path(Exe).parent_path() / L"dxcompiler.dll");
                std::error_code Ec;
                std::vector<fs::path> Sdks;
                for (const wchar_t* Root : { L"C:\\Program Files (x86)\\Windows Kits\\10\\bin", L"C:\\Program Files\\Windows Kits\\10\\bin" })
                    for (const auto& E : fs::directory_iterator(Root, Ec))
                        if (fs::exists(E.path() / L"x64" / L"dxcompiler.dll", Ec)) Sdks.push_back(E.path() / L"x64" / L"dxcompiler.dll");
                // newest SDK first: by its version folder
                std::sort(Sdks.begin(), Sdks.end(), [](const fs::path& A, const fs::path& B) {
                    return A.parent_path().parent_path().filename().wstring() > B.parent_path().parent_path().filename().wstring();
                });
                Candidates.insert(Candidates.end(), Sdks.begin(), Sdks.end());
                for (const auto& C : Candidates)
                    if (fs::exists(C, Ec) && SUCCEEDED(D.Support.InitializeForDll(C.wstring().c_str(), "DxcCreateInstance")))
                    {
                        D.Ok = true, D.From = C.string();
                        return;
                    }
                if (SUCCEEDED(D.Support.Initialize())) D.Ok = true, D.From = "dxcompiler.dll";
            });
            return D;
        }

        // ---- the container's parts -----------------------------------------------------------

        constexpr uint32_t FourCC(const char (&S)[5]) { return (uint32_t)S[0] | (uint32_t)S[1] << 8 | (uint32_t)S[2] << 16 | (uint32_t)S[3] << 24; }

        // fourcc -> (offset, size) of each part
        std::map<uint32_t, std::pair<size_t, size_t>> Parts(const uint8_t* B, size_t Size)
        {
            std::map<uint32_t, std::pair<size_t, size_t>> Out;
            if (Size < 0x20 || std::memcmp(B, "DXBC", 4) != 0) return Out;
            const uint32_t Count = At<uint32_t>(B, Size, 0x1c);
            for (uint32_t K = 0; K < Count && 0x20 + (size_t)K * 4 + 4 <= Size; K++)
            {
                const uint32_t Off = At<uint32_t>(B, Size, 0x20 + (size_t)K * 4);
                if ((size_t)Off + 8 > Size) continue;
                const uint32_t N = At<uint32_t>(B, Size, Off + 4);
                if ((size_t)Off + 8 + N > Size) continue;
                Out[At<uint32_t>(B, Size, Off)] = { (size_t)Off + 8, (size_t)N };
            }
            return Out;
        }

        // ISG1 / OSG1: u32 count, u32 offset, 32-byte elements
        std::vector<Parameter> SignaturePart(const uint8_t* B, size_t Size, uint32_t Part)
        {
            std::vector<Parameter> Out;
            const auto P = Parts(B, Size);
            const auto It = P.find(Part);
            if (It == P.end()) return Out;
            const size_t Base = It->second.first, End = Base + It->second.second;
            const uint32_t Count = At<uint32_t>(B, Size, Base), First = At<uint32_t>(B, Size, Base + 4);
            static const char* const Types[] = { "unknown", "uint", "int", "float" };
            for (uint32_t K = 0; K < Count; K++)
            {
                const size_t E = Base + First + (size_t)K * 32;
                if (E + 32 > End) break;
                Parameter Q;
                for (size_t C = Base + At<uint32_t>(B, Size, E + 4); C < End && B[C]; C++) Q.Semantic.push_back((char)B[C]);
                Q.Index = At<uint32_t>(B, Size, E + 8);
                Q.SystemValue = At<uint32_t>(B, Size, E + 12);
                const uint32_t Comp = At<uint32_t>(B, Size, E + 16);
                Q.Component = Comp < 4 ? Types[Comp] : std::to_string(Comp);
                Q.Register = At<uint32_t>(B, Size, E + 20);
                Q.Mask = B[E + 24], Q.Used = B[E + 25];
                Out.push_back(Q);
            }
            return Out;
        }

        // PSV0's resource bindings {type, space, lower, upper}
        std::vector<Resource> PsvResources(const uint8_t* B, size_t Size)
        {
            std::vector<Resource> Out;
            const auto P = Parts(B, Size);
            const auto It = P.find(FourCC("PSV0"));
            if (It == P.end()) return Out;
            size_t O = It->second.first;
            const size_t End = O + It->second.second;
            O += 4 + At<uint32_t>(B, Size, O);
            if (O + 4 > End) return Out;
            const uint32_t Count = At<uint32_t>(B, Size, O);
            O += 4;
            if (Count == 0) return Out;
            const uint32_t Stride = At<uint32_t>(B, Size, O);
            O += 4;
            static const char* const Kinds[] = { "invalid", "sampler", "cbuffer", "texture", "byteaddress", "structured", "uav_typed", "uav_raw",
                                                 "uav_structured", "uav_structured_counter" };
            static const int Classes[] = { 0, 3, 2, 0, 0, 0, 1, 1, 1, 1 };
            for (uint32_t K = 0; K < Count && Stride >= 16 && O + Stride <= End; K++, O += Stride)
            {
                const uint32_t Type = At<uint32_t>(B, Size, O), Lower = At<uint32_t>(B, Size, O + 8), Upper = At<uint32_t>(B, Size, O + 12);
                Resource R;
                R.Kind = Type < std::size(Kinds) ? Kinds[Type] : std::to_string(Type);
                R.Class = Type < std::size(Classes) ? Classes[Type] : 0;
                R.Space = At<uint32_t>(B, Size, O + 4), R.Slot = Lower;
                R.Count = Upper == 0xffffffffu ? 0 : (int32_t)(Upper - Lower + 1);
                Out.push_back(R);
            }
            return Out;
        }

        // ---- the reflection's names for its enums --------------------------------------------

        std::string StageName(UINT Version)
        {
            switch (D3D12_SHVER_GET_TYPE(Version))
            {
            case D3D12_SHVER_PIXEL_SHADER: return "ps";
            case D3D12_SHVER_VERTEX_SHADER: return "vs";
            case D3D12_SHVER_GEOMETRY_SHADER: return "gs";
            case D3D12_SHVER_HULL_SHADER: return "hs";
            case D3D12_SHVER_DOMAIN_SHADER: return "ds";
            case D3D12_SHVER_COMPUTE_SHADER: return "cs";
            case D3D12_SHVER_LIBRARY: return "lib";
            case 0xd: return "ms";
            case 0xe: return "as";
            default: return std::format("type{}", (int)D3D12_SHVER_GET_TYPE(Version));
            }
        }
        const char* KindName(D3D_SHADER_INPUT_TYPE T)
        {
            switch (T)
            {
            case D3D_SIT_CBUFFER: return "cbuffer";
            case D3D_SIT_TBUFFER: return "tbuffer";
            case D3D_SIT_TEXTURE: return "texture";
            case D3D_SIT_SAMPLER: return "sampler";
            case D3D_SIT_UAV_RWTYPED: return "uav_typed";
            case D3D_SIT_STRUCTURED: return "structured";
            case D3D_SIT_UAV_RWSTRUCTURED: return "uav_structured";
            case D3D_SIT_BYTEADDRESS: return "byteaddress";
            case D3D_SIT_UAV_RWBYTEADDRESS: return "uav_raw";
            case D3D_SIT_UAV_APPEND_STRUCTURED: return "uav_append";
            case D3D_SIT_UAV_CONSUME_STRUCTURED: return "uav_consume";
            case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER: return "uav_structured_counter";
            case D3D_SIT_RTACCELERATIONSTRUCTURE: return "accel";
            case D3D_SIT_UAV_FEEDBACKTEXTURE: return "feedback";
            default: return "unknown";
            }
        }
        int ClassOf(D3D_SHADER_INPUT_TYPE T)
        {
            switch (T)
            {
            case D3D_SIT_CBUFFER:
            case D3D_SIT_TBUFFER: return 2;
            case D3D_SIT_SAMPLER: return 3;
            case D3D_SIT_TEXTURE:
            case D3D_SIT_STRUCTURED:
            case D3D_SIT_BYTEADDRESS:
            case D3D_SIT_RTACCELERATIONSTRUCTURE: return 0;
            default: return 1;
            }
        }
        std::string DimensionName(D3D_SRV_DIMENSION D)
        {
            switch (D)
            {
            case D3D_SRV_DIMENSION_BUFFER: return "buffer";
            case D3D_SRV_DIMENSION_TEXTURE1D: return "1d";
            case D3D_SRV_DIMENSION_TEXTURE1DARRAY: return "1darray";
            case D3D_SRV_DIMENSION_TEXTURE2D: return "2d";
            case D3D_SRV_DIMENSION_TEXTURE2DARRAY: return "2darray";
            case D3D_SRV_DIMENSION_TEXTURE2DMS: return "2dms";
            case D3D_SRV_DIMENSION_TEXTURE2DMSARRAY: return "2dmsarray";
            case D3D_SRV_DIMENSION_TEXTURE3D: return "3d";
            case D3D_SRV_DIMENSION_TEXTURECUBE: return "cube";
            case D3D_SRV_DIMENSION_TEXTURECUBEARRAY: return "cubearray";
            case D3D_SRV_DIMENSION_BUFFEREX: return "bufferex";
            default: return "";
            }
        }
        std::string ComponentName(D3D_REGISTER_COMPONENT_TYPE T)
        {
            switch (T)
            {
            case D3D_REGISTER_COMPONENT_UINT32: return "uint";
            case D3D_REGISTER_COMPONENT_SINT32: return "int";
            case D3D_REGISTER_COMPONENT_FLOAT32: return "float";
            case D3D_REGISTER_COMPONENT_UINT16: return "uint16";
            case D3D_REGISTER_COMPONENT_SINT16: return "int16";
            case D3D_REGISTER_COMPONENT_FLOAT16: return "half";
            default: return "unknown";
            }
        }
        std::vector<Parameter> Signature(ID3D12ShaderReflection* R, UINT Count, bool Input)
        {
            std::vector<Parameter> Out;
            for (UINT I = 0; I < Count; I++)
            {
                D3D12_SIGNATURE_PARAMETER_DESC P{};
                if (FAILED(Input ? R->GetInputParameterDesc(I, &P) : R->GetOutputParameterDesc(I, &P))) continue;
                Parameter Q;
                Q.Semantic = P.SemanticName != nullptr ? P.SemanticName : "";
                Q.Index = P.SemanticIndex, Q.Register = P.Register, Q.SystemValue = (uint32_t)P.SystemValueType;
                Q.Component = ComponentName(P.ComponentType);
                // an output's used mask: the components it never writes
                Q.Mask = P.Mask, Q.Used = Input ? P.ReadWriteMask : (uint8_t)(~P.ReadWriteMask & 0xf);
                Out.push_back(Q);
            }
            return Out;
        }

        // ---- the disassembly -----------------------------------------------------------------

        // A list split at its top-level commas, each part trimmed
        std::vector<std::string> SplitTop(const std::string& S)
        {
            std::vector<std::string> Parts;
            int Depth = 0;
            bool Quote = false;
            std::string Cur;
            for (const char C : S)
            {
                if (C == '"') Quote = !Quote;
                if (!Quote && (C == '{' || C == '[' || C == '<' || C == '(')) Depth++;
                if (!Quote && (C == '}' || C == ']' || C == '>' || C == ')')) Depth--;
                if (!Quote && Depth == 0 && C == ',')
                {
                    Parts.push_back(Cur);
                    Cur.clear();
                    continue;
                }
                Cur += C;
            }
            if (!Cur.empty()) Parts.push_back(Cur);
            for (auto& P : Parts)
            {
                P.erase(0, P.find_first_not_of(' '));
                P.erase(P.find_last_not_of(' ') + 1);
            }
            return Parts;
        }

        // A struct's name as the types map keys it: %"x" or %x -> x
        std::string StructKey(std::string S)
        {
            if (!S.empty() && S[0] == '%') S.erase(0, 1);
            if (S.size() > 1 && S.front() == '"' && S.back() == '"') S = S.substr(1, S.size() - 2);
            return S;
        }

        // An LLVM type as HLSL: <4 x float> float4, [4 x T] T[4], %hostlayout.struct.X X,
        // class.Texture2D<vector<float, 4> > Texture2D<float4>
        std::string Hlsl(std::string T)
        {
            std::erase(T, '"');
            if (T.size() > 2 && T.front() == '[')
            {
                const size_t X = T.find(" x ");
                if (X == std::string::npos) return T;
                const std::string N = T.substr(1, X - 1), Inner = Hlsl(T.substr(X + 3, T.size() - X - 4));
                return N == "0" ? Inner : Inner + "[" + N + "]";
            }
            if (T.size() > 2 && T.front() == '<')
            {
                const size_t X = T.find(" x ");
                if (X == std::string::npos) return T;
                return Hlsl(T.substr(X + 3, T.size() - X - 4)) + T.substr(1, X - 1);
            }
            for (const char* P : { "%hostlayout.struct.", "%hostlayout.", "%struct.", "%class.", "%" })
                if (T.rfind(P, 0) == 0)
                {
                    T.erase(0, std::strlen(P));
                    break;
                }
            for (size_t V; (V = T.find("vector<")) != std::string::npos;)
            {
                const size_t C = T.find(", ", V), E = T.find('>', V);
                if (C == std::string::npos || E == std::string::npos || C > E) break;
                T.replace(V, E + 1 - V, T.substr(V + 7, C - V - 7) + T.substr(C + 2, E - C - 2));
            }
            for (size_t Sp; (Sp = T.find(" >")) != std::string::npos;) T.erase(Sp, 1);
            if (T == "i32") return "int";
            if (T == "i16") return "int16_t";
            if (T == "i64") return "int64_t";
            if (T == "i1") return "bool";
            return T;
        }

        // The struct types of the module: name (without %) -> body between the braces
        struct Types
        {
            std::map<std::string, std::string> Structs;

            // Where a member of type T starts after Offset and its size, by the legacy cbuffer
            // rules (a scalar or vector stays in its 16-byte row, an array or struct starts one);
            // false when a type is not known
            bool Layout(const std::string& T, uint32_t Offset, uint32_t& Start, uint32_t& Size, int Depth = 0) const
            {
                if (Depth > 32) return false;
                const auto Scalar = [](const std::string& N) -> uint32_t {
                    if (N == "float" || N == "i32" || N == "i1" || N == "i8") return 4;
                    if (N == "half" || N == "i16") return 2;
                    if (N == "double" || N == "i64") return 8;
                    return 0;
                };
                if (const uint32_t Bytes = Scalar(T))
                {
                    Start = (Offset % 16) + Bytes > 16 ? (Offset + 15) / 16 * 16 : Offset;
                    Size = Bytes;
                    return true;
                }
                const size_t X = T.find(" x ");
                if (T.size() > 2 && T.front() == '<' && T.back() == '>' && X != std::string::npos)
                {
                    const uint32_t Bytes = Scalar(T.substr(X + 3, T.size() - X - 4));
                    if (Bytes == 0) return false;
                    Size = (uint32_t)std::stoul(T.substr(1, X - 1)) * Bytes;
                    Start = (Offset % 16) + Size > 16 ? (Offset + 15) / 16 * 16 : Offset;
                    return true;
                }
                if (T.size() > 2 && T.front() == '[' && T.back() == ']' && X != std::string::npos)
                {
                    const uint32_t N = (uint32_t)std::stoul(T.substr(1, X - 1));
                    uint32_t ES = 0, EZ = 0;
                    if (!Layout(T.substr(X + 3, T.size() - X - 4), 0, ES, EZ, Depth + 1)) return false;
                    Start = (Offset + 15) / 16 * 16;
                    Size = N == 0 ? 0 : (EZ + 15) / 16 * 16 * (N - 1) + EZ;
                    return true;
                }
                if (!T.empty() && T.front() == '%')
                {
                    const auto It = Structs.find(StructKey(T));
                    if (It == Structs.end()) return false;
                    Start = (Offset + 15) / 16 * 16;
                    uint32_t End = 0;
                    for (const auto& F : SplitTop(It->second))
                    {
                        uint32_t FS = 0, FZ = 0;
                        if (!Layout(F, End, FS, FZ, Depth + 1)) return false;
                        End = FS + FZ;
                    }
                    Size = End;
                    return true;
                }
                return false;
            }
        };

        // The metadata references of a node's text ("!12, null, !14"), in order, "null" kept
        std::vector<std::string> Refs(const std::string& Text)
        {
            std::vector<std::string> Out;
            for (size_t P = 0; P < Text.size();)
            {
                if (Text.compare(P, 4, "null") == 0)
                {
                    Out.push_back("null");
                    P += 4;
                }
                else if (Text[P] == '!' && P + 1 < Text.size() && std::isdigit((unsigned char)Text[P + 1]))
                {
                    size_t E = P + 1;
                    while (E < Text.size() && std::isdigit((unsigned char)Text[E])) E++;
                    Out.push_back(Text.substr(P, E - P));
                    P = E;
                }
                else
                    P++;
            }
            return Out;
        }

        std::vector<std::string_view> Lines(const std::string& Text)
        {
            std::vector<std::string_view> Out;
            for (size_t P = 0; P < Text.size();)
            {
                size_t E = Text.find('\n', P);
                if (E == std::string::npos) E = Text.size();
                std::string_view L(Text.data() + P, E - P);
                if (!L.empty() && L.back() == '\r') L.remove_suffix(1);
                Out.push_back(L);
                P = E + 1;
            }
            return Out;
        }

        // The resource list of the module (!dx.resources): each resource's range id, HLSL type,
        // space, register and count, a cbuffer's size and its struct's members laid out
        std::vector<Resource> ModuleResources(const std::vector<std::string_view>& Text)
        {
            Types Ty;
            std::unordered_map<std::string, std::string> Meta;
            std::string Root;
            for (const auto Line : Text)
            {
                if (Line.size() > 2 && Line[0] == '%')
                {
                    const size_t Eq = Line.find(" = type {");
                    if (Eq != std::string::npos && Line.back() == '}')
                        Ty.Structs[StructKey(std::string(Line.substr(0, Eq)))] = std::string(Line.substr(Eq + 9, Line.size() - Eq - 10));
                }
                else if (Line.starts_with("!dx.resources = !{"))
                    Root = std::string(Line.substr(18, Line.size() - 19));
                else if (Line.size() > 1 && Line[0] == '!' && std::isdigit((unsigned char)Line[1]))
                {
                    const size_t Eq = Line.find(" = ");
                    if (Eq != std::string::npos) Meta[std::string(Line.substr(0, Eq))] = std::string(Line.substr(Eq + 3));
                }
            }
            std::vector<Resource> Out;
            if (Root.empty() || !Meta.count(Root)) return Out;
            const std::vector<std::string> Lists = Refs(Meta[Root]);
            for (size_t Class = 0; Class < Lists.size() && Class < 4; Class++)
            {
                if (Lists[Class] == "null" || !Meta.count(Lists[Class])) continue;
                for (const auto& Id : Refs(Meta[Lists[Class]]))
                {
                    // !{i32 id, <type>* undef, !"name", i32 space, i32 lower bound, i32 count, ...}
                    const auto Rec = Meta.find(Id);
                    if (Rec == Meta.end()) continue;
                    const std::string& V = Rec->second;
                    const size_t TypeAt = V.find(", "), Undef = V.find("* undef, !\"");
                    if (V.rfind("!{i32 ", 0) != 0 || TypeAt == std::string::npos || Undef == std::string::npos) continue;
                    const std::string Type = V.substr(TypeAt + 2, Undef - TypeAt - 2);
                    const size_t NameEnd = V.find('"', Undef + 11);
                    if (NameEnd == std::string::npos) continue;
                    std::vector<int64_t> Ints;
                    for (size_t Q = NameEnd; (Q = V.find("i32 ", Q)) != std::string::npos;)
                    {
                        Q += 4;
                        size_t End = Q;
                        if (End < V.size() && V[End] == '-') End++;
                        while (End < V.size() && std::isdigit((unsigned char)V[End])) End++;
                        if (End == Q) continue;
                        Ints.push_back(std::stoll(V.substr(Q, End - Q)));
                    }
                    if (Ints.size() < 3) continue;
                    Resource S;
                    S.Class = (int)Class;
                    S.RangeId = std::atoi(V.c_str() + 6);
                    S.HlslType = Hlsl(Type);
                    S.Space = (uint32_t)Ints[0], S.Slot = (uint32_t)Ints[1];
                    S.Count = Ints[2] < 0 || Ints[2] == 0xffffffffll ? 0 : (int32_t)Ints[2];
                    if (Class == 2)
                    {
                        // a cbuffer: {..., i32 size, null}; its struct's members, a member by its
                        // struct's name or by its rank
                        if (Ints.size() > 3) S.Size = (uint32_t)Ints[3];
                        const auto Body = Ty.Structs.find(StructKey(Type));
                        if (Body != Ty.Structs.end())
                        {
                            uint32_t End = 0;
                            bool Ok = true;
                            const auto Fields = SplitTop(Body->second);
                            for (size_t K = 0; K < Fields.size() && Ok; K++)
                            {
                                uint32_t FS = 0, FZ = 0;
                                Ok = Ty.Layout(Fields[K], End, FS, FZ);
                                if (!Ok) break;
                                const std::string FT = Hlsl(Fields[K]);
                                S.Vars.push_back({ Fields[K][0] == '%' ? FT : std::format("{}[{}]", S.HlslType, K), FT, FS, FZ });
                                End = FS + FZ;
                            }
                            S.LayoutChecked = Ok && (End + 15) / 16 * 16 == (S.Size + 15) / 16 * 16;
                        }
                    }
                    Out.push_back(std::move(S));
                }
            }
            return Out;
        }

        // The value names an SSA line defines and reads: " %x = <rhs>" -> (x, rhs)
        bool Assignment(std::string_view L, std::string& Var, std::string_view& Rhs)
        {
            const size_t Eq = L.find(" = "), Pct = L.find('%');
            if (L.size() < 4 || L[0] != ' ' || Eq == std::string_view::npos || Pct == std::string_view::npos || Pct > Eq) return false;
            Var.assign(L.substr(Pct, Eq - Pct));
            Rhs = L.substr(Eq + 3);
            return true;
        }
        // The SSA values a right-hand side reads (%name tokens; not the types: %dx.types.Handle,
        // %"class.Texture2D<...>", %struct.X, %hostlayout.X)
        std::vector<std::string> Operands(std::string_view Rhs)
        {
            std::vector<std::string> Out;
            for (size_t P = 0; (P = Rhs.find('%', P)) != std::string_view::npos;)
            {
                size_t E = P + 1;
                while (E < Rhs.size() && (std::isalnum((unsigned char)Rhs[E]) || Rhs[E] == '_' || Rhs[E] == '.')) E++;
                const std::string_view Name = Rhs.substr(P, E - P);
                const bool Type = Name.starts_with("%dx.") || Name.starts_with("%class.") || Name.starts_with("%struct.") || Name.starts_with("%hostlayout.") ||
                                  (E < Rhs.size() && Rhs[E] == '"') || (P + 1 < Rhs.size() && Rhs[P + 1] == '"');
                if (E > P + 1 && !Type) Out.emplace_back(Name);
                P = E;
            }
            return Out;
        }

        // The loads of the buffer (Space, Slot) at "add i32 %base, N" (or a constant N), and which of
        // the values loaded index an unbounded SRV table: the value (extracted, possibly offset or
        // masked) is createHandle's index on such a range. A component k of a vector load is at N + 4k.
        std::vector<BufferRead> BufferReads(const std::vector<std::string_view>& Text, const std::vector<Resource>& Res, uint32_t Space, uint32_t Slot)
        {
            int Range = -1;
            std::set<int> Unbounded;
            for (const auto& S : Res)
            {
                if (S.Class == 0 && S.Space == Space && S.Slot == Slot) Range = S.RangeId;
                // a table: unbounded (Black Ops 6 and 7) or a large array (Modern Warfare III's 270 336)
                if (S.Class == 0 && S.Count != 1) Unbounded.insert(S.RangeId);
            }
            std::vector<BufferRead> Out;
            if (Range < 0) return Out;
            std::set<std::string> Handles;
            std::unordered_map<std::string, uint32_t> Adds;
            std::unordered_map<std::string, std::set<uint32_t>> Loads;   // a load's result -> its offset
            std::unordered_map<std::string, std::set<uint32_t>> Taint;   // a value from a loaded one -> the offsets it comes from
            std::map<uint32_t, bool> Reads;
            const std::string HandleCall = std::format("@dx.op.createHandle(i32 57, i8 0, i32 {}, ", Range);
            // the values computed from a loaded one (an offset, a mask, a cast, a phi: a waterfall
            // loop over a non-uniform index), to a fixed point (a phi may name a later value)
            std::vector<std::pair<std::string, std::string_view>> Derived;
            std::vector<std::string_view> Handled;   // createHandle on another range
            for (const auto L : Text)
            {
                std::string Var;
                std::string_view Rhs;
                if (!Assignment(L, Var, Rhs)) continue;
                for (const char* Op : { "add ", "sub ", "or ", "xor ", "and ", "shl ", "lshr ", "zext ", "trunc ", "bitcast ", "select ", "phi " })
                    if (Rhs.starts_with(Op))
                    {
                        Derived.emplace_back(Var, Rhs);
                        break;
                    }
                if (Rhs.find("@dx.op.createHandle(i32 57, i8 0, i32 ") != std::string_view::npos && Rhs.find(HandleCall) == std::string_view::npos)
                    Handled.push_back(Rhs);
                if (Rhs.find(HandleCall) != std::string_view::npos)
                {
                    Handles.insert(Var);
                    continue;
                }
                if (Rhs.find("@dx.op.annotateHandle(") != std::string_view::npos)
                {
                    for (const auto& O : Operands(Rhs))
                        if (Handles.count(O)) Handles.insert(Var);
                    continue;
                }
                if (Rhs.starts_with("add i32 %"))
                {
                    const size_t Comma = Rhs.find(", ");
                    if (Comma != std::string_view::npos && Comma + 2 < Rhs.size() && std::isdigit((unsigned char)Rhs[Comma + 2]))
                        Adds[Var] = (uint32_t)std::strtoul(std::string(Rhs.substr(Comma + 2)).c_str(), nullptr, 10);
                }
                if (Rhs.find("@dx.op.bufferLoad.") != std::string_view::npos || Rhs.find("@dx.op.rawBufferLoad.") != std::string_view::npos)
                {
                    const size_t H = Rhs.find("%dx.types.Handle %");
                    if (H == std::string_view::npos) continue;
                    const size_t HEnd = Rhs.find(',', H + 17);
                    if (HEnd == std::string_view::npos || !Handles.count(std::string(Rhs.substr(H + 17, HEnd - H - 17)))) continue;
                    const size_t A = Rhs.find("i32 ", HEnd);
                    if (A == std::string_view::npos) continue;
                    const size_t AEnd = Rhs.find(',', A);
                    const std::string Index(Rhs.substr(A + 4, (AEnd == std::string_view::npos ? Rhs.size() : AEnd) - A - 4));
                    uint32_t Offset = 0;
                    if (const auto It = Adds.find(Index); It != Adds.end()) Offset = It->second;
                    else if (!Index.empty() && std::isdigit((unsigned char)Index[0])) Offset = (uint32_t)std::strtoul(Index.c_str(), nullptr, 10);
                    else continue;
                    Reads.emplace(Offset, false);
                    Loads[Var].insert(Offset);
                    continue;
                }
                // a component of a load: extractvalue %r, k
                if (Rhs.starts_with("extractvalue "))
                {
                    const auto Ops = Operands(Rhs);
                    const size_t Last = Rhs.rfind(", ");
                    if (!Ops.empty() && Loads.count(Ops[0]) && Last != std::string_view::npos)
                    {
                        const uint32_t K = (uint32_t)std::strtoul(std::string(Rhs.substr(Last + 2)).c_str(), nullptr, 10);
                        for (const uint32_t O : Loads[Ops[0]]) Taint[Var].insert(O + 4 * K);
                    }
                }
            }
            for (bool Changed = true; Changed;)
            {
                Changed = false;
                for (const auto& [Var, Rhs] : Derived)
                    for (const auto& O : Operands(Rhs))
                        if (const auto It = Taint.find(O); It != Taint.end() && O != Var)
                        {
                            const std::set<uint32_t> From = It->second;   // (Taint[Var] may rehash)
                            auto& Mine = Taint[Var];
                            const size_t Before = Mine.size();
                            Mine.insert(From.begin(), From.end());
                            Changed |= Mine.size() != Before;
                        }
            }
            // a handle on an unbounded table indexed by a loaded value
            for (const auto Rhs : Handled)
            {
                const auto Parts = SplitTop(std::string(Rhs.substr(Rhs.find("createHandle(") + 13)));
                if (Parts.size() >= 4 && Unbounded.count(std::atoi(Parts[2].c_str() + 4)))
                    for (const auto& O : Operands(Parts[3]))
                        if (const auto It = Taint.find(O); It != Taint.end())
                            for (const uint32_t Off : It->second) Reads[Off] = true;
            }
            for (const auto& [Off, Texture] : Reads) Out.push_back({ Off, Texture });
            return Out;
        }
    }

    std::string Compiler()
    {
        const auto& D = Dxc();
        return D.Ok ? D.From : std::string();
    }

    HRESULT CreateInstance(REFCLSID Clsid, REFIID Iid, void** Out)
    {
        auto& D = Dxc();
        return D.Ok ? D.Support.CreateInstance(Clsid, Iid, (IUnknown**)Out) : E_FAIL;
    }

    std::string ProgramKind(const uint8_t* Data, size_t Size)
    {
        static const char* const Kinds[] = { "ps", "vs", "gs", "hs", "ds", "cs", "lib", "rg", "is", "ah", "ch", "ms", "cl", "as", "ms" };
        const auto P = Parts(Data, Size);
        const auto It = P.find(FourCC("DXIL"));
        if (It == P.end() || It->second.second < 4) return "";
        const uint32_t Kind = At<uint32_t>(Data, Size, It->second.first) >> 16;
        return Kind < std::size(Kinds) ? Kinds[Kind] : std::to_string(Kind);
    }

    Shader Reflect(const uint8_t* Data, size_t Size, std::optional<std::pair<uint32_t, uint32_t>> ReadsOf)
    {
        Shader R;
        const auto P = Parts(Data, Size);
        if (P.empty())
        {
            R.Status = "not a DXBC container";
            return R;
        }
        for (const auto& [Cc, Where] : P) R.Parts.push_back(std::string((const char*)&Cc, 4));
        R.Stage = ProgramKind(Data, Size);
        // without DXC: the container's own parts
        const auto Fallback = [&](const std::string& Why) {
            R.Inputs = SignaturePart(Data, Size, FourCC("ISG1"));
            R.Outputs = SignaturePart(Data, Size, FourCC("OSG1"));
            R.Resources = PsvResources(Data, Size);
            R.Ok = !R.Resources.empty() || P.count(FourCC("PSV0"));
            R.Status = R.Ok ? "psv0 (" + Why + ")" : Why;
            return R;
        };
        auto& D = Dxc();
        if (!D.Ok) return Fallback("dxcompiler.dll not found");
        ComPtr<IDxcLibrary> Library;
        ComPtr<IDxcContainerReflection> Container;
        ComPtr<IDxcBlobEncoding> Blob;
        if (FAILED(D.Support.CreateInstance(CLSID_DxcLibrary, Library.GetAddressOf())) ||
            FAILED(D.Support.CreateInstance(CLSID_DxcContainerReflection, Container.GetAddressOf())) ||
            FAILED(Library->CreateBlobWithEncodingFromPinned(Data, (UINT32)Size, 0, &Blob)) || FAILED(Container->Load(Blob.Get())))
            return Fallback("the container does not load");
        UINT32 Part = 0;
        R.Status = "stat";
        if (FAILED(Container->FindFirstPartKind(hlsl::DFCC_ShaderStatistics, &Part)))
        {
            R.Status = "dxil";
            if (FAILED(Container->FindFirstPartKind(hlsl::DFCC_DXIL, &Part))) return Fallback("no DXIL part");
        }
        ComPtr<ID3D12ShaderReflection> Refl;
        D3D12_SHADER_DESC Desc{};
        if (FAILED(Container->GetPartReflection(Part, __uuidof(ID3D12ShaderReflection), (void**)Refl.GetAddressOf())) || FAILED(Refl->GetDesc(&Desc)))
            return Fallback("the DXIL part does not reflect");
        R.Ok = true;
        R.Stage = StageName(Desc.Version);
        R.ShaderModel = std::format("{}_{}", D3D12_SHVER_GET_MAJOR(Desc.Version), D3D12_SHVER_GET_MINOR(Desc.Version));
        R.Inputs = Signature(Refl.Get(), Desc.InputParameters, true);
        R.Outputs = Signature(Refl.Get(), Desc.OutputParameters, false);
        // the module's resources (types, range ids, cbuffer layouts), then the reflection's
        // bindings: their kind and dimension, and any the module did not list
        std::string Text;
        ComPtr<IDxcCompiler> Compiler;
        ComPtr<IDxcBlobEncoding> Asm;
        if (SUCCEEDED(D.Support.CreateInstance(CLSID_DxcCompiler, Compiler.GetAddressOf())) && SUCCEEDED(Compiler->Disassemble(Blob.Get(), &Asm)) && Asm != nullptr)
            Text.assign((const char*)Asm->GetBufferPointer(), Asm->GetBufferSize());
        const auto Program = Lines(Text);
        R.Resources = ModuleResources(Program);
        for (UINT I = 0; I < Desc.BoundResources; I++)
        {
            D3D12_SHADER_INPUT_BIND_DESC B{};
            if (FAILED(Refl->GetResourceBindingDesc(I, &B))) continue;
            const int Class = ClassOf(B.Type);
            auto It = std::find_if(R.Resources.begin(), R.Resources.end(),
                                   [&](const Resource& S) { return S.Class == Class && S.Space == B.Space && S.Slot == B.BindPoint; });
            if (It == R.Resources.end())
            {
                Resource S;
                S.Class = Class, S.Space = B.Space, S.Slot = B.BindPoint, S.Count = (int32_t)B.BindCount;
                R.Resources.push_back(S);
                It = R.Resources.end() - 1;
            }
            It->Kind = KindName(B.Type);
            It->Dimension = DimensionName(B.Dimension);
        }
        // a cbuffer the blob kept its variables for (a STAT part): those, by name
        for (UINT K = 0; K < Desc.ConstantBuffers; K++)
        {
            auto* Cb = Refl->GetConstantBufferByIndex(K);
            D3D12_SHADER_BUFFER_DESC Bd{};
            if (Cb == nullptr || FAILED(Cb->GetDesc(&Bd)) || Bd.Variables == 0 || Bd.Name == nullptr || !*Bd.Name) continue;
            D3D12_SHADER_INPUT_BIND_DESC B{};
            if (FAILED(Refl->GetResourceBindingDescByName(Bd.Name, &B))) continue;
            for (auto& S : R.Resources)
            {
                if (S.Class != 2 || S.Space != B.Space || S.Slot != B.BindPoint) continue;
                S.Vars.clear();
                for (UINT V = 0; V < Bd.Variables; V++)
                {
                    auto* Var = Cb->GetVariableByIndex(V);
                    D3D12_SHADER_VARIABLE_DESC Vd{};
                    D3D12_SHADER_TYPE_DESC Td{};
                    if (Var == nullptr || FAILED(Var->GetDesc(&Vd))) continue;
                    std::string Type;
                    if (auto* T = Var->GetType(); T != nullptr && SUCCEEDED(T->GetDesc(&Td)) && Td.Name) Type = Td.Name;
                    S.Vars.push_back({ Vd.Name ? Vd.Name : "", Type, Vd.StartOffset, Vd.Size });
                }
                S.Size = Bd.Size, S.LayoutChecked = true;
            }
        }
        std::sort(R.Resources.begin(), R.Resources.end(),
                  [](const Resource& A, const Resource& B) { return std::tie(A.Class, A.Space, A.Slot) < std::tie(B.Class, B.Space, B.Slot); });
        if (ReadsOf) R.Reads = BufferReads(Program, R.Resources, ReadsOf->first, ReadsOf->second);
        return R;
    }

    void ToJson(const Shader& S, nlohmann::ordered_json& Out)
    {
        static const char* const Classes[] = { "srv", "uav", "cbv", "sampler" };
        static const char* const Letters[] = { "t", "u", "b", "s" };
        const auto Params = [](const std::vector<Parameter>& Ps) {
            Json A = Json::array();
            for (const auto& P : Ps)
                A.push_back({ { "semantic", P.Semantic }, { "index", P.Index }, { "register", P.Register }, { "system_value", P.SystemValue },
                              { "component", P.Component }, { "mask", std::format("{:x}", P.Mask) }, { "used", std::format("{:x}", P.Used) } });
            return A;
        };
        Out["shader_model"] = S.ShaderModel.empty() ? Json(nullptr) : Json(S.ShaderModel);
        Out["reflection"] = S.Status;
        Out["parts"] = S.Parts;
        Out["inputs"] = Params(S.Inputs);
        Out["outputs"] = Params(S.Outputs);
        Json Cbs = Json::array(), Res = Json::array();
        for (const auto& R : S.Resources)
        {
            const std::string Register = std::format("{}{}", Letters[R.Class & 3], R.Slot);
            if (R.Class == 2)
            {
                Json Vars = Json::array();
                for (const auto& V : R.Vars) Vars.push_back({ { "name", V.Name }, { "offset", V.Offset }, { "size", V.Size }, { "type", V.Type } });
                Cbs.push_back({ { "name", R.HlslType.empty() ? Json(nullptr) : Json(R.HlslType) }, { "register", Register }, { "slot", R.Slot },
                                { "space", R.Space }, { "size", R.Size }, { "layout_checked", R.LayoutChecked }, { "vars", Vars } });
            }
            else
                Res.push_back({ { "name", nullptr }, { "class", Classes[R.Class & 3] }, { "type", R.Kind }, { "hlsl_type", R.HlslType.empty() ? Json(nullptr) : Json(R.HlslType) },
                                { "dimension", R.Dimension.empty() ? Json(nullptr) : Json(R.Dimension) }, { "register", Register }, { "slot", R.Slot },
                                { "space", R.Space }, { "count", R.Count } });
        }
        Out["cbuffers"] = Cbs;
        Out["resources"] = Res;
        Json Reads = Json::array();
        for (const auto& R : S.Reads) Reads.push_back({ { "offset", R.Offset }, { "texture_index", R.TextureIndex } });
        Out["material_reads"] = Reads;
    }
}
