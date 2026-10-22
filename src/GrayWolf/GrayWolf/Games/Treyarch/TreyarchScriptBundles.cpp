#include "stdafx.h"

#include "Games/Treyarch/TreyarchScriptBundles.h"

#include "ExportNaming.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>

namespace
{
    using Json = nlohmann::ordered_json;
    using ExportNaming::At;

    // a value type's shape and the prefix acts writes it with
    enum class Shape { Str, Hash, Int, Float, Enum, Duration, Unknown };
    struct ValueType
    {
        Shape S;
        const char* Prefix;
    };
    // acts' bo4 SB_ValueType (0-30)
    const ValueType Bo4Types[] = {
        { Shape::Str, "" }, { Shape::Hash, "" }, { Shape::Int, "" }, { Shape::Float, "" }, { Shape::Hash, "anim" },
        { Shape::Hash, "playeranim" }, { Shape::Hash, "siegeanim" }, { Shape::Hash, "model" }, { Shape::Hash, "aitype" },
        { Shape::Hash, "character" }, { Shape::Hash, "fx" }, { Shape::Hash, "surface_fx_table" }, { Shape::Str, "rumble" },
        { Shape::Str, "scriptbundle" }, { Shape::Str, "xcam" }, { Shape::Unknown, "material" }, { Shape::Hash, "image" },
        { Shape::Hash, "localized" }, { Shape::Hash, "localized" }, { Shape::Unknown, "" }, { Shape::Hash, "weapon" },
        { Shape::Hash, "vehicle" }, { Shape::Enum, "enum" }, { Shape::Str, "streamerhint" }, { Shape::Str, "status_effect" },
        { Shape::Duration, "duration" }, { Shape::Hash, "objective" }, { Shape::Hash, "gesture" }, { Shape::Hash, "render_override_bundle" },
        { Shape::Str, "gesture_table" }, { Shape::Hash, "impact_fx_table" },
    };
    // acts' cw SB_ValueType (0-34): Black Ops 4's with a scriptbundle hash at 14 and three more
    const ValueType ColdWarTypes[] = {
        { Shape::Str, "" }, { Shape::Hash, "" }, { Shape::Int, "" }, { Shape::Float, "" }, { Shape::Hash, "anim" },
        { Shape::Hash, "playeranim" }, { Shape::Hash, "siegeanim" }, { Shape::Hash, "model" }, { Shape::Hash, "aitype" },
        { Shape::Hash, "character" }, { Shape::Hash, "fx" }, { Shape::Hash, "surface_fx_table" }, { Shape::Str, "rumble" },
        { Shape::Str, "scriptbundle" }, { Shape::Hash, "scriptbundle" }, { Shape::Str, "xcam" }, { Shape::Unknown, "material" },
        { Shape::Hash, "image" }, { Shape::Hash, "localized" }, { Shape::Hash, "localized" }, { Shape::Unknown, "" },
        { Shape::Hash, "weapon" }, { Shape::Hash, "vehicle" }, { Shape::Enum, "enum" }, { Shape::Str, "streamerhint" },
        { Shape::Str, "status_effect" }, { Shape::Duration, "duration" }, { Shape::Hash, "objective" }, { Shape::Hash, "gesture" },
        { Shape::Hash, "render_override_bundle" }, { Shape::Str, "gesture_table" }, { Shape::Hash, "impact_fx_table" },
        { Shape::Unknown, "" }, { Shape::Hash, "execution" }, { Shape::Hash, "vehicle_skin" },
    };

    struct Layout
    {
        int64_t Hash;                                   // an XHash's size
        int64_t BundleType, BundleObjects;              // in the bundle
        int64_t Entry, ValueHash, KeyScr, StringRef, TypeAt, Value;   // an entry
        int64_t Sub, SubCount, SubItems;                // a sub
        bool SubKeyIsString;                            // Black Ops 4: a script string at 0; Cold War: a hash at 0, a script hash at 8
        const ValueType* Types;
        size_t TypeCount;
    };
    const Layout Bo4{ 16, 0x10, 0x20, 0x30, 0x10, 0x20, 0x24, 0x28, 0x2c, 0x18, 8, 0x10, true, Bo4Types, std::size(Bo4Types) };
    const Layout ColdWar{ 8, 8, 0x10, 0x20, 8, 0x10, 0x14, 0x18, 0x1c, 0x20, 0x10, 0x18, false, ColdWarTypes, std::size(ColdWarTypes) };

    constexpr uint64_t MaxCount = 1u << 20;

    struct Reader
    {
        const Treyarch::BundleZone& Z;
        const Layout& L;
        const std::vector<uint8_t>& Img;
        const std::vector<std::pair<int64_t, int64_t>>& Reads;
        bool Bad = false;
        int64_t Last = -1;   // the last block found among the reads

        // where the pointer at Field leads: relocated, else (an inline block, -1) the next read of Size bytes
        int64_t Follow(int64_t Field, int64_t Size)
        {
            const uint64_t P = At<uint64_t>(Img, Field);
            if (const int64_t To = Z.Off(P); To >= 0) return Last = std::max(Last, To), To;
            for (const auto& [Pos, Bytes] : Reads)
                if (Pos > Last && Bytes == Size) return Last = Pos;
            return -1;
        }

