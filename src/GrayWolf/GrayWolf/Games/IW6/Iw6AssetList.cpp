#include "stdafx.h"
#include "ByteView.h"

#include "Iw6AssetList.h"

#include <cstring>

namespace Iw6
{
    namespace
    {
        // "The data for this pointer follows here in the stream".
        constexpr uint64_t InlineMarker = 0xFDFDFDFFFFFFFFFFull;
        // A pointer to data already placed in the zone keeps this prefix and carries an offset.
        constexpr uint32_t ReferencePrefix = 0xFDFDFDF3u;

        // XFile { uint64 size; uint64 externalSize; uint64 blockSize[7] } then XAssetList
        // { stringCount, strings, assetCount, assets, globals }, every field pointer wide.
        constexpr uint64_t StringCountOffset = 0x48;
        constexpr uint64_t StringsPointerOffset = 0x50;
        constexpr uint64_t AssetCountOffset = 0x58;
        constexpr uint64_t StreamStart = 0x70;
        constexpr uint64_t EntrySize = 16;
        constexpr uint64_t GfxGlobalsSize = 104;

        const char* const TypeNames[AssetTypeCount] = {
            "physpreset", "phys_collmap", "xanim", "xmodelsurfs", "xmodel", "material",
            "computeshader", "vertexshader", "hullshader", "domainshader", "pixelshader",
            "vertexdecl", "techset", "image", "sound", "sndcurve", "lpfcurve", "reverbsendcurve",
            "loaded_sound", "col_map_sp", "com_map", "glass_map", "aipaths", "vehicle_track",
            "map_ents", "fx_map", "gfx_map", "lightdef", "ui_map", "font", "menufile", "menu",
            "animclass", "localize", "attachment", "weapon", "snddriverglobals", "fx", "impactfx",
            "surfacefx", "aitype", "mptype", "character", "xmodelalias", "rawfile", "scriptfile",
            "stringtable", "leaderboarddef", "structureddatadef", "tracer", "vehicle",
            "addon_map_ents", "netconststrings", "reverbpreset", "luafile", "scriptable",
            "colorization", "colorizationset", "tonemapping", "equipsndtable", "vectorfield",
            "dopplerpreset", "particlesimanimation"
        };

        template <typename T>
        bool ReadAt(ByteView Zone, uint64_t Offset, T& Value)
        {
            if (Offset > Zone.size() || Zone.size() - Offset < sizeof(T))
                return false;

            T Read{};
            std::memcpy(&Read, Zone.data() + Offset, sizeof(T));
            Value = Read;
            return true;
        }

        bool ReadU64(ByteView Zone, uint64_t Offset, uint64_t& Value) { return ReadAt(Zone, Offset, Value); }
        bool ReadU32(ByteView Zone, uint64_t Offset, uint32_t& Value) { return ReadAt(Zone, Offset, Value); }

        bool IsPointer(uint64_t Value)
        {
            return Value == 0 || Value == InlineMarker || (Value >> 32) == ReferencePrefix;
        }

        bool EntryParses(ByteView Zone, uint64_t Offset)
        {
            uint32_t Type = 0;
            uint32_t Padding = 0;
            uint64_t Header = 0;

            if (!ReadU32(Zone, Offset, Type) || !ReadU32(Zone, Offset + 4, Padding)
                || !ReadU64(Zone, Offset + 8, Header))
            {
                return false;
            }

            return Type < AssetTypeCount && Padding == 0 && IsPointer(Header);
        }

        bool ArrayParsesAt(ByteView Zone, uint64_t Offset, uint64_t Count)
        {
            if (Count > (Zone.size() - Offset) / EntrySize)
                return false;

            for (uint64_t Index = 0; Index < Count; ++Index)
            {
                if (!EntryParses(Zone, Offset + Index * EntrySize))
                    return false;
            }

            return true;
        }

