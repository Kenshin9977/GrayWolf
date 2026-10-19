#include "stdafx.h"

// ZonekitZone's struct dumps kind (ZonekitZone.h, ExportsStructDumps): assets whose fields are not
// named yet, each as JSON by offset, the blocks its pointers name followed as the walk read them.

#include "ZonekitZone.h"

#include "ExportNaming.h"
#include "ExportWriters.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <map>
#include <functional>
#include <set>
#include <unordered_map>

namespace fs = std::filesystem;

namespace
{
    using Json = nlohmann::ordered_json;
    using ExportNaming::Shortest;

    // A 4-byte word as what it most plausibly holds: 0; a float of a usual magnitude (about 1e-7 to
    // 3e7, either sign: written with its decimal point); a small integer (within +-2^20); anything
    // else (flags, packed bytes, half a hash) as hex text
    Json Word(uint32_t W)
    {
        if (W == 0) return 0;
        const uint32_t Exponent = (W >> 23) & 0xFF;
        if (Exponent >= 0x68 && Exponent <= 0x98)
        {
            float F;
            std::memcpy(&F, &W, 4);
            return Shortest(F);
        }
        const int32_t I = (int32_t)W;
        if (I >= -0x100000 && I <= 0xFFFFF) return I;
        return std::format("0x{:08x}", W);
    }
}

