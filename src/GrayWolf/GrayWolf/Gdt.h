#pragma once

#include "GdtWriter.h"   // Gdt::Entry, Gdt::Document

// The gdt kind (DiskGame::Kind::Gdt): a zone's assets written as Black Ops 3 Mod Tools GDT entries.
// Every asset type is a folder of data, zonekit/profiles/<profile>/gdt/<type>/: type.jsonc, its
// structs' layouts (*_fields.jsonc) and, to port a later title's type to BO3, porting.jsonc and the
// BO3 target it fills. What a table cannot say is a converter: a C++ function registered by name
// (GDT_CONVERTER) in Games/<engine>/Gdt/, which type.jsonc names. No code names a game or a type:
// a type is added with its folder of tables.
//
// Black Ops III's own types follow HydraX (github.com/Scobalula/HydraX, GPL-3.0, by Scobalula);
// here they are read from the relocated zone.
// The porting tables of BO4 and Cold War weapons come from Artorias, the user's earlier tool.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Gdt
{
    // What a zone gives the reader: its relocated image and how to name what a field points at.
    struct Memory
    {
        const std::vector<uint8_t>* Image = nullptr;
        std::function<int64_t(uint64_t Address)> Off;                                // zone offset, -1 outside
        std::function<std::string(const std::string& Kind, int64_t Pos)> AssetName;  // a layout kind's asset at Pos
        std::function<std::string(uint64_t Hash)> HashName;                          // "" when not known
        std::function<std::string(uint64_t Address)> String;                         // a string pointer's text
        std::function<std::string(uint32_t Index)> ScriptString;                     // the zone's script string
        std::function<std::string(uint32_t Hash)> AliasName;                         // a sound alias by hash, ""
        uint64_t HashMask = ~0ull;                                                   // a hash field's bits

        // helpers for converters: a value at a zone offset, where a pointer field leads (-1: nowhere
        // in the zone), the string or the asset name (its name pointer at NameAt, a "," reference's
        // name without it) a pointer field names
        template <class T>
        T At(int64_t Pos) const
        {
            T V{};
            if (Pos >= 0 && Pos + (int64_t)sizeof(T) <= (int64_t)Image->size()) std::memcpy(&V, Image->data() + Pos, sizeof(T));
            return V;
        }
        int64_t Follow(int64_t Field) const { return Field < 0 ? -1 : Off(At<uint64_t>(Field)); }
        std::string StringField(int64_t Field) const { return Field < 0 ? std::string() : String(At<uint64_t>(Field)); }
        std::string AssetField(int64_t Field, int64_t NameAt = 0) const;
    };

    // What the conversion did, for the run's counters (gdt_*).
    struct Stats
    {
        size_t Entries = 0;        // filled from the asset (the rest keep the target's default)
        size_t Animations = 0;     // of which animation entries
        // by entry: a number outside the entry's range, written at its bound; a value the entry's
        // choices do not hold, left at its default
        std::map<std::string, size_t> Clamped, NotAChoice;
        size_t Hashes = 0, HashesNamed = 0;   // hash fields read, named from the index
        std::map<std::string, size_t> Counters;   // anything else, gdt_<name>
        // Files a converter writes beside the .gdt (HydraX's animtables/, behavior/...): the path
        // under the gdt kind's folder ('/'-separated, relative) and the bytes. The gdt kind writes
        // and empties them after each asset; an asset giving files and no block is still written.
        std::vector<std::pair<std::string, std::string>> Files;
    };

    // One GDT entry: "Name" ( "Gdf" ), or derived from another, "Name" [ "Parent" ]; its keys and
    // values in the order written.
    struct Block
    {
        std::string Name, Gdf, Parent;
        std::vector<std::pair<std::string, std::string>> Keys;
    };
    // A block's text, CRLF as the Mod Tools write them; Sorted: the keys in the Mod Tools' order
    // (by name, case ignored)
    std::string Text(const Block& B, bool Sorted);
    // a .gdt file of blocks' texts
    // numbers as the GDT writes them: a whole number, else the shortest float text without exponent
    std::string Number(double X, bool Whole);

    class Type
    {
    public:
        // A type's folder (zonekit/profiles/<profile>/gdt/<type>); Profiles: the profiles folder (a
        // porting type's target is there); TypeNames: the profile's [type id] = zone type, which a
        // layout's pool names. nullptr and Error when it does not read.
        static std::shared_ptr<const Type> Load(const std::string& Dir, const std::string& Profiles,
                                                const std::vector<std::string>& TypeNames, std::string& Error);
        ~Type();
        const std::string& Id() const;        // the folder's name
        const std::string& ZoneType() const;  // the zone-level assets it reads (the root layout's pool)
        // the asset at Pos: its name ("" unnamed; a string name keeps its "," when a reference), and
        // its GDT text, one block or more ("" for a name-only header another zone defines)
        std::string Name(const Memory& M, int64_t Pos) const;
        std::string Convert(const Memory& M, int64_t Pos, const std::string& Name, Stats& S) const;

        // For converters. Fields: every field of the layouts read at Pos, as the GDT text of its key,
        // the layouts' order; Settings: type.jsonc; Folder: the type's folder, for its own data files.
        std::vector<std::pair<std::string, std::string>> Fields(const Memory& M, int64_t Pos, Stats& S) const;
        const nlohmann::ordered_json& Settings() const;
        const std::string& Folder() const;

        // Where else its assets are (type.jsonc): "nested", also those loaded inline in another
        // asset; "found_in", those another type's assets point at, an array of Count pointers at
        // Pointers in each of them
        struct Source
        {
            std::string ZoneType;
            int64_t Pointers = 0;
            int Count = 0;
        };
        bool Nested() const;
        const std::vector<Source>& FoundIn() const;

        // Its layouts' fields as the tables name them, by the zone type each layout's pool names (a
        // struct dump names its fields with them: ZonekitZone::ExportsStructDumps). Source: "engine"
        // (a name the game's own field list or the earlier title's matching weapons give), "observed"
        struct NamedField
        {
            int64_t Offset = 0;
            std::string Name, Form, Source;
            int Count = 0;
        };
        std::vector<std::pair<std::string, std::vector<NamedField>>> NamedFields() const;
        // Every field of the layouts read at Pos (the structs its address fields lead to included),
        // by name, as read: numbers (an array's every element) or names (a hash's, an enum's)
        struct FieldValue
        {
            std::vector<double> Numbers;
            std::vector<std::string> Texts;
        };
        std::map<std::string, FieldValue> Values(const Memory& M, int64_t Pos, Stats& S) const;

        struct Data;

    private:
        Type() = default;
        std::unique_ptr<Data> D;
    };

    // A converter writes an asset's blocks itself (Out); false: the asset is not one it writes.
    using Converter = bool (*)(const Type& T, const Memory& M, int64_t Pos, const std::string& Name, std::vector<Block>& Out,
                               Stats& S);
    bool Register(const char* Name, Converter Fn);
    Converter Find(const std::string& Name);
}

// In a converter's .cpp (Games/<engine>/Gdt/<type>.cpp): GDT_CONVERTER(t7_weapon, Fn) registers Fn
// under "t7_weapon", the name type.jsonc's "converter" gives.
#define GDT_CONVERTER(Id, Fn) static const bool GdtConverter_##Id = ::Gdt::Register(#Id, Fn)