        // Between the script strings and the asset array sits XGlobals, one pointer, and when it is
        // inline an XGfxGlobals and its arrays. Its size is computable, so the array's offset is
        // derived rather than searched for. The arrays of D3D state objects are written to the
        // runtime block, which puts no bytes in the file at all, so only these five are here.
        bool SkipGlobals(ByteView Zone, uint64_t& Position)
        {
            uint64_t GfxGlobals = 0;
            if (!ReadU64(Zone, Position, GfxGlobals))
                return false;

            Position += 8;
            if (GfxGlobals != InlineMarker)
                return true;

            const uint64_t Base = Position;
            uint32_t DepthStencilStates = 0;
            uint32_t BlendStates = 0;
            uint32_t PerPrim = 0;
            uint32_t PerObject = 0;
            uint32_t Stable = 0;
            if (!ReadU32(Zone, Base, DepthStencilStates) || !ReadU32(Zone, Base + 4, BlendStates)
                || !ReadU32(Zone, Base + 40, PerPrim) || !ReadU32(Zone, Base + 44, PerObject)
                || !ReadU32(Zone, Base + 48, Stable))
            {
                return false;
            }

            Position = Base + GfxGlobalsSize;

            const struct { uint64_t At; uint64_t Count; uint64_t Width; } Arrays[] = {
                { Base + 8, DepthStencilStates, 8 },
                { Base + 16, BlendStates, 4 },
                { Base + 56, PerPrim, 4 },
                { Base + 64, PerObject, 4 },
                { Base + 72, Stable, 4 },
            };

            for (const auto& Array : Arrays)
            {
                uint64_t Pointer = 0;
                if (!ReadU64(Zone, Array.At, Pointer))
                    return false;

                if (Pointer == InlineMarker)
                    Position += Array.Count * Array.Width;
            }

            return Position <= Zone.size();
        }

        bool ReadStrings(ByteView Zone, AssetList& Out, uint64_t& Position, std::string& Error)
        {
            uint64_t StringCount = 0;
            uint64_t StringsPointer = 0;
            if (!ReadU64(Zone, StringCountOffset, StringCount)
                || !ReadU64(Zone, StringsPointerOffset, StringsPointer))
            {
                Error = "the zone is too small to hold an asset list";
                return false;
            }

            Position = StreamStart;
            if (StringsPointer != InlineMarker || StringCount == 0)
                return true;

            // Only the slots holding the inline marker are followed by text; slot 0 is the empty
            // script string and is written null.
            const uint64_t Slots = StringCount;
            if (Slots > (Zone.size() - StreamStart) / 8)
            {
                Error = "the string table does not fit in the zone";
                return false;
            }

            uint64_t Text = StreamStart + Slots * 8;
            Out.Strings.reserve((size_t)Slots);

            for (uint64_t Index = 0; Index < Slots; ++Index)
            {
                uint64_t Slot = 0;
                if (!ReadU64(Zone, StreamStart + Index * 8, Slot))
                {
                    Error = "the string table runs past the end of the zone";
                    return false;
                }

                if (Slot != InlineMarker)
                    continue;

                uint64_t End = Text;
                while (End < Zone.size() && Zone[End] != 0)
                    ++End;

                if (End >= Zone.size())
                {
                    Error = "a script string is not terminated";
                    return false;
                }

                Out.Strings.emplace_back(reinterpret_cast<const char*>(Zone.data() + Text), (size_t)(End - Text));
                Text = End + 1;
            }

            Position = Text;
            return true;
        }
    } // namespace

    const char* AssetTypeName(const uint32_t Type)
    {
        return Type < AssetTypeCount ? TypeNames[Type] : nullptr;
    }

    bool ReadAssetList(ByteView Zone, AssetList& Out, std::string& Error)
    {
        Out = AssetList();

        if (Zone.size() < StreamStart)
        {
            Error = "the zone is too small to hold an asset list";
            return false;
        }

        uint64_t AssetCount = 0;
        if (!ReadU64(Zone, AssetCountOffset, AssetCount))
        {
            Error = "the zone is too small to hold an asset list";
            return false;
        }

        uint64_t Position = StreamStart;
        if (!ReadStrings(Zone, Out, Position, Error))
            return false;

        if (!SkipGlobals(Zone, Position))
        {
            Error = "the zone globals run past the end of the zone";
            return false;
        }

        Out.ArrayOffset = Position;
        if (AssetCount == 0)
            return true;

        // A corrupt count must not become a huge reservation before anything has been validated.
        if (AssetCount > Zone.size() / EntrySize)
        {
            Error = "the asset count is larger than the zone";
            return false;
        }

        // Everything above is derived, so a failure here means the layout is wrong, not that the
        // array is somewhere else. Say so rather than hunting for something that parses.
        if (!ArrayParsesAt(Zone, Out.ArrayOffset, AssetCount))
        {
            Error = "the asset array does not parse where the layout puts it";
            return false;
        }

        Out.Assets.reserve((size_t)AssetCount);
        for (uint64_t Index = 0; Index < AssetCount; ++Index)
        {
            const uint64_t At = Out.ArrayOffset + Index * EntrySize;

            ListedAsset Asset;
            uint32_t Type = 0;
            if (!ReadU32(Zone, At, Type) || !ReadU64(Zone, At + 8, Asset.Header))
            {
                Error = "the asset array runs past the end of the zone";
                return false;
            }

            Asset.Type = Type;
            Asset.IsReference = Asset.Header != InlineMarker;
            Out.Assets.push_back(Asset);
        }

        return true;
    }
}
