#include "stdafx.h"

#include "Gdt.h"

#include "ExportNaming.h"
#include "GdtWriter.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>

namespace fs = std::filesystem;
using Json = nlohmann::ordered_json;

namespace Gdt
{
namespace
{
    // A struct's field as its layout gives it (*_fields.jsonc).
    struct Field
    {
        int64_t Offset = 0;
        std::string Name, Kind, Form;   // Kind: what it holds (float, xmodel, soundAlias, xanim[]...)
        std::string Source;             // where the name comes from: engine, observed
        int Count = 0;                  // the elements of an array form
        double Maximum = 0;             // float[]@: a larger value says the pointer is not to these
        bool Indirect = false;          // address: a pointer to the pointer
        std::map<int64_t, std::string> Values;   // an enum's names
        int64_t NameAt = 0;             // asset: where the asset's name pointer is
        bool Stem = false;              // asset: the name without folders and extension
        std::string Join;               // an array of names or numbers written as one text, joined by this
        double Scale = 0;               // a number written multiplied by this
        std::string Prefix = "fx\\\\";  // fx: before the path (a GDF path relative to share/raw/fx: "")
    };

    // HydraX's stitch: the buffer's [At, At + Size) is read from where the pointer at buffer offset
    // From leads
    struct Section
    {
        int64_t At = 0, Size = 0, From = 0;
    };

    struct Layout
    {
        std::string Struct, Type;   // the game's struct name, the zone type its pool names
        int64_t NameAt = 0;         // the asset's name pointer
        std::vector<Section> Sections;
        std::vector<Field> Fields;
    };

    // A field's value as read: numbers or names, one or an array of them.
    struct Value
    {
        bool Text = false;       // names (Texts), else numbers
        bool Integral = false;   // whole numbers the struct holds (int, byte): a time in milliseconds
        bool Enum = false;       // a name of the field's enum: carried even when its number is 0
        std::vector<std::string> Texts;
        std::vector<double> Numbers;
        size_t Size() const { return Text ? Texts.size() : Numbers.size(); }
    };

    // One entry of a porting target's GDF.
    struct TargetEntry
    {
        std::string Kind, Default;   // Float, Int, CheckBox, Combo, AssetCombo, String, Path...
        bool Ranged = false;
        double Low = 0, High = 0;
        std::vector<std::string> Choices;
    };

    struct Implied
    {
        std::string Entry, From;
        std::map<std::string, std::string> Values;
    };

    std::string Lower(std::string S) { return ExportNaming::Lower(std::move(S)); }

    bool ReadJson(const fs::path& P, Json& Out, std::string& Error)
    {
        std::ifstream In(P, std::ios::binary);
        if (!In)
        {
            Error = "cannot open " + P.string();
            return false;
        }
        try
        {
            Out = Json::parse(In, nullptr, true, true);   // comments allowed: the tables are commented
            return true;
        }
        catch (const std::exception& E)
        {
            Error = P.filename().string() + ": " + E.what();
            return false;
        }
    }

    std::vector<std::string> Names(const Json& J)
    {
        std::vector<std::string> Out;
        if (J.is_string()) Out.push_back(J.get<std::string>());
        else if (J.is_array())
            for (const auto& E : J) Out.push_back(E.get<std::string>());
        return Out;
    }

    int64_t Hex(const Json& J, const char* Key)
    {
        if (!J.contains(Key)) return 0;
        const auto& V = J.at(Key);
        return V.is_string() ? std::stoll(V.get<std::string>(), nullptr, 16) : V.get<int64_t>();
    }

    // A ported number, as the porting tables were checked with (the float's shortest text)
    std::string PortNumber(double X, bool Whole)
    {
        if (Whole) return std::to_string(std::llround(X));
        char B[48];
        const auto R = std::to_chars(B, B + sizeof(B), (float)X);
        return std::string(B, R.ptr);
    }

    // A length of time the target counts in seconds: fireTime, reloadEmptyAddTime, burstFireDelay
    bool IsTime(const std::string& Entry)
    {
        return Entry.find("Time") != std::string::npos ||
               (Entry.size() > 5 && Entry.compare(Entry.size() - 5, 5, "Delay") == 0);
    }

