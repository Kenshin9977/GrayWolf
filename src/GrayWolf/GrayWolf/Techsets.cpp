#include "stdafx.h"

// The techsets kind (Techsets.h): one reader for every game's techsets, the game's Format giving
// its layouts and its arguments' meaning.

#include "Techsets.h"

#include "DiskGame.h"
#include "DxilReflection.h"
#include "ExportNaming.h"
#include "ExportWriters.h"
#include "FastFile.h"
#include "ZonekitZone.h"

#include <algorithm>
#include <execution>
#include <filesystem>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>

namespace Techsets
{
    namespace
    {
        namespace fs = std::filesystem;
        using Json = nlohmann::ordered_json;

        constexpr uint64_t IdBits = 0x7FFFFFFFFFFFFFFFull;

        std::string Id(uint64_t H) { return std::format("{:016x}", H & IdBits); }
        std::string Csv(const std::string& V) { return ExportWriters::CsvField(V); }

        // The zone's bytes as the reader reads them
        struct Reader
        {
            const ZoneView& Z;
            ByteView B;

            template <class T>
            T At(int64_t Pos) const { return ExportNaming::At<T>(B, Pos); }
            int64_t Ptr(int64_t Field) const { return Field < 0 ? -1 : Z.Ptr(Field); }
            std::string Hex(int64_t Pos, int64_t N) const { return Techsets::Hex(B, Pos, N); }
            std::string Text(int64_t Pos) const
            {
                std::string S;
                for (int64_t C = Pos; C >= 0 && C < (int64_t)B.size() && B[(size_t)C]; C++) S.push_back((char)B[(size_t)C]);
                return S;
            }
            // the hash an asset pointer field names: the asset's in the zone, or (bit 63) the hash itself
            uint64_t HashAt(int64_t Field) const
            {
                const int64_t P = Ptr(Field);
                if (P >= 0) return At<uint64_t>(P) & IdBits;
                const uint64_t V = At<uint64_t>(Field);
                return (V >> 63) ? V & IdBits : 0;
            }
            std::string Name(uint64_t H) const { return Z.NameOf ? Z.NameOf(H & IdBits) : std::string(); }
            Json NameJson(uint64_t H) const
            {
                const std::string N = Name(H);
                return N.empty() ? Json(nullptr) : Json(N);
            }
        };

        struct Technique
        {
            int64_t Pos = -1;
            uint64_t Id = 0;
            uint32_t Slot = 0;
            std::vector<std::pair<const char*, uint64_t>> Shaders;   // stage, shader id
            std::vector<Argument> Args;
        };
        struct Techset
        {
            uint64_t Id = 0;
            int64_t Pos = -1;
            std::vector<Technique> Techniques;
        };