void ZonekitZone::ExportsStructDumps(DiskGame::Kind K, std::vector<DumpType> Types, std::function<std::string(const std::string&)> Decrypt)
{
    Exports(K, [this, K, Types, Decrypt](const DiskGame::Context& C, DiskGame::Totals& T) {
        auto& Kt = T.Kinds[K];
        constexpr uint64_t IdBits = 0x7FFFFFFFFFFFFFFFull;
        constexpr size_t WordBudget = 4u << 20;   // words per asset, its blocks included
        constexpr int MaxDepth = 24;
        const auto& Img = Image();
        const auto Hex = [](uint64_t H) { return std::format("{:016x}", H); };
        const auto TypeOf = [this](const ZoneTables::ZoneResult::Instance& I) -> std::string {
            return I.Owner < R.OwnerNames.size() ? R.OwnerNames[I.Owner] : std::string("?");
        };
        std::map<std::string, int64_t> Wanted;   // type -> NameAt
        std::map<std::string, int64_t> IdAtOf;   // type -> where its name hash is
        for (const auto& D : Types) Wanted[D.Type] = D.NameAt, IdAtOf[D.Type] = D.IdAt;
        const auto IdOf = [&](const ZoneTables::ZoneResult::Instance& I) {
            const auto It = IdAtOf.find(TypeOf(I));
            return At<uint64_t>(Img, I.Pos + (It != IdAtOf.end() ? It->second : 0)) & IdBits;
        };
        std::vector<size_t> Targets;
        for (size_t I = 0; I < R.Instances.size(); I++)
            if (Wanted.count(TypeOf(R.Instances[I])) != 0) Targets.push_back(I);
        if (Targets.empty()) return;

        // every read by position, the largest first (a string's size is negative): what a pointer
        // names, and how much of it
        std::vector<std::pair<int64_t, int64_t>> Reads;
        Reads.reserve(R.ReadPos.size());
        for (size_t I = 0; I < R.ReadPos.size() && I < R.Reads.size(); I++)
            if (R.ReadPos[I] >= 0 && R.Reads[I] != 0) Reads.emplace_back(R.ReadPos[I], R.Reads[I]);
        std::sort(Reads.begin(), Reads.end(), [](const auto& A, const auto& B) { return A.first != B.first ? A.first < B.first : A.second > B.second; });
        // the asset instances by position (the outermost first), the pointers the walk left unlinked
        std::unordered_map<int64_t, size_t> InstanceAt;
        for (size_t I = 0; I < R.Instances.size(); I++) InstanceAt.try_emplace(R.Instances[I].Pos, I);
        std::unordered_map<int64_t, uint64_t> Unlinked;
        for (const auto& [Field, Value] : R.BackRefs) Unlinked.try_emplace(Field, Value);

        const auto Named = [this, &Hex](uint64_t Id, Json& J) {
            J["id"] = Hex(Id);
            const std::string N = KnownHash(Id);
            if (!N.empty()) J["name"] = N;
        };
        // a string read: its text, through the game's cipher
        const auto Text = [&](int64_t Pos, int64_t Max) {
            std::string S;
            for (int64_t B = 0; B < Max && Pos + B < (int64_t)Img.size() && Img[(size_t)(Pos + B)] != 0; B++) S.push_back((char)Img[(size_t)(Pos + B)]);
            return Decrypt && !S.empty() ? Decrypt(S) : S;
        };

        // the size of the read starting at To (the largest; negative: a string), else of the rest of
        // the read To falls in (In: where that read starts), 0 when no read holds it
        const auto ReadAt = [&](int64_t To, int64_t& In) -> int64_t {
            In = -1;
            const auto Hit = std::lower_bound(Reads.begin(), Reads.end(), To, [](const auto& A, int64_t V) { return A.first < V; });
            if (Hit != Reads.end() && Hit->first == To) return Hit->second;
            for (auto Back = Hit; Back != Reads.begin() && Hit - Back < 64;)
            {
                --Back;
                if (Back->second > 0 && Back->first + Back->second > To)
                {
                    In = Back->first;
                    return Back->first + Back->second - To;
                }
            }
            return 0;
        };

        // each asset's name: by its hash, else the string its NameAt field points to
        std::unordered_map<size_t, std::pair<std::string, std::string>> NameOf;   // name, where from
        for (const size_t Index : Targets)
        {
            const int64_t Pos = R.Instances[Index].Pos;
            auto& [Name, From] = NameOf[Index];
            Name = KnownHash(IdOf(R.Instances[Index])), From = "hash";
            const int64_t NameAt = Wanted[TypeOf(R.Instances[Index])];
            const uint64_t V = NameAt >= 0 ? At<uint64_t>(Img, Pos + NameAt) : 0;
            int64_t In = -1, Size = 0;
            if (const int64_t To = V != 0 ? Off(V) : -1; Name.empty() && To >= 0 && (Size = ReadAt(To, In)) < 0 && In < 0)
                Name = Text(To, -Size), From = std::format("string at 0x{:x}", NameAt);
            if (Name.empty()) From.clear();
        }
        // who points to each (their reads scanned), and what each points to, in its reads' order
        std::unordered_map<size_t, std::vector<size_t>> ReferencedBy, Refers;
        for (const size_t Index : Targets)
        {
            const auto& I = R.Instances[Index];
            for (uint32_t Rd = I.FirstRead; Rd < I.EndRead && Rd < R.Reads.size() && Rd < R.ReadPos.size(); Rd++)
                for (int64_t B = 0; R.ReadPos[Rd] >= 0 && B + 8 <= R.Reads[Rd]; B += 8)
                {
                    const uint64_t V = At<uint64_t>(Img, R.ReadPos[Rd] + B);
                    const auto It = V != 0 ? InstanceAt.find(Off(V)) : InstanceAt.end();
                    if (It == InstanceAt.end() || It->second == Index || !NameOf.count(It->second)) continue;
                    auto& By = ReferencedBy[It->second];
                    if (std::find(By.begin(), By.end(), Index) == By.end()) By.push_back(Index), Refers[Index].push_back(It->second);
                }
        }
        // an unnamed asset only one named asset points to: by that one's name, #n when it points to
        // several of its type (the n-th in its reads)
        for (const size_t Index : Targets)
        {
            auto& [Name, From] = NameOf[Index];
            const auto& By = ReferencedBy[Index];
            if (!Name.empty() || By.size() != 1 || NameOf[By[0]].first.empty() || NameOf[By[0]].second.rfind("referenced", 0) == 0) continue;
            size_t Rank = 0, Count = 0;
            for (const size_t Other : Refers[By[0]])
                if (R.Instances[Other].Owner == R.Instances[Index].Owner)
                {
                    Count++;
                    if (Other == Index) Rank = Count;
                }
            Name = NameOf[By[0]].first + (Count > 1 ? std::format("#{}", Rank) : std::string());
            From = "referenced by " + TypeOf(R.Instances[By[0]]);
        }

        // one asset's dump state
        size_t Root = 0, Words = 0;
        const std::map<std::string, DumpField>* FieldNames = nullptr;   // the asset's type's
        std::map<std::string, const std::map<std::string, DumpField>*> NamesOf;
        for (const auto& D : Types) NamesOf[D.Type] = &D.Fields;
        std::unordered_map<int64_t, std::string> Seen;   // block position -> its first path
        std::function<Json(int64_t, int64_t, const std::string&, int)> Block;
        const auto Pointer = [&](int64_t To, const std::string& Path, int Depth) -> Json {
            Json J;
            if (To == R.Instances[Root].Pos) return Json{ { "self", true } };
            if (const auto It = InstanceAt.find(To); It != InstanceAt.end())
            {
                const auto& I = R.Instances[It->second];
                J["asset"] = TypeOf(I);
                Named(IdOf(I), J);
                if (const auto N = NameOf.find(It->second); !J.contains("name") && N != NameOf.end() && !N->second.first.empty())
                    J["name"] = N->second.first;
                return J;
            }
            int64_t In = -1;
            const int64_t Size = ReadAt(To, In);
            if (Size < 0) return Json{ { "string", Text(To, -Size) } };
            if (Size == 0) return Json{ { "zone_offset", std::format("0x{:x}", To) } };
            J["block"] = std::format("0x{:x}", Size);
            if (In >= 0) J["read_starts_before"] = std::format("0x{:x}", To - In);
            if (const auto S = Seen.find(To); S != Seen.end())
            {
                J["same_as"] = S->second;
                return J;
            }
            if (Depth >= MaxDepth || Words + (size_t)Size / 4 > WordBudget)
            {
                J["truncated"] = true;
                return J;
            }
            Seen.emplace(To, Path);
            J["fields"] = Block(To, Size, Path, Depth + 1);
            return J;
        };
        Block = [&](int64_t P, int64_t Size, const std::string& Path, int Depth) -> Json {
            Json Out = Json::object();
            if (P < 0 || P + Size > (int64_t)Img.size()) return Out;
            // (appended without the ordered map's search for the key: offsets are unique)
            struct Appender
            {
                Json::object_t& Map;
                std::string Key;
                Appender& operator=(Json V)
                {
                    Map.emplace_back(Key, std::move(V));
                    return *this;
                }
            };
            auto& Map = Out.get_ref<Json::object_t&>();
            const auto F = [&Map](const std::string& Key) { return Appender{ Map, Key }; };
            const int Width = Size > 0x10000 ? 6 : 4;
            int64_t O = 0;
            while (O + 4 <= Size)
            {
                const std::string Key = std::format("0x{:0{}x}", O, Width);
                const std::string Here = Path.empty() ? Key : Path + "/" + Key;
                const auto Named_ = FieldNames != nullptr ? FieldNames->find(Here) : std::map<std::string, DumpField>::const_iterator();
                const bool HasName = FieldNames != nullptr && Named_ != FieldNames->end();
                const std::string Shown = HasName ? Key + " " + Named_->second.Name : Key;
                Words++;
                if (HasName && (Named_->second.Kind == "i32" || Named_->second.Kind == "f32" || Named_->second.Kind == "u16x2"))
                {
                    if (Named_->second.Kind == "i32") F(Shown) = At<int32_t>(Img, P + O);
                    else if (Named_->second.Kind == "f32") F(Shown) = Shortest(At<float>(Img, P + O));
                    // two u16 (the name "low|high", "_" for one not named)
                    else F(Shown) = Json::array({ At<uint16_t>(Img, P + O), At<uint16_t>(Img, P + O + 2) });
                    O += 4;
                    continue;
                }
                if (O % 8 == 0 && O + 8 <= Size)
                {
                    const int64_t Field = P + O;
                    const uint64_t V = At<uint64_t>(Img, Field);
                    if (Field == R.Instances[Root].Pos)
                    {
                        // the asset's own hash
                        Json J;
                        Named(V & IdBits, J);
                        F(Shown) = std::move(J);
                        O += 8;
                        continue;
                    }
                    const int64_t To = V != 0 ? Off(V) : -1;
                    if (To >= 0)
                    {
                        F(Shown) = Pointer(To, Here, Depth);
                        O += 8;
                        continue;
                    }
                    if (const auto U = Unlinked.find(Field); V != 0 && U != Unlinked.end())
                    {
                        // an asset of another zone (its hash, the type its chain reads), or data by a key
                        // no read of this zone files
                        Json J;
                        const auto Type = R.RefTypes.find(Field);
                        J["ref"] = Type != R.RefTypes.end() && Type->second < R.OwnerNames.size() ? R.OwnerNames[Type->second] : std::string("?");
                        Named(V & IdBits, J);
                        F(Shown) = std::move(J);
                        O += 8;
                        continue;
                    }
                    // a hash a name index knows (both halves set: not two small numbers)
                    if ((V >> 32) != 0 && (uint32_t)V != 0 && (V & IdBits) >= (1ull << 40))
                        if (const std::string N = KnownHash(V & IdBits); !N.empty())
                        {
                            F(Shown) = Json{ { "hash", Hex(V & IdBits) }, { "name", N } };
                            O += 8;
                            continue;
                        }
                }
                F(Shown) = Word(At<uint32_t>(Img, P + O));
                O += 4;
            }
            if (O < Size)
            {
                std::string Tail;
                for (; O < Size; O++) Tail += std::format("{:02x}", Img[(size_t)(P + O)]);
                F("tail") = Tail;
            }
            return Out;
        };

        const fs::path Dir = fs::path(C.KindDir(K)) / ExportNaming::SafeFileName(ZoneName);
        std::set<std::pair<uint16_t, uint64_t>> Done;
        std::map<std::string, uint64_t> FileOf;   // a file written, by the id it holds
        for (const size_t Index : Targets)
        {
            const auto& I = R.Instances[Index];
            const std::string Type = TypeOf(I);
            const uint64_t Id = IdOf(I);
            if (!Done.insert({ I.Owner, Id != 0 ? Id : (uint64_t)I.Pos }).second) continue;
            // its struct: the largest read at its position among its own
            int64_t Size = 0, Bytes = 0;
            for (uint32_t Rd = I.FirstRead; Rd < I.EndRead && Rd < R.Reads.size() && Rd < R.ReadPos.size(); Rd++)
            {
                Bytes += R.Reads[Rd] < 0 ? -R.Reads[Rd] : R.Reads[Rd];
                if (R.ReadPos[Rd] == I.Pos) Size = std::max(Size, R.Reads[Rd]);
            }
            if (Size <= 0)
            {
                Kt.References++;
                continue;
            }
            const auto& [Name, NameFrom] = NameOf[Index];
            // (an asset without a hash: by its place in the zone)
            const uint64_t Key = Id != 0 ? Id : (uint64_t)I.Pos;
            const std::string File = !Name.empty() ? Name : Id != 0 ? Hex(Id) : std::format("at_{:x}", I.Pos);
            if (!C.Wants(K, File) && !C.Wants(K, Hex(Id))) continue;
            Root = Index, Words = 0;
            FieldNames = NamesOf[Type];
            Seen.clear();
            Seen.emplace(I.Pos, "");
            Json Fields = Block(I.Pos, Size, "", 0);
            Kt.Seen++;
            Json A;
            A["type"] = Type;
            A["id"] = Hex(Id);
            A["name"] = Name;
            if (!NameFrom.empty()) A["name_from"] = NameFrom;
            if (const auto By = ReferencedBy.find(Index); By != ReferencedBy.end())
            {
                Json List = Json::array();
                for (size_t J = 0; J < By->second.size() && J < 32; J++)
                {
                    const size_t Other = By->second[J];
                    const std::string& N = NameOf[Other].first;
                    List.push_back(TypeOf(R.Instances[Other]) + " " + (N.empty() ? Hex(IdOf(R.Instances[Other])) : N));
                }
                if (By->second.size() > 32) List.push_back(std::format("... {} in all", By->second.size()));
                A["referenced_by"] = std::move(List);
            }
            A["zone"] = ZoneName;
            if (I.Parent >= 0) A["inside"] = TypeOf(R.Instances[(size_t)I.Parent]);
            A["size"] = std::format("0x{:x}", Size);
            A["bytes"] = Bytes;
            A["fields"] = std::move(Fields);
            Counter(std::string(DiskGame::KindName(K)) + "_" + Type) += 1;
            const std::string S = A.dump(1, '\t', false, nlohmann::json::error_handler_t::replace) + "\n";
            std::string Stem = ExportNaming::SafeFileName(File);
            const auto Have = FileOf.find(Type + "/" + Stem);
            if (Have != FileOf.end() && Have->second != Key) Stem += "_" + (Id != 0 ? Hex(Id) : std::format("at_{:x}", I.Pos));
            FileOf.emplace(Type + "/" + Stem, Key);
            const fs::path Path = Dir / Type / (Stem + ".json");
            if (ExportWriters::WriteBytes(Path, (const uint8_t*)S.data(), S.size()))
            {
                Kt.Written++;
                // (no settings type: the asset type stands as its type's name)
                T.Index(K, Hex(Id), "", Type, ZoneName, Name, Path.string());
            }
            else
                T.Fail(ZoneName, K, File, "write failed");
        }
    });
}
