#include "stdafx.h"

// ZonekitZone's script bundles kind (ZonekitZone.h, ExportsScriptBundles): each scriptbundle asset
// of a zone as JSON, its key/value tree walked as the game's loader lays it out.

#include "ZonekitZone.h"

#include "ExportNaming.h"
#include "ExportWriters.h"

#include <nlohmann/json.hpp>

#include <format>
#include <functional>
#include <set>

namespace fs = std::filesystem;

namespace
{
    using Json = nlohmann::ordered_json;
    using V = BundleValue;
    using ExportNaming::Shortest;

    const char* KindName(V K)
    {
        switch (K)
        {
        case V::Bool: return "bool";
        case V::Int: return "int";
        case V::Float: return "float";
        case V::Localize: return "localize";
        case V::String: return "string";
        case V::Struct: return "struct";
        case V::KeyedStruct: return "keyed_struct";
        case V::Array: return "array";
        case V::Anim: return "anim";
        case V::Hash: return "hash";
        case V::Asset: return "asset";
        default: return "raw";
        }
    }
}

void ZonekitZone::ExportsScriptBundles(const ScriptBundleLayout& L, std::function<std::string(const std::string&)> Decrypt)
{
    ExportsBundleTrees(DiskGame::Kind::ScriptBundles, { { "scriptbundle", L.Table, L.TypeHash } }, L, Decrypt);
}