        // The techsets a zone defines (a name-only one, defined elsewhere, counted in References);
        // OnShader reads a technique's shader record (stage, position) into its id
        std::vector<Techset> ReadTechsets(const Reader& R, const Layout& L, size_t& References,
                                          const std::function<uint64_t(const char*, int64_t)>& OnShader)
        {
            std::vector<Techset> Out;
            std::set<uint64_t> Done;
            for (const auto& [Type, Pos] : R.Z.Assets)
            {
                if (Type != "techset") continue;
                Techset Ts;
                Ts.Id = R.At<uint64_t>(Pos) & IdBits, Ts.Pos = Pos;
                const int64_t List = R.Ptr(Pos + L.TechsetTechniques);
                if (List < 0)
                {
                    References++;
                    continue;
                }
                if (!Done.insert(Ts.Id).second) continue;
                uint32_t K = 0;
                for (int64_t W = 0; W < L.TechsetMaskWords; W++)
                {
                    const uint64_t Mask = R.At<uint64_t>(Pos + L.TechsetMasks + 8 * W);
                    for (uint32_t Bit = 0; Bit < 64; Bit++)
                    {
                        if (((Mask >> Bit) & 1) == 0) continue;
                        const int64_t T = R.Ptr(List + 8 * (int64_t)K++);
                        if (T < 0) continue;
                        Technique Tq;
                        Tq.Pos = T, Tq.Id = R.At<uint64_t>(T) & IdBits, Tq.Slot = (uint32_t)(64 * W + Bit);
                        for (const auto& [Off, Stage] : L.TechniqueStages)
                            if (const int64_t S = R.Ptr(T + Off); S >= 0) Tq.Shaders.emplace_back(Stage, OnShader ? OnShader(Stage, S) : 0);
                        const int64_t A = R.Ptr(T + L.TechniqueArgs);
                        const uint32_t Count = R.At<uint8_t>(T + L.TechniqueArgCount);
                        for (uint32_t I = 0; A >= 0 && I < Count; I++)
                        {
                            const int64_t E = A + 8 * (int64_t)I;
                            Tq.Args.push_back({ R.At<uint8_t>(E), R.At<uint8_t>(E + 1), R.At<uint8_t>(E + 6), R.At<uint8_t>(E + 7), R.At<uint16_t>(E + 2),
                                                R.At<uint16_t>(E + 4) });
                        }
                        Ts.Techniques.push_back(std::move(Tq));
                    }
                }
                Out.push_back(std::move(Ts));
            }
            return Out;
        }

        // The install's techsets (its techsets_*.ff and code_post_gfx.ff, the only other zone
        // holding some), walked once per folder: what a zone's materials use when it does not
        // define their techset. Their techniques' arguments only.
        struct Library
        {
            std::map<uint64_t, Techset> Techsets;
            size_t Zones = 0;
        };
        const Library& LibraryOf(const ZoneView& Z, const Layout& L)
        {
            static std::mutex Lock;
            static std::map<std::string, std::unique_ptr<Library>> Cache;
            std::lock_guard<std::mutex> Guard(Lock);
            auto& Slot = Cache[Z.Profile + "|" + Z.Folder];
            if (Slot) return *Slot;
            Slot = std::make_unique<Library>();
            std::string Error;
            const auto* Loaded = ExportWriters::ZoneProfile(Z.Profile, Error);
            std::error_code Ec;
            if (Loaded == nullptr || Z.Folder.empty()) return *Slot;
            std::vector<fs::path> Files;
            for (const auto& E : fs::directory_iterator(Z.Folder, Ec))
            {
                const std::string F = E.path().filename().string();
                if ((F.rfind("techsets_", 0) == 0 || F == "code_post_gfx.ff") && E.path().extension() == ".ff") Files.push_back(E.path());
            }
            std::sort(Files.begin(), Files.end());
            for (const auto& F : Files)
            {
                ZoneBytes Bytes;
                if (!FastFile::Decompress(F.string(), Bytes, Error)) continue;
                const auto W = ZoneTables::WalkZone(Bytes, Loaded->Tables, Loaded->Profile.TypeNames, Loaded->Profile.Rules, true);
                if (W.ParseFailed) continue;
                const auto Links = ResolveLinks(W, Loaded->Profile.Rules, true, Bytes);
                std::unordered_map<int64_t, int64_t> Map(Links.Links.begin(), Links.Links.end());
                const ByteView Image = Bytes;
                ZoneView V;
                V.Name = F.stem().string();
                V.Image = &Image;
                V.Ptr = [&Map](int64_t Field) {
                    const auto It = Map.find(Field);
                    return It == Map.end() ? (int64_t)-1 : It->second;
                };
                for (const auto& I : W.Instances)
                    if (I.Owner < W.OwnerNames.size() && W.OwnerNames[I.Owner] == "techset") V.Assets.emplace_back("techset", I.Pos);
                size_t Refs = 0;
                for (auto& Ts : ReadTechsets(Reader{ V, Bytes }, L, Refs, nullptr))
                {
                    for (auto& Tq : Ts.Techniques) Tq.Pos = -1, Tq.Shaders.clear();
                    Slot->Techsets.emplace(Ts.Id, std::move(Ts));
                }
                Slot->Zones++;
            }
            return *Slot;
        }