        std::string HashText(uint64_t H) const
        {
            H &= Z.HashMask;
            const std::string N = H != 0 && Z.HashName ? Z.HashName(H) : std::string();
            return !N.empty() ? N : std::format("{:x}", (unsigned long long)H);
        }
        std::string Scr(uint32_t I) const { return Z.ScriptString ? Z.ScriptString(I) : std::string(); }

        // the objects array {u64 count 0, entries 8, u64 sub count 0x10, subs 0x18} at Pos
        Json Objects(int64_t Pos, int Depth)
        {
            Json Out = Json::object();
            if (Pos < 0 || Depth > 16)
            {
                Bad = true;
                return Out;
            }
            const uint64_t Count = At<uint64_t>(Img, Pos), Subs = At<uint64_t>(Img, Pos + 0x10);
            if (Count > MaxCount || Subs > MaxCount)
            {
                Bad = true;
                return Out;
            }
            const int64_t Entries = Count ? Follow(Pos + 8, (int64_t)Count * L.Entry) : 0, SubList = Subs ? Follow(Pos + 0x18, (int64_t)Subs * L.Sub) : 0;
            if (Entries < 0 || SubList < 0 ||
                (uint64_t)Entries + Count * L.Entry > Img.size() || (uint64_t)SubList + Subs * L.Sub > Img.size())
            {
                Bad = true;
                return Out;
            }
            for (uint64_t K = 0; K < Subs; K++)
            {
                const int64_t S = SubList + (int64_t)(K * L.Sub);
                const std::string Key = L.SubKeyIsString ? Scr(At<uint32_t>(Img, S))
                                                         : [&] {
                                                               const uint64_t H = At<uint64_t>(Img, S) & Z.HashMask;
                                                               const std::string N = H && Z.HashName ? Z.HashName(H) : std::string();
                                                               return !N.empty() ? N : std::format("#{:08x}", At<uint32_t>(Img, S + 8));
                                                           }();
                const uint64_t N = At<uint64_t>(Img, S + L.SubCount);
                const int64_t Items = N && N <= MaxCount ? Follow(S + L.SubItems, (int64_t)N * 0x20) : 0;
                Json List = Json::array();
                if (N > MaxCount || Items < 0) Bad = true;
                else
                    for (uint64_t E = 0; E < N; E++) List.push_back(Objects(Items + (int64_t)(E * 0x20), Depth + 1));
                Out[Key.empty() ? std::format("sub_{}", K) : Key] = std::move(List);
            }
            for (uint64_t K = 0; K < Count; K++)
            {
                const int64_t E = Entries + (int64_t)(K * L.Entry);
                const uint64_t KeyHash = At<uint64_t>(Img, E) & Z.HashMask;
                std::string Key = KeyHash && Z.HashName ? Z.HashName(KeyHash) : std::string();
                if (Key.empty()) Key = std::format("#{:08x}", At<uint32_t>(Img, E + L.KeyScr));
                const uint32_t Type = At<uint32_t>(Img, E + L.TypeAt), Ref = At<uint32_t>(Img, E + L.StringRef);
                const uint64_t Hash = At<uint64_t>(Img, E + L.ValueHash);
                const ValueType V = Type < L.TypeCount ? L.Types[Type] : ValueType{ Shape::Unknown, "" };
                const std::string Prefix = V.Prefix;
                Json Value;
                switch (V.S)
                {
                case Shape::Str: Value = Prefix.empty() ? Scr(Ref) : Prefix + "&" + Scr(Ref); break;
                case Shape::Hash: Value = Prefix + "#" + HashText(Hash); break;
                case Shape::Int: Value = At<int32_t>(Img, E + L.Value); break;
                case Shape::Float: Value = ExportNaming::Shortest(At<float>(Img, E + L.Value)); break;
                case Shape::Enum:
                case Shape::Duration: Value = Prefix + ":" + std::to_string(At<int32_t>(Img, E + L.Value)); break;
                default:
                    Value = Ref ? std::format("<unk{}>:{}", Type, Scr(Ref)) : std::format("<unk{}>:#{}", Type, HashText(Hash));
                    break;
                }
                Out[Key] = std::move(Value);
            }
            return Out;
        }
    };
}

bool Treyarch::ReadScriptBundle(const BundleZone& Z, BundleLayout Which, int64_t Pos, const std::vector<std::pair<int64_t, int64_t>>& Reads,
                                std::string& Type, std::string& Text)
{
    const Layout& L = Which == BundleLayout::Bo4 ? Bo4 : ColdWar;
    const auto& Img = *Z.Image;
    Text.clear();
    Reader R{ Z, L, Img, Reads };
    R.Last = Pos;
    const uint64_t TypeHash = At<uint64_t>(Img, Pos + L.BundleType) & Z.HashMask;
    Type = TypeHash == 0 ? std::string() : R.HashText(TypeHash);
    const int64_t Objects = Pos + L.BundleObjects;
    if (At<uint64_t>(Img, Objects) == 0 && At<uint64_t>(Img, Objects + 0x10) == 0) return true;
    Json J;
    J["name"] = R.HashText(At<uint64_t>(Img, Pos));
    J["type"] = Type;
    J["fields"] = R.Objects(Objects, 0);
    if (R.Bad) return false;
    Text = J.dump(1, '\t', false, nlohmann::json::error_handler_t::replace) + "\n";
    return true;
}
