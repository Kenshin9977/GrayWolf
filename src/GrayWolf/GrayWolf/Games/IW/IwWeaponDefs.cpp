#include "stdafx.h"

// Weapon files from a game's weapon field layout (IwWeaponDefs.h).

#include "Games/IW/IwWeaponDefs.h"
#include "Games/IW/IWZone.h"

#include "ExportNaming.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <set>

namespace
{
    using ExportNaming::At;
    using WeaponFiles::Clean;

    // a hit location's weapon file field: torso_upper -> locTorsoUpper
    std::string LocField(const std::string& Location)
    {
        std::string F = "loc";
        bool Up = true;
        for (const char C : Location)
        {
            if (C == '_')
            {
                Up = true;
                continue;
            }
            F += Up ? (char)std::toupper((unsigned char)C) : C;
            Up = false;
        }
        return F;
    }

    enum class Unit : uint8_t { Int, Ms, Float, Enum, Hits };
    Unit UnitOf(const std::string& Name)
    {
        if (Name == "weaponType" || Name == "weaponClass" || Name == "fireType") return Unit::Enum;
        if (Name == "locationDamageMultipliers") return Unit::Hits;
        if (Name.find("Range") != std::string::npos || Name.ends_with("Scale")) return Unit::Float;
        if (Name.ends_with("Time") || Name.ends_with("TimeAkimbo")) return Unit::Ms;
        return Unit::Int;
    }
}