        struct Shader
        {
            uint64_t Id = 0;
            int64_t Pos = -1;
            const char* Stage = "";
            std::string Name;
            uint32_t DxbcSize = 0;
            std::vector<uint8_t> Dxbc;
            std::string Prefix;   // what precedes the DXBC where it is stored (hex)
            bool InZone = false, Empty = false;
            uint64_t Key = 0;     // its package object's
            DxilReflection::Shader Refl;
        };

        // The DXBC container an object holds (after its header: {0, -4, size}), cut to its own size
        bool ContainerIn(const uint8_t* V, size_t Size, Shader& S)
        {
            for (size_t K = 0; K + 0x20 <= Size && K <= 0x40; K += 4)
                if (std::memcmp(V + K, "DXBC", 4) == 0)
                {
                    uint32_t N = 0;
                    std::memcpy(&N, V + K + 0x18, 4);
                    if (N < 0x20 || K + N > Size) return false;
                    S.Dxbc.assign(V + K, V + K + N);
                    for (size_t J = 0; J < K; J++) S.Prefix += std::format("{:02x}", V[J]);
                    return true;
                }
            return false;
        }

        std::string StagesText(uint8_t S)
        {
            std::string T = std::string(S & 1 ? "v" : "") + (S & 2 ? "p" : "");
            if (S & ~3) T += std::format("+{:x}", S & ~3);
            return T;
        }

        Json RawFields(const Reader& R, int64_t Pos, const std::vector<std::pair<int64_t, int64_t>>& Fields)
        {
            Json Raw = Json::object();
            for (const auto& [Off, N] : Fields) Raw[std::format("0x{:02x}", Off)] = R.Hex(Pos + Off, N);
            return Raw;
        }
    }

    std::string Hex(ByteView B, int64_t Pos, int64_t N)
    {
        std::string S;
        for (int64_t K = 0; K < N && Pos >= 0 && Pos + K < (int64_t)B.size(); K++) S += std::format("{:02x}", B[(size_t)(Pos + K)]);
        return S;
    }

    Block Format::BlockOf(const ZoneView&, const MaterialSlots& M) const
    {
        const uint32_t Header = Layouts().BlockHeader, NC = (uint32_t)M.ConstantSemantics.size();
        Block B;
        for (uint32_t J = 0; J < NC; J++) B.Constants.push_back(Header + 16 * J);
        for (uint32_t K = 0; K < M.ImageSemantics.size(); K++) B.Images.push_back(Header + 16 * NC + 4 * K);
        return B;
    }