    std::mutex RegistryLock;
    std::map<std::string, Converter>& Registry()
    {
        static std::map<std::string, Converter> R;
        return R;
    }
}

std::string Memory::AssetField(int64_t Field, int64_t NameAt) const
{
    const int64_t A = Follow(Field);
    if (A < 0) return std::string();
    std::string N = String(At<uint64_t>(A + NameAt));
    if (!N.empty() && N[0] == ',') N.erase(0, 1);   // a name-only header: the asset another zone defines
    return N;
}

std::string Number(double X, bool Whole)
{
    if (!std::isfinite(X)) return "0";
    if (Whole) return std::to_string(std::llround(X));
    if (X == 0) return "0";
    char B[96];
    const auto R = std::to_chars(B, B + sizeof(B), (float)X, std::chars_format::fixed);
    return R.ec == std::errc() ? std::string(B, R.ptr) : std::string("0");
}

std::string Text(const Block& B, bool Sorted)
{
    std::vector<std::pair<std::string, std::string>> Keys = B.Keys;
    if (Sorted)
    {
        // the Mod Tools' order: case ignored, '_' before digits and letters (material1_1_x before material1_10)
        std::vector<std::pair<std::string, size_t>> By;
        for (size_t I = 0; I < Keys.size(); I++)
        {
            std::string S = Keys[I].first;
            for (char& C : S) C = C == '_' ? (char)1 : (char)std::tolower((unsigned char)C);
            By.emplace_back(std::move(S), I);
        }
        std::stable_sort(By.begin(), By.end(), [](const auto& X, const auto& Y) { return X.first < Y.first; });
        std::vector<std::pair<std::string, std::string>> Ordered;
        for (const auto& P : By) Ordered.push_back(std::move(Keys[P.second]));
        Keys = std::move(Ordered);
    }
    std::string Out = B.Parent.empty() ? "\t\"" + B.Name + "\" ( \"" + B.Gdf + "\" )\r\n\t{\r\n"
                                       : "\t\"" + B.Name + "\" [ \"" + B.Parent + "\" ]\r\n\t{\r\n";
    for (const auto& [K, V] : Keys) Out += "\t\t\"" + K + "\" \"" + V + "\"\r\n";
    Out += "\t}\r\n";
    return Out;
}

bool Register(const char* Name, Converter Fn)
{
    std::lock_guard<std::mutex> Guard(RegistryLock);
    return Registry().emplace(Name, Fn).second;
}

Converter Find(const std::string& Name)
{
    std::lock_guard<std::mutex> Guard(RegistryLock);
    const auto It = Registry().find(Name);
    return It == Registry().end() ? nullptr : It->second;
}

struct Type::Data
{
    std::string Id, Folder, Gdf, ZoneType;
    int64_t NameAt = 0;
    bool Nested = false;
    std::vector<Type::Source> FoundIn;
    Json Settings;
    std::vector<Layout> Layouts;
    size_t Root = 0;
    bool HashNames = false;   // the name: a hash at the root's offset 0, else a string pointer
    Converter Convert = nullptr;
    std::vector<std::pair<std::string, std::string>> Constants;   // keys every block holds
    // a porting type (type.jsonc "target"): porting.jsonc and the target's entries
    bool Port = false;
    bool Milliseconds = false;
    std::map<std::string, size_t> Slots;
    std::vector<std::pair<std::string, std::map<int, std::string>>> Animations;   // field -> slot -> purpose
    std::vector<std::pair<std::string, std::vector<std::string>>> Fields, Curves;
    std::vector<std::pair<std::string, std::string>> PortConstants;
    std::vector<Implied> Implies;
    std::string Asset;
    std::vector<std::string> Order;
    std::map<std::string, TargetEntry> Entries;
    std::map<std::string, std::vector<std::string>> Purposes;   // anim_entries.jsonc

    const Layout* LayoutOf(const std::string& Kind) const
    {
        const std::string K = Lower(Kind);
        for (const auto& L : Layouts)
            if (Lower(L.Struct) == K) return &L;
        return nullptr;
    }