namespace IwWeaponDefs
{
std::vector<WeaponFiles::Weapon> Weapons(const Layout& L, const IwWeaponStats::ZoneView& V)
{
    const auto& Img = *V.Image;
    const auto& R = *V.Walk;
    const auto Fits = [&](int64_t P, int64_t N) { return P >= 0 && P + N <= (int64_t)Img.size(); };
    std::vector<WeaponFiles::Weapon> Out;
    std::set<std::string> Done;
    // an animation package's times (its u16 timers behind AnimTimers; none without them), through Put;
    // 0xffff is a time the package has not (the getter's override search skips it): left out
    const auto AnimTimes = [&](int64_t Pkg, const auto& Put) {
        const uint64_t TimersPtr = Pkg >= 0 && Fits(Pkg + L.AnimTimers, 8) ? At<uint64_t>(Img, Pkg + L.AnimTimers) : 0;
        const int64_t Timers = TimersPtr != 0 ? V.Off(TimersPtr) : -1;
        for (const auto& [Field, Offset] : L.AnimTimes)
            if (Timers >= 0 && Fits(Timers + Offset, 2))
                if (const uint16_t Ms = At<uint16_t>(Img, Timers + Offset); Ms != 0xffff) Put(Field, std::format("{}", Ms / 1000.0));
    };
    for (const auto& I : R.Instances)
    {
        if (I.Owner >= R.OwnerNames.size() || R.OwnerNames[I.Owner] != "weapon") continue;
        const int64_t P = I.Pos;
        int64_t Def = P;
        if (L.Def >= 0)
        {
            if (!Fits(P + L.Def, 8)) continue;
            const uint64_t Ptr = At<uint64_t>(Img, P + L.Def);
            Def = Ptr != 0 ? V.Off(Ptr) : -1;
        }
        if (Def < 0) continue;
        const std::string Name = V.Text(P + L.Name);
        // (a name-only header ",name" is a reference to another zone's weapon)
        if (Name.empty() || Name[0] == ',' || !Done.insert(Name).second) continue;
        std::string Text = "WEAPONFILE";
        const auto Put = [&](const std::string& K, const std::string& Value) { Text += "\\" + K + "\\" + Clean(Value); };
        if (L.Display >= 0)
            if (const std::string D = V.Text(P + L.Display); !D.empty()) Put("displayName", D);
        // a middle damage stop (damage2-4) whose range is 0: not used (Infinite Warfare's), or holding up
        // to the next stop's range, the next damage unused (ZeroRangeExtends, WWII's)
        std::set<std::string> Unused;
        std::map<std::string, int64_t> RangeFrom;   // a range read from another field's place
        const auto FieldAt = [&](const std::string& Name) -> int64_t {
            for (const auto& F : L.Fields)
                if (F.Name == Name && F.At >= 0) return (F.InDef ? Def : P) + F.At;
            return -1;
        };
        for (const auto& F : L.Fields)
            if (F.Name.starts_with("damageRange") && F.Name.size() == 12)
                if (const int64_t A = (F.InDef ? Def : P) + F.At; F.At >= 0 && Fits(A, 4) && At<float>(Img, A) == 0.0f)
                {
                    if (!L.ZeroRangeExtends)
                    {
                        Unused.insert(F.Name), Unused.insert("damage" + F.Name.substr(11));
                        continue;
                    }
                    const std::string Next = std::to_string(F.Name[11] - '0' + 1);
                    if (const int64_t N = FieldAt("damageRange" + Next); N >= 0)
                        RangeFrom[F.Name] = N, Unused.insert("damageRange" + Next), Unused.insert("damage" + Next);
                }
        std::string Offsets;   // WWII's rules: each field written, at its WeaponDef offset
        const auto ClipSize = [&]() -> int32_t {
            const int64_t A = FieldAt("clipSize");
            return A >= 0 && Fits(A, 4) ? At<int32_t>(Img, A) : 0;
        };
        for (const auto& F : L.Fields)
        {
            const Unit How = UnitOf(F.Name);
            int64_t A = (F.InDef ? Def : P) + F.At;
            if (const auto It = RangeFrom.find(F.Name); It != RangeFrom.end()) A = It->second;
            if (F.At < 0 || !Fits(A, How == Unit::Hits ? 8 : 4) || Unused.count(F.Name)) continue;
            if (F.InDef && How != Unit::Hits) Offsets += std::format("{} 0x{:x};", F.Name, A - Def);
            if (F.Byte)
            {
                Put(F.Name, std::to_string(At<uint8_t>(Img, A)));
                continue;
            }
            switch (How)
            {
            case Unit::Int:
            {
                int32_t V = At<int32_t>(Img, A);
                if (const int64_t B = (F.InDef ? Def : P) + F.TimesClip; V == 0 && F.TimesClip >= 0 && Fits(B, 4)) V = At<int32_t>(Img, B) * ClipSize();
                Put(F.Name, std::to_string(V));
                break;
            }
            case Unit::Ms: Put(F.Name, std::format("{}", At<int32_t>(Img, A) / 1000.0)); break;
            case Unit::Float:
                if (const float X = At<float>(Img, A); std::isfinite(X)) Put(F.Name, std::format("{}", X));
                break;
            case Unit::Enum:
            {
                const auto& Names = F.Name == "weaponType" ? L.Types : F.Name == "weaponClass" ? L.Classes : L.FireTypes;
                const int32_t E = At<int32_t>(Img, A);
                Put(F.Name, E >= 0 && E < (int32_t)Names.size() ? Names[(size_t)E] : std::to_string(E));
                break;
            }
            case Unit::Hits:
            {
                const uint64_t Ptr = L.HitsInline ? 0 : At<uint64_t>(Img, A);
                const int64_t H = L.HitsInline ? A : Ptr != 0 ? V.Off(Ptr) : -1;
                if (!Fits(H, 4 * (int64_t)L.HitLocations.size())) break;
                for (size_t K = 1; K < L.HitLocations.size(); K++)
                    if (const float X = At<float>(Img, H + 4 * (int64_t)K); std::isfinite(X)) Put(LocField(L.HitLocations[K]), std::format("{}", X));
                break;
            }
            }
        }
        // the animation package: its name (joined to the package file of the zone defining it), and its
        // times when this zone defines it
        if (L.AnimPkg >= 0 && Fits(Def + L.AnimPkg, 8))
        {
            const uint64_t PkgPtr = At<uint64_t>(Img, Def + L.AnimPkg);
            const int64_t Pkg = PkgPtr != 0 ? V.Off(PkgPtr) : -1;
            if (Pkg >= 0)
            {
                std::string PkgName = V.Text(Pkg);
                if (!PkgName.empty() && PkgName[0] == ',') PkgName.erase(0, 1);
                if (!PkgName.empty()) Put(WeaponFiles::AnimPackageField, PkgName);
            }
            AnimTimes(Pkg, Put);
        }
        if (const auto& At_ = L.Attachments; At_.List >= 0)
        {
            // (lists ';' separated: a weapon file value holds no line break) its attachments (the u8 count, the pointers), defaults (list indices), reload animation
            // overrides (kind, attachment 1, attachment 2, state; time at 0x18, alternate time at 0x1c)
            const int Count = Fits(Def + At_.Count, 1) ? At<uint8_t>(Img, Def + At_.Count) : 0;
            const uint64_t ListPtr = Fits(Def + At_.List, 8) ? At<uint64_t>(Img, Def + At_.List) : 0;
            const int64_t List = ListPtr != 0 ? V.Off(ListPtr) : -1;
            std::string Names;
            for (int K = 0; List >= 0 && K < Count && Fits(List + 8 * K, 8); K++)
            {
                const uint64_t P_ = At<uint64_t>(Img, List + 8 * K);
                const int64_t A = P_ != 0 ? V.Off(P_) : -1;
                std::string N = A >= 0 ? V.Text(A) : std::string();
                if (!N.empty() && N[0] == ',') N.erase(0, 1);
                Names += (Names.empty() ? "" : ";") + (N.empty() ? std::string("?") : N);
            }
            if (!Names.empty()) Put("attachments", Names);
            std::string Defaults;
            for (int K = 0; K < At_.DefaultCount && Fits(Def + At_.Defaults + K, 1); K++)
                Defaults += (K ? " " : "") + std::to_string(At<uint8_t>(Img, Def + At_.Defaults + K));
            Put("s2.defaults", Defaults);
            const int Anims = Fits(Def + At_.AnimCount, 1) ? At<uint8_t>(Img, Def + At_.AnimCount) : 0;
            const uint64_t AnimPtr = Fits(Def + At_.Anims, 8) ? At<uint64_t>(Img, Def + At_.Anims) : 0;
            const int64_t Anim = AnimPtr != 0 ? V.Off(AnimPtr) : -1;
            std::string Lines;
            for (int K = 0; Anim >= 0 && K < Anims && Fits(Anim + 32 * K, 32); K++)
            {
                const int64_t E = Anim + 32 * K;
                const int State = At<uint8_t>(Img, E + 3);
                if (std::find(At_.AnimStates.begin(), At_.AnimStates.end(), State) == At_.AnimStates.end()) continue;
                Lines += std::format("{} {} {} {} {} {};", At<uint8_t>(Img, E), At<uint8_t>(Img, E + 1), At<uint8_t>(Img, E + 2), State,
                                     At<int32_t>(Img, E + 0x18), At<int32_t>(Img, E + 0x1c));
            }
            Put("s2.animOverrides", Lines);
            Put("s2.offsets", Offsets);
        }
        if (!L.OffsetsField.empty()) Put(L.OffsetsField, Offsets);
        Out.push_back({ Name, Text, {} });
    }
    // WWII's attachment assets, as attachment files: the overrides of the layout's fields (an i32
    // field set (operation 3) by an i32, every other value an f32)
    if (const auto& At_ = L.Attachments; At_.List >= 0)
    {
        std::map<int64_t, Unit> Known;
        for (const auto& F : L.Fields)
            if (F.InDef && F.At >= 0 && !F.Byte) Known[F.At] = UnitOf(F.Name);
        for (const auto& I : R.Instances)
        {
            if (I.Owner >= R.OwnerNames.size() || R.OwnerNames[I.Owner] != "attachment") continue;
            const int64_t P = I.Pos;
            const std::string Name = V.Text(P);
            if (Name.empty() || Name[0] == ',' || !Done.insert("\x02" + Name).second || !Fits(P + At_.AltB, 1)) continue;
            std::string Text = "ATTACHMENTFILE\\s2.format\\1";
            const auto Put = [&](const std::string& K, const std::string& Value) { Text += "\\" + K + "\\" + Clean(Value); };
            if (const std::string D = V.Text(P + At_.DisplayName); !D.empty()) Put("displayName", D);
            Put("attachmentType", std::to_string(At<int32_t>(Img, P + At_.Type)));
            Put("s2.alt", At<int32_t>(Img, P + At_.AltA) != 0 || At<uint8_t>(Img, P + At_.AltB) != 0 ? "1" : "0");
            const int32_t N = At<int32_t>(Img, P + At_.EntryCount);
            const uint64_t OffPtr = At<uint64_t>(Img, P + At_.Offsets), EntPtr = At<uint64_t>(Img, P + At_.Entries);
            const int64_t Offs = OffPtr != 0 ? V.Off(OffPtr) : -1, Ents = EntPtr != 0 ? V.Off(EntPtr) : -1;
            std::string Lines;
            for (int32_t K = 0; Offs >= 0 && Ents >= 0 && K < N && K < 4096 && Fits(Offs + 2 * K, 2) && Fits(Ents + 16 * K, 16); K++)
            {
                const uint16_t Field = At<uint16_t>(Img, Offs + 2 * K);
                const auto It = Known.find(Field);
                if (It == Known.end()) continue;
                const uint8_t Op = At<uint8_t>(Img, Ents + 16 * K + 2);
                const bool Int = It->second != Unit::Float;
                const std::string Value = Op == 3 && Int ? std::to_string(At<int32_t>(Img, Ents + 16 * K + 8))
                                                         : std::format("{}", At<float>(Img, Ents + 16 * K + 8));
                Lines += std::format("0x{:x} {} {};", Field, Op, Value);
            }
            Put("s2.overrides", Lines);
            Out.push_back({ Name, Text, {} });
        }
    }
    // the animation packages the zone defines, as package files (ANIMPACKAGE\<field>\<value>...)
    if (!L.AnimPkgType.empty())
        for (const auto& I : R.Instances)
        {
            if (I.Owner >= R.OwnerNames.size() || R.OwnerNames[I.Owner] != L.AnimPkgType) continue;
            const std::string Name = V.Text(I.Pos);
            if (Name.empty() || Name[0] == ',' || !Done.insert("\x01" + Name).second) continue;
            std::string Text = "ANIMPACKAGE";
            const auto Put = [&](const std::string& K, const std::string& Value) { Text += "\\" + K + "\\" + Clean(Value); };
            AnimTimes(I.Pos, Put);
            if (Text.size() > 11) Out.push_back({ Name, Text, {} });
        }
    return Out;
}

WeaponFiles::Notes GameNotes(const std::string& Title, const std::string& LanguageZone, const std::string& Checked, const std::string& Where,
                             const std::string& Known)
{
    WeaponFiles::Notes N;
    N.Title = Title;
    N.Names = "The display names are the localized strings of the language's zones (" + LanguageZone +
              "...):\n"
              "export one into the same --out. A weapon a patch zone holds is read from it (the game loads it after\n"
              "the zone it patches); zones lists every zone holding the weapon.\n";
    N.Source = Title + " keeps no weapon field table in its exe (the mod tools' weapon files are not shipped).\n"
               "Each weapon is read from " + Where + ",\n"
               "whose field names are the weapon file's (zonetool, which dumps and rebuilds these zones, writes\n"
               "them by these names), and written back as the weapon file they make. Each field was checked on\n" +
               Checked + "'s weapons against the game's known numbers (" + Known + ").\n"
               "weaponType, weaponClass, fireType and the hit locations are named by the exe's own name tables.\n"
               "Times are milliseconds in the asset (written as the weapon file's seconds and read back),\n"
               "distances inches. Fields not checked are left out (their columns stay empty).\n";
    return N;
}

void Export(const Layout& L, const IwWeaponStats::ZoneView& V, const std::vector<std::pair<std::string, std::string>>& Strings,
            const std::string& Zone, const DiskGame::Context& C, DiskGame::Totals& T, const std::map<std::string, std::string>& Tables)
{
    WeaponFiles::ExportStats(Weapons(L, V), Strings, Zone, C, T, L.Notes, Tables);
}

std::map<std::string, ZonekitZone::DumpField> DumpFields(const Layout& L)
{
    std::map<std::string, ZonekitZone::DumpField> F;
    const auto Key = [](int64_t At) { return std::format("0x{:04x}", At); };
    const std::string Def = L.Def >= 0 ? Key(L.Def) + "/" : std::string();
    F[Key(L.Name)] = { "name", "" };
    if (L.Display >= 0) F[Key(L.Display)] = { "displayName", "" };
    if (L.Def >= 0) F[Key(L.Def)] = { "weapDef", "" };
    for (const auto& Fd : L.Fields)
    {
        if (Fd.At < 0 || Fd.Byte) continue;   // (a dump names whole dwords: a one-byte flag is not one)
        const std::string P = (Fd.InDef ? Def : std::string()) + Key(Fd.At);
        const Unit How = UnitOf(Fd.Name);
        if (How == Unit::Hits && L.HitsInline)
        {
            // the array itself: each hit location's f32
            for (size_t K = 0; K < L.HitLocations.size(); K++)
                F[(Fd.InDef ? Def : std::string()) + Key(Fd.At + 4 * (int64_t)K)] = { LocField(L.HitLocations[K]), "f32" };
            continue;
        }
        F[P] = { Fd.Name, How == Unit::Float ? "f32" : How == Unit::Hits ? "" : "i32" };
    }
    return F;
}
}