    void Export(const ZoneView& Z, const Format& F, const DiskGame::Context& C, DiskGame::Totals& T)
    {
        using DiskGame::Kind;
        const Layout& L = F.Layouts();
        const Reader R{ Z, *Z.Image };
        auto& Kt = T.Kinds[Kind::Techsets];
        auto& Ct = T.Counters;

        // ---- the shader records: their bytecode from the zone, else from the packages ----
        std::map<uint64_t, Shader> Shaders;
        const auto ReadShader = [&](const char* Stage, int64_t Pos) -> uint64_t {
            const uint64_t SId = R.At<uint64_t>(Pos) & IdBits;
            auto [It, New] = Shaders.try_emplace(SId);
            if (!New) return SId;
            Shader& S = It->second;
            S.Id = SId, S.Pos = Pos, S.Stage = Stage;
            if (L.ShaderName >= 0) S.Name = R.Text(R.Ptr(Pos + L.ShaderName));
            S.DxbcSize = R.At<uint32_t>(Pos + L.ShaderDxbcSize);
            if ((R.At<uint32_t>(Pos + L.ShaderCode + 0xc) >> 31) != 0)
            {
                const int64_t Code = R.Ptr(Pos + L.ShaderCode);
                const uint32_t N = R.At<uint32_t>(Pos + L.ShaderCode + 8);
                S.InZone = Code >= 0 && Code + N <= (int64_t)R.B.size() && ContainerIn(R.B.data() + Code, N, S);
                return SId;
            }
            S.Key = R.At<uint64_t>(Pos + L.ShaderCode + 8);
            // a stage with no program (no key, no bytecode: a depth-only pass's pixel shader)
            if (S.Key == 0 && S.DxbcSize == 0)
            {
                S.Empty = true;
                return SId;
            }
            std::vector<uint8_t> Object;
            if (S.Key != 0 && Z.Package && Z.Package(S.Key, Object)) ContainerIn(Object.data(), Object.size(), S);
            return SId;
        };

        // ---- the techsets ----
        size_t References = 0;
        const auto Own = ReadTechsets(R, L, References, ReadShader);
        std::map<uint64_t, const Techset*> Known;
        std::set<int64_t> TechniquesSeen;
        Json Sets = Json::array();
        for (const auto& Ts : Own)
        {
            Known[Ts.Id] = &Ts;
            Json J = { { "id", Id(Ts.Id) }, { "name", R.NameJson(Ts.Id) } };
            if (!J["name"].is_null()) Ct["techsets_named"]++;
            Json Qs = Json::array();
            for (const auto& Tq : Ts.Techniques)
            {
                if (TechniquesSeen.insert(Tq.Pos).second) Ct["techsets_techniques"]++;
                Json Q = { { "slot", Tq.Slot }, { "id", Id(Tq.Id) }, { "name", R.NameJson(Tq.Id) } };
                Json Sh = Json::object();
                for (const auto& [Off, Stage] : L.TechniqueStages) Sh[Stage] = nullptr;
                for (const auto& [Stage, SId] : Tq.Shaders) Sh[Stage] = Id(SId);
                Q["shaders"] = Sh;
                Json Args = Json::array();
                for (const auto& A : Tq.Args)
                {
                    const Binding B = F.Bind(A);
                    Args.push_back({ { "type", A.Type }, { "kind", B.Kind }, { "stages", StagesText(A.Stages) },
                                     { "register", B.Register.empty() ? Json(nullptr) : Json(B.Register) }, { "space", B.Space >= 0 ? Json(B.Space) : Json(nullptr) },
                                     { "dest", A.Dest }, { "source", A.Source }, { "view", A.View }, { "dimension", A.Dimension } });
                }
                Q["arguments"] = Args;
                Json Extra = Json::object(), Raw = RawFields(R, Tq.Pos, L.TechniqueRaw);
                F.TechniqueFields(Z, Tq.Pos, Extra, Raw);
                for (auto& [K, V] : Extra.items()) Q[K] = V;
                Q["raw"] = Raw;
                Qs.push_back(Q);
            }
            J["techniques"] = Qs;
            J["raw"] = RawFields(R, Ts.Pos, L.TechsetRaw);
            Sets.push_back(J);
        }
        Ct["techsets"] += Own.size();
        Kt.Seen += Own.size();
        Kt.References += References;

        // ---- the shaders: reflected on every core (DXC: 20 to 40 ms a shader) ----
        std::vector<Shader*> Work;
        for (auto& [SId, S] : Shaders)
            if (!S.Dxbc.empty()) Work.push_back(&S);
        const std::pair<uint32_t, uint32_t> MaterialBuffer{ 1, L.MaterialBuffer };
        std::for_each(std::execution::par, Work.begin(), Work.end(),
                      [&](Shader* S) { S->Refl = DxilReflection::Reflect(S->Dxbc.data(), S->Dxbc.size(), MaterialBuffer); });
        const fs::path Dir = fs::path(C.KindDir(Kind::Techsets)) / ExportNaming::SafeFileName(Z.Name);
        std::error_code Ec;
        if (!Shaders.empty()) fs::create_directories(Dir / "shaders", Ec);
        Json ShaderList = Json::array();
        for (auto& [SId, S] : Shaders)
        {
            Json J = { { "id", Id(SId) }, { "name", S.Name.empty() ? Json(nullptr) : Json(S.Name) }, { "stage", S.Stage }, { "size", S.DxbcSize },
                       { "in_zone", S.InZone }, { "source_zone", S.InZone ? Json(Z.Name) : Json(nullptr) },
                       { "xsub_key", S.InZone || S.Key == 0 ? Json(nullptr) : Json(Id(S.Key)) }, { "found", !S.Dxbc.empty() } };
            Ct["techsets_shaders"]++;
            if (S.Dxbc.empty())
            {
                Ct[S.Empty ? "techsets_shaders_empty" : "techsets_shaders_missing"]++;
                J["file"] = nullptr;
                J["reflection"] = S.Empty ? "empty record (no key, no bytecode)" : "no bytecode";
            }
            else
            {
                Ct[S.InZone ? "techsets_shaders_in_zone" : "techsets_shaders_in_packages"]++;
                const std::string Rel = std::format("shaders/{}.{}.dxbc", Id(SId), S.Stage);
                if (!ExportWriters::WriteBytes(Dir / Rel, S.Dxbc.data(), S.Dxbc.size()))
                {
                    T.Fail(Z.Name, Kind::Techsets, Id(SId), "write failed: " + (Dir / Rel).string());
                    continue;
                }
                J["file"] = Rel;
                J["stored_prefix"] = S.Prefix;
                if (S.Refl.Ok) Ct[S.Refl.Status.rfind("psv0", 0) == 0 ? "techsets_shaders_reflected_psv0" : "techsets_shaders_reflected"]++;
                else Ct["techsets_shaders_not_reflected"]++;
                if (!S.Refl.Stage.empty() && S.Refl.Stage != S.Stage) Ct["techsets_shaders_stage_mismatch"]++;
                DxilReflection::ToJson(S.Refl, J);
            }
            J["raw"] = RawFields(R, S.Pos, L.ShaderRaw);
            ShaderList.push_back(J);
        }

        // ---- the materials: their block in the material buffer, their techniques' arguments ----
        std::string Rows = "material_id,material_name,techset_id,technique_slot,kind,register,space,value\n";
        struct Recipe
        {
            std::set<uint64_t> Materials;
            std::string Example;
        };
        std::map<std::tuple<uint64_t, std::string, std::string>, Recipe> Recipes;   // (techset, kind, what) -> materials
        const Library* Lib = nullptr;
        const std::string Buffer = std::format("t{}", L.MaterialBuffer);
        std::set<uint64_t> MaterialsDone;
        for (const auto& [Type, Pos] : Z.Assets)
        {
            if (Type != "material") continue;
            const uint64_t MId = R.At<uint64_t>(Pos) & IdBits;
            const int64_t Slots = R.Ptr(Pos + L.MaterialImageSlots), Images = R.Ptr(Pos + L.MaterialImages), CSlots = R.Ptr(Pos + L.MaterialConstSlots),
                          Consts = R.Ptr(Pos + L.MaterialConsts);
            const uint32_t NS = Slots >= 0 ? R.At<uint8_t>(Pos + L.MaterialImageSlotCount) : 0, NC = CSlots >= 0 ? R.At<uint8_t>(Pos + L.MaterialConstSlotCount) : 0;
            const uint32_t NImages = R.At<uint8_t>(Pos + L.MaterialImageCount), NRows = R.At<uint8_t>(Pos + L.MaterialConstCount);
            // a material the zone only names (defined in another zone) has neither
            if (NS == 0 && NC == 0)
            {
                Ct["techsets_materials_empty"]++;
                continue;
            }
            if (!MaterialsDone.insert(MId).second) continue;
            const bool HasTechset = L.MaterialKind < 0 || (R.At<uint64_t>(Pos + L.MaterialKind) & L.MaterialKindMask) != L.MaterialNoTechset;
            if (!HasTechset) Ct["techsets_materials_decal"]++;
            const uint64_t TsId = HasTechset ? R.HashAt(Pos + L.MaterialTechset) : 0;
            MaterialSlots M;
            M.Pos = Pos;
            for (uint32_t K = 0; K < NS; K++) M.ImageSemantics.push_back(R.At<uint8_t>(Slots + 2 * K));
            for (uint32_t J = 0; J < NC; J++) M.ConstantSemantics.push_back(R.At<uint8_t>(CSlots + 2 * J));
            const Block Bk = F.BlockOf(Z, M);
            Ct["techsets_materials"]++;
            if (Bk.Layers != 0) Ct["techsets_materials_layered"]++;
            if (L.MaterialBlockSize >= 0)
                Ct[R.At<uint32_t>(Pos + L.MaterialBlockSize) == L.BlockHeader + 16 * NC + 4 * NS ? "techsets_material_blocks_sized" : "techsets_material_blocks_other"]++;
            const std::string Head = std::format("{},{},{},", Id(MId), Csv(R.Name(MId)), TsId ? Id(TsId) : std::string());
            const auto Row = [&](const std::string& Slot, const char* Kind, const std::string& Reg, const std::string& Space, const std::string& Value) {
                Rows += Head + Slot + "," + Kind + "," + Reg + "," + Space + "," + Csv(Value) + "\n";
                Ct["techsets_material_bindings"]++;
            };
            const auto Use = [&](const char* Kind, const std::string& What, const std::string& Value) {
                auto& Rc = Recipes[{ TsId, Kind, What }];
                if (Rc.Materials.insert(MId).second && Rc.Example.empty()) Rc.Example = Value;
            };
            std::map<uint32_t, std::string> Textures, Constants;   // by semantic, for the techniques' arguments
            for (uint32_t J = 0; J < NC; J++)
            {
                const uint32_t Row16 = R.At<uint8_t>(CSlots + 2 * J + 1);
                std::string Value = std::format("0x{:02x}:", M.ConstantSemantics[J]);
                if (Consts >= 0 && Row16 < NRows)
                {
                    const int64_t At = Consts + 16 * (int64_t)Row16;
                    Value += std::format(" {} {} {} {} ({})", R.At<float>(At), R.At<float>(At + 4), R.At<float>(At + 8), R.At<float>(At + 12), R.Hex(At, 16));
                }
                else
                    Value += " row out of range";
                const std::string Reg = std::format("{}+0x{:x}", Buffer, Bk.Constants[J]);
                Row("*", "constant", Reg, "1", Value);
                Use("constant", std::format("0x{:02x}@{}", M.ConstantSemantics[J], Reg), Value);
                Constants.emplace(M.ConstantSemantics[J], Value);
            }
            for (uint32_t K = 0; K < NS; K++)
            {
                const uint32_t Index = R.At<uint8_t>(Slots + 2 * K + 1);
                const uint64_t Image = Images >= 0 && Index < NImages ? R.HashAt(Images + 8 * (int64_t)Index) : 0;
                // the image's id, the slot's semantic, and the image's name when an index knows it
                const std::string Name = Image ? R.Name(Image) : std::string();
                const std::string Value = std::format("{} 0x{:02x}{}{}", Id(Image), M.ImageSemantics[K], Name.empty() ? "" : " ", Name);
                const std::string Reg = std::format("{}+0x{:x}", Buffer, Bk.Images[K]);
                Row("*", "image", Reg, "1", Value);
                Use("image", std::format("0x{:02x}@{}", M.ImageSemantics[K], Reg), Value);
                Textures.emplace(M.ImageSemantics[K], Value);
            }
            if (Bk.Layers != 0) Row("*", "other", std::format("{}+0x{:x}", Buffer, L.BlockHeader - 4), "1", std::format("layers {}", Bk.Layers));
            for (const auto& [Off, N] : L.MaterialRaw) Row("", "other", std::format("material+0x{:02x}", Off), "", R.Hex(Pos + Off, N));
            if (!F.TechniquesBindMaterials()) continue;
            // what each technique binds to a register of its own: its techset here, else the install's
            const Techset* Ts = Known.count(TsId) ? Known[TsId] : nullptr;
            if (Ts == nullptr && TsId != 0)
            {
                if (Lib == nullptr) Lib = &LibraryOf(Z, L);
                const auto It = Lib->Techsets.find(TsId);
                if (It != Lib->Techsets.end()) Ts = &It->second;
            }
            if (Ts == nullptr)
            {
                Ct["techsets_materials_no_techset"]++;
                continue;
            }
            for (const auto& Tq : Ts->Techniques)
                for (const auto& A : Tq.Args)
                {
                    const Binding B = F.Bind(A);
                    const std::string Slot = std::to_string(Tq.Slot), Space = B.Space >= 0 ? std::to_string(B.Space) : std::string();
                    if (B.From == Binding::Source::MaterialTexture && Textures.count(A.Source))
                    {
                        Row(Slot, "image", B.Register, Space, Textures[A.Source]);
                        Use("image", std::format("0x{:02x}@{}:space{}", A.Source, B.Register, Space), Textures[A.Source]);
                    }
                    else if (B.From == Binding::Source::MaterialConstant && Constants.count(A.Source))
                    {
                        Row(Slot, "constant", B.Register, Space, Constants[A.Source]);
                        Use("constant", std::format("0x{:02x}@{}:space{}", A.Source, B.Register, Space), Constants[A.Source]);
                    }
                    else if (B.From == Binding::Source::Sampler)
                    {
                        Row(Slot, "sampler", B.Register, Space, std::format("state {}", A.Source));
                        Use("sampler", B.Register, std::format("state {}", A.Source));
                    }
                }
        }
        if (Lib != nullptr)
        {
            Ct["techsets_library_zones"] = std::max(Ct["techsets_library_zones"], Lib->Zones);
            Ct["techsets_library_techsets"] = std::max(Ct["techsets_library_techsets"], Lib->Techsets.size());
        }

        // ---- the recipes: per techset, what its materials of this zone bind; a techset defined here
        // and used by none still lists what its techniques ask of a material (materials_using 0) ----
        if (F.TechniquesBindMaterials())
            for (const auto& Ts : Own)
                for (const auto& Tq : Ts.Techniques)
                    for (const auto& A : Tq.Args)
                    {
                        const Binding B = F.Bind(A);
                        if (B.From == Binding::Source::MaterialTexture) Recipes[{ Ts.Id, "image", std::format("0x{:02x}@{}:space{}", A.Source, B.Register, B.Space) }];
                        if (B.From == Binding::Source::MaterialConstant) Recipes[{ Ts.Id, "constant", std::format("0x{:02x}@{}:space{}", A.Source, B.Register, B.Space) }];
                        if (B.From == Binding::Source::Sampler) Recipes[{ Ts.Id, "sampler", B.Register }];
                    }
        std::string RecipeText = "techset_id,techset_name,kind,semantic_or_register,materials_using,example_value\n";
        for (const auto& [Key, Rc] : Recipes)
        {
            const auto& [TsId, Kind, What] = Key;
            RecipeText += std::format("{},{},{},{},{},{}\n", TsId ? Id(TsId) : std::string(), Csv(R.Name(TsId)), Kind, What, Rc.Materials.size(), Csv(Rc.Example));
        }

        if (Own.empty() && MaterialsDone.empty()) return;
        fs::create_directories(Dir, Ec);
        const auto Write = [&](const char* File, const std::string& S) {
            if (!ExportWriters::WriteBytes(Dir / File, (const uint8_t*)S.data(), S.size())) T.Fail(Z.Name, Kind::Techsets, File, "write failed: " + (Dir / File).string());
        };
        Write("techsets.json", Sets.dump(1));
        Write("shaders.json", ShaderList.dump(1));
        Write("materials.csv", Rows);
        Write("recipes.csv", RecipeText);
        Kt.Written += Own.size();
    }
}