    // where a layout's field at Offset is in the zone: in a section, through its pointer (the
    // last section holding the offset, as HydraX's later copies overwrite the earlier); -1 when
    // the pointer leads nowhere in the zone
    int64_t Where(const Memory& M, const Layout& L, int64_t Pos, int64_t Offset, int Depth = 0) const
    {
        for (size_t I = L.Sections.size(); I-- > 0 && Depth < 8;)
        {
            const Section& S = L.Sections[I];
            if (Offset < S.At || Offset >= S.At + S.Size) continue;
            const int64_t P = Where(M, L, Pos, S.From, Depth + 1);
            const int64_t B = P < 0 ? -1 : M.Follow(P);
            return B < 0 ? -1 : B + (Offset - S.At);
        }
        return Pos + Offset;
    }

    void Read(const Memory& M, const Layout& L, int64_t Pos, std::map<std::string, Value>& Out, std::set<const Layout*>& Seen,
              Stats& S) const;
    std::string Block(const Memory& M, int64_t Pos, const std::string& Name, Stats& S) const;
    std::string Format(const Field& F, const Value& V) const;
};

std::shared_ptr<const Type> Type::Load(const std::string& Dir, const std::string& Profiles,
                                       const std::vector<std::string>& TypeNames, std::string& Error)
{
    std::shared_ptr<Type> T(new Type());
    T->D = std::make_unique<Data>();
    Data& D = *T->D;
    D.Folder = Dir;
    D.Id = fs::path(Dir).filename().string();
    try
    {
        if (!ReadJson(fs::path(Dir) / "type.jsonc", D.Settings, Error)) return nullptr;
        D.HashNames = D.Settings.value("names", std::string("string")) == "hash";
        D.Gdf = D.Settings.value("gdf", std::string());
        if (D.Settings.contains("constants"))
            for (const auto& [K, V] : D.Settings.at("constants").items()) D.Constants.emplace_back(K, V.get<std::string>());
        if (D.Settings.contains("converter"))
        {
            const std::string C = D.Settings.at("converter").get<std::string>();
            D.Convert = Find(C);
            if (!D.Convert)
            {
                Error = D.Id + ": no converter " + C + " compiled in";
                return nullptr;
            }
        }

        // ---- the layouts
        std::vector<fs::path> Files;
        for (const auto& E : fs::directory_iterator(Dir))
        {
            const std::string F = E.path().filename().string();
            if (F.size() > 13 && F.compare(F.size() - 13, 13, "_fields.jsonc") == 0) Files.push_back(E.path());
        }
        std::sort(Files.begin(), Files.end());
        for (const auto& P : Files)
        {
            Json J;
            if (!ReadJson(P, J, Error)) return nullptr;
            Layout L;
            L.Struct = J.at("struct").get<std::string>();
            const int Pool = J.at("pool").get<int>();
            if (Pool >= 0 && (size_t)Pool < TypeNames.size()) L.Type = TypeNames[(size_t)Pool];
            L.NameAt = Hex(J, "name_at");
            if (J.contains("sections"))
                for (const auto& S : J.at("sections")) L.Sections.push_back({ Hex(S, "at"), Hex(S, "size"), Hex(S, "from") });
            for (const auto& F : J.at("fields"))
            {
                Field Fd;
                Fd.Offset = std::stoll(F.at("offset").get<std::string>(), nullptr, 16);
                Fd.Name = F.at("name").get<std::string>();
                Fd.Kind = F.value("kind", std::string());
                Fd.Form = F.value("form", std::string());
                Fd.Source = F.value("source", std::string());
                Fd.Count = F.value("count", 0);
                Fd.Maximum = F.value("maximum", 0.0);
                Fd.Indirect = F.value("indirect", false);
                if (F.contains("values"))
                    for (const auto& [K, V] : F.at("values").items()) Fd.Values[std::stoll(K)] = V.get<std::string>();
                Fd.NameAt = Hex(F, "name_at");
                Fd.Stem = F.value("stem", false);
                Fd.Join = F.value("join", std::string());
                Fd.Scale = F.value("scale", 0.0);
                Fd.Prefix = F.value("prefix", Fd.Prefix);
                L.Fields.push_back(std::move(Fd));
            }
            D.Layouts.push_back(std::move(L));
        }
        // a converter reading the struct itself needs no layout: type.jsonc names the zone type
        D.ZoneType = D.Settings.value("zone_type", std::string());
        D.NameAt = Hex(D.Settings, "name_at");
        D.Nested = D.Settings.value("nested", false);
        if (D.Settings.contains("found_in"))
            for (const auto& F : D.Settings.at("found_in"))
                D.FoundIn.push_back({ F.at("zone_type").get<std::string>(), Hex(F, "pointers"), F.value("count", 0) });
        if (D.Layouts.empty() && D.Convert && !D.ZoneType.empty())
        {
            if (D.Gdf.empty()) D.Gdf = D.ZoneType + ".gdf";
            return T;
        }
        if (D.Layouts.empty())
        {
            Error = "no *_fields.jsonc in " + Dir;
            return nullptr;
        }
        // the root: the layout no other one's field leads into
        std::set<std::string> Reached;
        for (const auto& L : D.Layouts)
            for (const auto& F : L.Fields) Reached.insert(Lower(F.Kind));
        D.Root = D.Layouts.size();
        for (size_t I = 0; I < D.Layouts.size(); I++)
            if (!Reached.count(Lower(D.Layouts[I].Struct)) && D.Root == D.Layouts.size()) D.Root = I;
        if (D.Root == D.Layouts.size() || D.Layouts[D.Root].Type.empty())
        {
            Error = "no root layout (or its pool names no zone type) in " + Dir;
            return nullptr;
        }
        if (D.ZoneType.empty()) D.ZoneType = D.Layouts[D.Root].Type;
        if (!D.Settings.contains("name_at")) D.NameAt = D.Layouts[D.Root].NameAt;
        if (D.Gdf.empty()) D.Gdf = D.ZoneType + ".gdf";
        if (!D.Settings.contains("target")) return T;

        // ---- a porting type: porting.jsonc, and the target it fills
        D.Port = true;
        Json P;
        if (!ReadJson(fs::path(Dir) / "porting.jsonc", P, Error)) return nullptr;
        D.Milliseconds = P.value("times", std::string()) == "milliseconds";
        if (P.contains("slots"))
            for (const auto& [K, V] : P.at("slots").items()) D.Slots[K] = V.get<size_t>();
        if (P.contains("animations"))
            for (const auto& [K, V] : P.at("animations").items())
            {
                std::map<int, std::string> Slot;
                for (const auto& [N, Purpose] : V.items()) Slot[std::stoi(N)] = Purpose.get<std::string>();
                D.Animations.emplace_back(K, std::move(Slot));
            }
        for (const auto& [K, V] : P.at("fields").items()) D.Fields.emplace_back(K, Names(V));
        if (P.contains("curves"))
            for (const auto& [K, V] : P.at("curves").items()) D.Curves.emplace_back(K, Names(V));
        if (P.contains("constants"))
            for (const auto& [K, V] : P.at("constants").items()) D.PortConstants.emplace_back(K, V.get<std::string>());
        if (P.contains("implied"))
            for (const auto& [K, V] : P.at("implied").items())
            {
                Implied I{ K, V.at("from").get<std::string>(), {} };
                for (const auto& [From, To] : V.at("values").items()) I.Values[From] = To.get<std::string>();
                D.Implies.push_back(std::move(I));
            }

        const fs::path Target = fs::path(Profiles) / D.Settings.at("target").get<std::string>();
        Json G, A;
        if (!ReadJson(Target / "weapon_gdt.json", G, Error)) return nullptr;
        if (!ReadJson(Target / "anim_entries.jsonc", A, Error)) return nullptr;
        D.Asset = G.at("asset").get<std::string>();
        for (const auto& N : G.at("order")) D.Order.push_back(N.get<std::string>());
        for (const auto& [K, V] : G.at("entries").items())
        {
            TargetEntry E;
            E.Kind = V.value("kind", std::string());
            E.Default = V.value("default", std::string());
            if (V.contains("low") && V.contains("high"))
            {
                E.Ranged = true;
                E.Low = V.at("low").get<double>();
                E.High = V.at("high").get<double>();
            }
            if (V.contains("choices"))
                for (const auto& C : V.at("choices")) E.Choices.push_back(C.get<std::string>());
            D.Entries[K] = std::move(E);
        }
        for (const auto& [K, V] : A.at("entries").items()) D.Purposes[K] = Names(V);
    }
    catch (const std::exception& E)
    {
        Error = fs::path(Dir).filename().string() + " tables: " + E.what();
        return nullptr;
    }
    return T;
}

Type::~Type() = default;

const std::string& Type::Id() const { return D->Id; }
const std::string& Type::ZoneType() const { return D->ZoneType; }
const nlohmann::ordered_json& Type::Settings() const { return D->Settings; }
const std::string& Type::Folder() const { return D->Folder; }
bool Type::Nested() const { return D->Nested; }
const std::vector<Type::Source>& Type::FoundIn() const { return D->FoundIn; }

std::vector<std::pair<std::string, std::vector<Type::NamedField>>> Type::NamedFields() const
{
    std::vector<std::pair<std::string, std::vector<NamedField>>> Out;
    for (const auto& L : D->Layouts)
    {
        if (L.Type.empty() || !L.Sections.empty()) continue;   // (a stitched layout's offsets are not the zone's)
        std::vector<NamedField> Fields;
        for (const auto& F : L.Fields) Fields.push_back({ F.Offset, F.Name, F.Form, F.Source, F.Count });
        Out.emplace_back(L.Type, std::move(Fields));
    }
    return Out;
}

void Type::Data::Read(const Memory& M, const Layout& L, int64_t Pos, std::map<std::string, Value>& Out,
                      std::set<const Layout*>& Seen, Stats& S) const
{
    using ExportNaming::At;
    const auto& Img = *M.Image;
    Seen.insert(&L);
    std::vector<std::pair<const Layout*, int64_t>> Inner;   // read after this struct: its names first
    for (const Field& F : L.Fields)
    {
        if (Out.count(F.Name)) continue;
        const int64_t A = L.Sections.empty() ? Pos + F.Offset : Where(M, L, Pos, F.Offset);
        Value V;
        const auto Floats = [&](int64_t From, int N) {
            for (int I = 0; I < N; I++) V.Numbers.push_back(At<float>(Img, From + 4 * I));
        };
        if (F.Form == "float") Floats(A, 1);
        else if (F.Form == "vec3") Floats(A, 3);
        else if (F.Form == "float[]") Floats(A, F.Count);
        else if (F.Form == "float[]@")
        {
            if (const int64_t O = M.Off(At<uint64_t>(Img, A)); O >= 0) Floats(O, F.Count);
            if (F.Maximum > 0 && std::any_of(V.Numbers.begin(), V.Numbers.end(),
                                             [&](double X) { return !(std::fabs(X) <= F.Maximum); }))
                V.Numbers.clear();
        }
        else if (F.Form == "int" || F.Form == "byte" || F.Form == "int[]" || F.Form == "uint")
        {
            V.Integral = true;
            if (F.Form == "byte") V.Numbers.push_back(At<uint8_t>(Img, A));
            else if (F.Form == "uint") V.Numbers.push_back(At<uint32_t>(Img, A));
            else
                for (int I = 0; I < std::max(F.Count, 1); I++) V.Numbers.push_back(At<int32_t>(Img, A + 4 * I));
        }
        else if (F.Form == "msec") V.Numbers.push_back(At<int32_t>(Img, A) / 1000.0);
        else if (F.Form == "string")
        {
            V.Text = true;
            V.Texts.push_back(A < 0 ? std::string() : M.String(At<uint64_t>(Img, A)));
        }
        else if (F.Form == "chars")
        {
            V.Text = true;
            std::string T;
            for (int I = 0; A >= 0 && I < F.Count && A + I < (int64_t)Img.size() && Img[(size_t)(A + I)]; I++)
                T.push_back((char)Img[(size_t)(A + I)]);
            V.Texts.push_back(T);
        }
        else if (F.Form == "scriptstring")
        {
            V.Text = true;
            for (int I = 0; I < std::max(F.Count, 1); I++)
                V.Texts.push_back(A < 0 || !M.ScriptString ? std::string() : M.ScriptString(At<uint32_t>(Img, A + 4 * I)));
        }
        else if (F.Form == "alias")
        {
            V.Text = true;
            const uint32_t H = A < 0 ? 0 : At<uint32_t>(Img, A);
            std::string N = H == 0 || !M.AliasName ? std::string() : M.AliasName(H);
            if (H != 0)
            {
                S.Counters["aliases"]++;
                if (N.empty()) S.Counters["aliases_unnamed"]++;
            }
            V.Texts.push_back(N);
        }
        else if (F.Form == "asset" || F.Form == "fx")
        {
            V.Text = true;
            std::string N = M.AssetField(A, F.NameAt);
            if (F.Stem && !N.empty()) N = fs::path(N).stem().string();
            if (F.Form == "fx" && !N.empty())
            {
                // HydraX: fx\\ (Prefix) + the name, its folders' separators as \\, extension .efx
                std::string P = fs::path(N).replace_extension(".efx").string();
                std::string Q = F.Prefix;
                for (char C : P) Q += (C == '/' || C == '\\') ? std::string("\\\\") : std::string(1, C);
                N = Q;
            }
            V.Texts.push_back(N);
        }
        else if (F.Form == "empty")
        {
            V.Text = true;
            V.Texts.push_back(std::string());
        }
        else if (F.Form == "hash")
        {
            V.Text = true;
            const uint64_t H = At<uint64_t>(Img, A) & M.HashMask;
            std::string N;
            if (H != 0)
            {
                S.Hashes++;
                N = M.HashName(H);
                if (!N.empty()) S.HashesNamed++;
                else
                {
                    char B[32];
                    std::snprintf(B, sizeof(B), "hash_%llx", (unsigned long long)H);
                    N = B;
                }
            }
            V.Texts.push_back(N);
        }
        else if (F.Form == "address")
        {
            V.Text = true;
            uint64_t P = At<uint64_t>(Img, A);
            if (F.Indirect)
            {
                const int64_t O = M.Off(P);
                P = O < 0 ? 0 : At<uint64_t>(Img, O);
            }
            const int64_t O = M.Off(P);
            const bool Array = F.Kind.size() > 2 && F.Kind.compare(F.Kind.size() - 2, 2, "[]") == 0;
            if (Array)
            {
                const std::string Element = F.Kind.substr(0, F.Kind.size() - 2);
                for (int I = 0; O >= 0 && I < F.Count; I++)
                {
                    const int64_t E = M.Off(At<uint64_t>(Img, O + 8 * I));
                    V.Texts.push_back(E < 0 ? std::string() : M.AssetName(Element, E));
                }
            }
            else if (const Layout* Sub = LayoutOf(F.Kind))
            {
                if (O >= 0 && !Seen.count(Sub)) Inner.emplace_back(Sub, O);
                continue;
            }
            else V.Texts.push_back(O < 0 ? std::string() : M.AssetName(F.Kind, O));
        }
        else continue;
        if (!F.Values.empty() && !V.Text && V.Numbers.size() == 1)
            if (const auto It = F.Values.find(std::llround(V.Numbers[0])); It != F.Values.end())
            {
                V.Text = V.Enum = true;
                V.Texts = { It->second };
            }
        Out[F.Name] = std::move(V);
    }
    for (const auto& [Sub, O] : Inner)
        if (!Seen.count(Sub)) Read(M, *Sub, O, Out, Seen, S);
}

std::string Type::Data::Format(const Field& F, const Value& V) const
{
    if (V.Text)
    {
        if (V.Texts.empty()) return std::string();
        if (V.Texts.size() == 1 || F.Join.empty()) return V.Texts[0];
        // the names up to the last one held, joined (the hide tags' \r\n, as the Mod Tools write them)
        size_t Last = 0;
        for (size_t I = 0; I < V.Texts.size(); I++)
            if (!V.Texts[I].empty()) Last = I + 1;
        std::string Out;
        for (size_t I = 0; I < Last; I++) Out += (I ? F.Join : std::string()) + V.Texts[I];
        return Out;
    }
    if (V.Numbers.empty()) return "0";
    if (V.Numbers.size() > 1 && !F.Join.empty())
    {
        // an array of numbers as one text (a Color entry's "r g b a")
        std::string Out;
        for (size_t I = 0; I < V.Numbers.size(); I++)
            Out += (I ? F.Join : std::string()) + Number(V.Numbers[I] * (F.Scale != 0 ? F.Scale : 1.0), V.Integral && F.Scale == 0);
        return Out;
    }
    // a number the enum does not name: HydraX's arrays would throw; the number is kept
    const double X = V.Numbers[0] * (F.Scale != 0 ? F.Scale : 1.0);
    return Number(X, V.Integral && F.Scale == 0);
}

std::string Type::Name(const Memory& M, int64_t Pos) const
{
    if (!D->HashNames) return M.String(M.At<uint64_t>(Pos + D->NameAt));
    if (D->Layouts.empty()) return std::string();
    for (const Field& F : D->Layouts[D->Root].Fields)
        if (F.Offset == 0 && F.Form == "hash")
        {
            const uint64_t H = ExportNaming::At<uint64_t>(*M.Image, Pos) & M.HashMask;
            if (H == 0) break;
            std::string N = M.HashName(H);
            if (N.empty())
            {
                char B[32];
                std::snprintf(B, sizeof(B), "%s_%llx", D->Id.c_str(), (unsigned long long)H);
                N = B;
            }
            return N;
        }
    return std::string();
}

std::map<std::string, Type::FieldValue> Type::Values(const Memory& M, int64_t Pos, Stats& S) const
{
    std::map<std::string, FieldValue> Out;
    if (D->Layouts.empty()) return Out;
    std::map<std::string, Value> Read;
    std::set<const Layout*> Seen;
    D->Read(M, D->Layouts[D->Root], Pos, Read, Seen, S);
    for (auto& [Name, V] : Read) Out[Name] = { std::move(V.Numbers), std::move(V.Texts) };
    return Out;
}

std::vector<std::pair<std::string, std::string>> Type::Fields(const Memory& M, int64_t Pos, Stats& S) const
{
    std::vector<std::pair<std::string, std::string>> Out;
    if (D->Layouts.empty()) return Out;
    std::map<std::string, Value> Values;
    std::set<const Layout*> Seen;
    D->Read(M, D->Layouts[D->Root], Pos, Values, Seen, S);
    for (const auto& L : D->Layouts)
        for (const Field& F : L.Fields)
            if (const auto It = Values.find(F.Name); It != Values.end())
            {
                Out.emplace_back(F.Name, D->Format(F, It->second));
                Values.erase(It);   // a name twice: its first field
            }
    return Out;
}

std::string Type::Convert(const Memory& M, int64_t Pos, const std::string& Name, Stats& S) const
{
    if (D->Port) return D->Block(M, Pos, Name, S);
    std::vector<Gdt::Block> Blocks;
    if (D->Convert)
    {
        if (!D->Convert(*this, M, Pos, Name, Blocks, S)) return std::string();
    }
    else
    {
        Gdt::Block B{ Name, D->Gdf, "", Fields(M, Pos, S) };
        Blocks.push_back(std::move(B));
    }
    std::string Out;
    for (auto& B : Blocks)
    {
        // the type's constants (configstringFileType...), unless the block holds the key
        for (const auto& [K, V] : D->Constants)
            if (std::none_of(B.Keys.begin(), B.Keys.end(), [&](const auto& P) { return P.first == K; })) B.Keys.emplace_back(K, V);
        S.Entries += B.Keys.size();
        Out += Text(B, true);
    }
    return Out;
}

// ---- a porting type: the source title's fields into the target's entries (Artorias's rules)

std::string Type::Data::Block(const Memory& M, int64_t Pos, const std::string& Name, Stats& S) const
{
    std::map<std::string, Value> Values;
    std::set<const Layout*> Seen;
    Read(M, Layouts[Root], Pos, Values, Seen, S);
    // a name-only header (every field but its name 0): another zone defines the asset
    bool Holds = false;
    for (const Field& F : Layouts[Root].Fields)
        if (F.Offset != 0)
            if (const auto It = Values.find(F.Name); It != Values.end() && !It->second.Enum)
            {
                const Value& V = It->second;
                Holds = Holds || std::any_of(V.Numbers.begin(), V.Numbers.end(), [](double X) { return X != 0; }) ||
                        std::any_of(V.Texts.begin(), V.Texts.end(), [](const std::string& X) { return !X.empty(); });
            }
    if (!Holds) return std::string();

    std::map<std::string, std::string> Set;
    // The entry's text for element I of a value, "" when it does not fill it: a 0 or an empty name
    // is a value the asset does not carry (the entry keeps its default) unless its enum names it.
    const auto Text = [&](const Value& V, size_t I, const std::string& Target) -> std::string {
        const auto It = Entries.find(Target);
        if (It == Entries.end() || I >= V.Size()) return std::string();
        const TargetEntry& E = It->second;
        const bool Numeric = E.Kind == "Float" || E.Kind == "Int" || E.Kind == "CheckBox";
        std::string Out;
        if (V.Text)
        {
            if (Numeric) return std::string();
            Out = V.Texts[I];
        }
        else
        {
            double X = V.Numbers[I];
            if (!std::isfinite(X) || X == 0) return std::string();
            if (E.Kind == "CheckBox") return "1";
            if (Milliseconds && V.Integral && E.Kind == "Float" && IsTime(Target)) X /= 1000.0;
            if (E.Ranged && (X < E.Low || X > E.High))
            {
                S.Clamped[Target]++;
                X = std::clamp(X, E.Low, E.High);
            }
            Out = PortNumber(X, E.Kind == "Int" || (V.Integral && E.Kind != "Float" && !E.Kind.starts_with("Vector")));
        }
        if (Out.empty()) return Out;
        if (!E.Choices.empty())
        {
            for (const auto& C : E.Choices)
                if (Lower(C) == Lower(Out)) return C;
            S.NotAChoice[Target]++;
            return std::string();
        }
        for (char& C : Out)
            if (C == '"') C = '\'';
        return Out;
    };
    const auto Assign = [&](const std::string& Target, const std::string& T) {
        if (T.empty() || Set.count(Target)) return false;
        Set[Target] = T;
        return true;
    };
    const auto Spread = [&](const Value& V, const std::vector<std::string>& Targets) {
        for (size_t I = 0; I < Targets.size(); I++) Assign(Targets[I], Text(V, I, Targets[I]));
    };
    const auto CurveOf = [&](const std::string& Field) -> const std::vector<std::string>* {
        for (const auto& [K, Targets] : Curves)
            if (K == Field) return &Targets;
        return nullptr;
    };

    // the fields, in the order of preference; an array spreads over its curve, a slotted one
    // gives its slot, any other array is not written
    std::set<std::string> Curved;
    for (const auto& [Field, Targets] : Fields)
    {
        const auto It = Values.find(Field);
        if (It == Values.end()) continue;
        const Value& V = It->second;
        if (const auto* Curve = CurveOf(Field); Curve && V.Size() > 1)
        {
            Curved.insert(Field);
            Spread(V, *Curve);
            continue;
        }
        size_t I = 0;
        if (const auto Slot = Slots.find(Field); Slot != Slots.end()) I = Slot->second;
        else if (V.Size() != 1) continue;
        for (const auto& Target : Targets) Assign(Target, Text(V, I, Target));
    }
    for (const auto& [Field, Targets] : Curves)
        if (!Curved.count(Field))
            if (const auto It = Values.find(Field); It != Values.end()) Spread(It->second, Targets);

    // the animations: each slot's purpose, the lowest slot first, into the entries holding it
    for (const auto& [Field, Slots] : Animations)
    {
        const auto It = Values.find(Field);
        if (It == Values.end() || !It->second.Text) continue;
        for (const auto& [Slot, Purpose] : Slots)
        {
            if (Slot < 0 || (size_t)Slot >= It->second.Texts.size() || It->second.Texts[(size_t)Slot].empty()) continue;
            const auto P = Purposes.find(Purpose);
            if (P == Purposes.end()) continue;
            for (const auto& Target : P->second)
                if (Entries.count(Target) && Assign(Target, It->second.Texts[(size_t)Slot])) S.Animations++;
        }
    }
    for (const auto& [Target, T] : PortConstants)
        if (Entries.count(Target)) Assign(Target, T);
    for (const auto& I : Implies)
        if (const auto From = Set.find(I.From); From != Set.end())
            if (const auto V = I.Values.find(From->second); V != I.Values.end()) Assign(I.Entry, V->second);

    Gdt::Fields Fields;
    for (const auto& K : Order)
    {
        const auto It = Set.find(K);
        std::string V;
        if (It != Set.end())
        {
            V = It->second;
            S.Entries++;
        }
        else if (const auto E = Entries.find(K); E != Entries.end()) V = E->second.Default;
        Fields.emplace_back(K, std::move(V));
    }
    return Gdt::Entry(Name, Asset, Fields);
}
}