void IWZone::ExportsIwWeapons(const IwWeaponDefs::Layout& L)
{
    using namespace DiskGame;
    ExportsStructDumps(Kind::Weapons, { { "weapon", L.Name, IwWeaponDefs::DumpFields(L), -1 }, { "attachment", 0, {}, -1 } });
    Exports(Kind::WeaponStats, [this, L](const Context& C, Totals& T) {
        IwWeaponStats::ZoneView V;
        V.Image = &Image();
        V.Walk = &Walk();
        V.Off = [this](uint64_t A) { return Off(A); };
        V.Text = [this](int64_t Field) { return TextAt(Field); };
        std::vector<std::pair<std::string, std::string>> S;
        Strings(S);
        // the class builder's combination table, when the zone holds it (WWII's)
        std::map<std::string, std::string> Tables;
        if (!L.Attachments.ComboTable.empty())
            for (const auto& I : Walk().Instances)
            {
                if (I.Parent >= 0 || !OwnerIs(I, "stringtable") || ExportNaming::Lower(AssetName(Kind::Data, I.Pos)) != L.Attachments.ComboTable) continue;
                uint32_t Columns = 0, Rows = 0;
                std::vector<std::string> Cells;
                if (!StringTableCells(I.Pos, {}, Columns, Rows, Cells) || Cells.size() < (size_t)Columns * Rows) continue;
                std::string Csv;
                for (uint32_t R_ = 0; R_ < Rows; R_++)
                {
                    for (uint32_t K = 0; K < Columns; K++) Csv += (K ? "," : "") + Cells[(size_t)R_ * Columns + K];
                    Csv += "\n";
                }
                Tables[L.Attachments.ComboTable] = Csv;
            }
        IwWeaponDefs::Export(L, V, S, std::filesystem::path(FastFilePath()).stem().string(), C, T, Tables);
    });
}
