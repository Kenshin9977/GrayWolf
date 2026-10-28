#pragma once

#include "ByteView.h"

// Port: how one game's decompressed zone begins (header, string table, asset list). The walker
// (ZoneTableWalker) knows only this interface; each game supplies an adapter in
// Games/<game>/ZoneFormat.cpp that registers itself under the profile's "zone_format" name.
// A new game adds a file, it edits nothing here.

#include "ZoneTableWalker.h"

#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ZoneTables
{
    struct ZoneHead
    {
        struct Asset
        {
            int Type = 0;
            uint64_t Head = 0;    // the asset's header pointer: a marker when its data is inline
        };
        std::vector<Asset> Assets;
        int64_t BodyStart = 0;    // zone offset of the first asset's data
        // with a layout: the start block's cursor when the first asset loads (ZoneResult::Start4)
        int64_t Start = 0;
    };

    // Walks the named reader table at a zone offset and returns where it ends: a format reads a
    // prefix the tables describe (IW7's XGlobals) with them.
    using PrefixWalk = std::function<int64_t(const std::string& Table, int64_t Pos)>;

    class IZoneFormat
    {
    public:
        virtual ~IZoneFormat() = default;
        virtual const char* Name() const = 0;
        // Throws std::runtime_error on a malformed header.
        virtual ZoneHead Read(ByteView Buf, const StreamRules& Rules) const
        {
            (void)Buf, (void)Rules;
            throw std::runtime_error(std::string(Name()) + " reads its zone head with the tables");
        }
        // The walker calls this one; a format whose prefix is read by table overrides it.
        virtual ZoneHead Read(ByteView Buf, const StreamRules& Rules,
                              const PrefixWalk& Walk) const
        {
            (void)Walk;
            return Read(Buf, Rules);
        }
    };

    // nullptr when no adapter registered that name
    const IZoneFormat* FindZoneFormat(const std::string& Name);

    // A static instance of this in the adapter's file registers it before main().
    struct ZoneFormatRegistration
    {
        explicit ZoneFormatRegistration(const IZoneFormat& Format);
    };

    // Bounds-checked reads for header parsers.
    class HeaderReader
    {
    public:
        explicit HeaderReader(ByteView B) : Buf(B) {}
        uint64_t U64(int64_t At) const { return Get<uint64_t>(At); }
        int32_t I32(int64_t At) const { return Get<int32_t>(At); }
        uint32_t U32(int64_t At) const { return Get<uint32_t>(At); }
        uint8_t U8(int64_t At) const { return Get<uint8_t>(At); }
        // the offset just past the NUL ending the string at At
        int64_t SkipString(int64_t At) const
        {
            const void* End = At >= 0 && At < (int64_t)Buf.size()
                ? memchr(Buf.data() + At, 0, Buf.size() - (size_t)At) : nullptr;
            if (!End) throw std::runtime_error("unterminated zone string");
            return static_cast<int64_t>(static_cast<const uint8_t*>(End) - Buf.data()) + 1;
        }
        // Count u64 string slots at At, then the text of every slot equal to Marker: the offset
        // just past both
        int64_t SkipStringSlots(int64_t At, int64_t Count, uint64_t Marker) const
        {
            std::vector<uint64_t> Slots;
            for (int64_t K = 0; K < Count; K++) Slots.push_back(U64(At + K * 8));
            int64_t Pos = At + Count * 8;
            for (uint64_t Slot : Slots)
                if (Slot == Marker) Pos = SkipString(Pos);
            return Pos;
        }
        int64_t Size() const { return (int64_t)Buf.size(); }

    private:
        template <class T>
        T Get(int64_t At) const
        {
            if (At < 0 || At + (int64_t)sizeof(T) > (int64_t)Buf.size()) throw std::runtime_error("header past the end");
            T V{};
            memcpy(&V, Buf.data() + At, sizeof(T));
            return V;
        }
        ByteView Buf;
    };
}