void ZonekitZone::ExportsBundleTrees(DiskGame::Kind K, std::vector<BundleOwner> Owners, const ScriptBundleLayout& L,
                                     std::function<std::string(const std::string&)> Decrypt)
{
    Exports(K, [this, K, Owners, L, Decrypt](const DiskGame::Context& C, DiskGame::Totals& T) {
        auto& Kt = T.Kinds[K];
        constexpr uint64_t IdBits = 0x7FFFFFFFFFFFFFFFull;
        const auto& Img = Image();
        const auto In = [&](int64_t P, int64_t N) { return P >= 0 && P + N <= (int64_t)Img.size(); };
        const auto Hex = [](uint64_t H) { return std::format("{:016x}", H); };
        // the texts the zone's bundles hold (strings, names, keys): what may name a settings type
        std::set<std::string> Texts;
        const auto Named = [this, &Hex, &Texts](uint64_t H) {
            const std::string N = KnownHash(H);
            if (!N.empty()) Texts.insert(N);
            return N.empty() ? Hex(H) : N;
        };
        const auto KindOf = [&L](uint8_t Code) { return Code < L.Types.size() ? L.Types[Code] : V::Raw; };
        size_t Fields = 0, KeysNamed = 0;
        // a string record (the text's pointer first), through the game's cipher when it has one
        const auto Text = [&](int64_t Record) {
            if (!In(Record, 8)) return std::string();
            std::string S = TextAt(Record);
            if (Decrypt && !S.empty()) S = Decrypt(S);
            if (!S.empty()) Texts.insert(S);
            return S;
        };
        std::function<Json(int64_t, int)> Table;
        // the value of Kind in the 8 bytes at Slot (an entry's value, an array's element)
        std::function<Json(V, int64_t, int, Json&)> Value = [&](V K, int64_t Slot, int Depth, Json& Field) -> Json {
            const uint64_t U = At<uint64_t>(Img, Slot);
            switch (K)
            {
            case V::Bool: return At<uint32_t>(Img, Slot) != 0;
            case V::Int: return At<int32_t>(Img, Slot);
            case V::Float: return Shortest(At<float>(Img, Slot));
            case V::Localize:
            case V::Hash:
            case V::Asset:
                if (U == 0) return nullptr;
                Field["value_hash"] = Hex(U & (K == V::Asset ? IdBits : ~0ull));
                return Named(K == V::Asset ? U & IdBits : U);
            case V::String: return U == 0 ? Json(nullptr) : Json(Text(Off(U)));
            case V::Anim:
            {
                // {its name's string record, ...}
                const int64_t To = Off(U);
                return To < 0 ? Json(nullptr) : Json(Text(Off(PointerAt(To))));
            }
            case V::Struct:
            case V::KeyedStruct:
            {
                const int64_t To = Off(U);
                return To < 0 || Depth > 32 ? Json::array() : Table(To, Depth + 1);
            }
            case V::Array:
            {
                // its elements, of one type (8 bytes each) or typed one by one (16-byte entries); the
                // lookup index the game builds over them is left out
                const int64_t To = Off(U);
                Json Items = Json::array();
                if (To < 0 || Depth > 32) return Items;
                const uint32_t N = At<uint32_t>(Img, To + L.ArrayCount);
                const int64_t Elems = Off(PointerAt(To + L.ArrayElements));
                const bool Read = N != 0 && Elems >= 0 && In(Elems, (int64_t)N * L.ArrayStride);
                if (L.ArrayType < 0 && !Read) return Items;
                const V Elem = KindOf(At<uint8_t>(Img, L.ArrayType >= 0 ? To + L.ArrayType : Elems));
                Field["element_type"] = KindName(Elem);
                if (!Read) return Items;
                for (uint32_t J = 0; J < N; J++)
                {
                    const int64_t E = Elems + (int64_t)J * L.ArrayStride;
                    Json Ignored;
                    if (L.ArrayType >= 0) Items.push_back(Value(Elem, E, Depth + 1, Ignored));
                    else Items.push_back(Value(KindOf(At<uint8_t>(Img, E)), E + 8, Depth + 1, Ignored));
                }
                return Items;
            }
            default: return Hex(U);
            }
        };
        // a key/value table: u32 count at Count, the keys' FNV-1a 64 (sorted) at Keys, the script
        // identifier hashes at ScriptHashes, the entries {u8 type, value at 8} at Entries
        Table = [&](int64_t P, int Depth) -> Json {
            Json Out = Json::array();
            const uint32_t Count = At<uint32_t>(Img, P + L.Count);
            const int64_t Keys = Off(PointerAt(P + L.Keys)), Script = Off(PointerAt(P + L.ScriptHashes)), Entries = Off(PointerAt(P + L.Entries));
            if (Count == 0 || Keys < 0 || Entries < 0 || !In(Keys, (int64_t)Count * 8) || !In(Entries, (int64_t)Count * 0x10)) return Out;
            for (uint32_t K = 0; K < Count; K++)
            {
                const uint64_t Key = At<uint64_t>(Img, Keys + (int64_t)K * 8);
                const uint64_t ScriptHash = Script >= 0 && In(Script, (int64_t)Count * 8) ? At<uint64_t>(Img, Script + (int64_t)K * 8) : 0;
                const int64_t E = Entries + (int64_t)K * 0x10;
                const uint8_t Code = At<uint8_t>(Img, E);
                const V Kd = KindOf(Code);
                const std::string KeyName = KnownHash(Key);
                Fields++;
                if (!KeyName.empty()) KeysNamed++, Texts.insert(KeyName);
                Json F;
                F["key"] = KeyName.empty() ? Hex(Key) : KeyName;
                F["key_hash"] = Hex(Key);
                if (ScriptHash != 0) F["script_hash"] = std::format("{:x}", ScriptHash);
                F["type"] = Kd == V::Raw ? std::format("raw{}", Code) : KindName(Kd);
                Json Extra = Json::object();
                Json Val = Value(Kd, E + 8, Depth, Extra);
                if (Extra.contains("element_type")) F["element_type"] = Extra["element_type"];
                F["value"] = std::move(Val);
                if (Extra.contains("value_hash")) F["value_hash"] = Extra["value_hash"];
                Out.push_back(std::move(F));
            }
            return Out;
        };
        const fs::path Dir = fs::path(C.KindDir(K)) / ExportNaming::SafeFileName(ZoneName);
        std::set<uint64_t> Done;
        // the zone's bundles, written once their settings types are named (from all their texts)
        struct Pending
        {
            Json B;
            fs::path File;
            std::string Id, Name, Type, AssetType;
            uint64_t TypeHash = 0;
        };
        std::vector<Pending> Out;
        for (const auto& I : R.Instances)
        {
            if (I.Parent >= 0) continue;
            const BundleOwner* O = nullptr;
            for (const auto& Each : Owners)
                if (OwnerIs(I, Each.Type.c_str())) O = &Each;
            if (O == nullptr) continue;
            const uint64_t Id = At<uint64_t>(Img, I.Pos) & IdBits;
            const int64_t Tab = I.Pos + O->Table;
            // an asset the zone only names keeps no table
            if (Off(PointerAt(Tab + L.Entries)) < 0 && At<uint32_t>(Img, Tab + L.Count) != 0)
            {
                Kt.References++;
                continue;
            }
            if (!Done.insert(Id).second) continue;
            std::string Name = KnownHash(Id);
            // its strings; a name not indexed proven by its hash: NameHash of one of its strings or
            // of the zone's name
            Json Strings = Json::object();
            std::vector<std::string> Candidates = { fs::path(FastFilePath()).stem().string() };
            for (const auto& [What, Field] : O->Strings)
            {
                const std::string S = PointerAt(I.Pos + Field) != 0 ? Text(I.Pos + Field) : std::string();
                Strings[What] = S.empty() ? Json(nullptr) : Json(S);
                if (!S.empty()) Candidates.push_back(S);
            }
            if (Name.empty() && O->NameHash && Candidates.size() > 1)
            {
                // and its first string followed by a word of the others ("zm_sat_locust" and
                // "..._locust_lighthouse_bg" give zm_sat_locust_lighthouse)
                const std::string First = Candidates[1];
                const std::vector<std::string> Others(Candidates.begin() + 2, Candidates.end());
                for (const auto& S : Others)
                    for (size_t B = 0, E; B <= S.size(); B = E + 1)
                    {
                        E = S.find_first_of("_/. ", B);
                        if (E == std::string::npos) E = S.size();
                        if (E > B) Candidates.push_back(First + "_" + S.substr(B, E - B));
                    }
                for (const auto& Cand : Candidates)
                    if (O->NameHash(Cand) == Id)
                    {
                        Name = Cand;
                        break;
                    }
            }
            if (!C.Wants(K, Name.empty() ? Hex(Id) : Name)) continue;
            Kt.Seen++;
            Json B;
            B["id"] = Hex(Id);
            B["name"] = Name;
            if (Owners.size() > 1) B["asset_type"] = O->Type;
            // the settings type: its hash, its name when one is proven (filled below)
            uint64_t Ty = 0;
            if (O->TypeHash >= 0)
                if (Ty = At<uint64_t>(Img, I.Pos + O->TypeHash) & IdBits; Ty != 0) B["type"] = Hex(Ty), B["type_name"] = nullptr;
            for (const auto& [What, Field] : O->Hashes)
            {
                // an asset of this zone (linked: its hash at 0) or of another (its hash alone)
                const uint64_t U = PointerAt(I.Pos + Field);
                const int64_t To = U != 0 ? Off(U) : -1;
                const uint64_t H = (To >= 0 && In(To, 8) ? At<uint64_t>(Img, To) : At<uint64_t>(Img, I.Pos + Field)) & IdBits;
                B[What] = H == 0 ? Json(nullptr) : Json(Named(H));
                if (H != 0) B[What + "_hash"] = Hex(H);
            }
            if (!Strings.empty()) B["strings"] = std::move(Strings);
            B["fields"] = Table(Tab, 0);
            if (!Name.empty()) Texts.insert(Name);
            Out.push_back({ std::move(B), Dir / (ExportNaming::SafeFileName(Name.empty() ? Hex(Id) : Name) + ".json"), Hex(Id), Name,
                            Ty != 0 ? Hex(Ty) : std::string(), O->Type, Ty });
        }
        // a settings type's name: the one an index gives, or a text of the zone's bundles, the asset
        // type names, whose hash (L.TypeNameHash) is the type's
        std::map<uint64_t, std::string> TypeNames;
        std::set<uint64_t> Types;
        for (const auto& P : Out)
            if (P.TypeHash != 0) Types.insert(P.TypeHash);
        if (!Types.empty() && L.TypeNameHash)
        {
            for (const uint64_t H : Types)
                if (const std::string N = KnownHash(H); !N.empty() && L.TypeNameHash(N) == H) TypeNames.emplace(H, N);
            // a text, and the type a name "type:name" starts with
            const auto Try = [&](const std::string& S) {
                for (const std::string& N : { S, S.substr(0, S.find(':')) })
                    if (const uint64_t H = L.TypeNameHash(N); !N.empty() && Types.count(H)) TypeNames.try_emplace(H, N);
            };
            for (const auto& S : Texts) Try(S);
            if (Loaded != nullptr)
                for (const auto& S : Loaded->Profile.TypeNames) Try(S);
        }
        size_t TypesNamed = 0;
        for (auto& P : Out)
        {
            if (P.TypeHash != 0)
                if (const auto It = TypeNames.find(P.TypeHash); It != TypeNames.end()) P.B["type_name"] = It->second, TypesNamed++;
            const std::string S = P.B.dump(1, '\t', false, nlohmann::json::error_handler_t::replace) + "\n";
            const std::string What = P.Name.empty() ? P.Id : P.Name;
            if (!ExportWriters::WriteBytes(P.File, (const uint8_t*)S.data(), S.size()))
            {
                T.Fail(ZoneName, K, What, "write failed");
                continue;
            }
            Kt.Written++;
            const auto It = TypeNames.find(P.TypeHash);
            T.Index(K, P.Id, P.Type, P.TypeHash == 0 ? P.AssetType : It != TypeNames.end() ? It->second : std::string(), ZoneName, P.Name,
                    P.File.string());
        }
        Counter("bundle_types_named") += TypesNamed;
        Counter("bundle_fields") += Fields;
        Counter("bundle_keys_named") += KeysNamed;
    });
}
