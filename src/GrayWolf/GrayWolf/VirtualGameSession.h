#pragma once

// Adapter between the offline zone readers and the live asset code (ReadXAnim, ReadXModel,
// the XAnim/XModel translators): a copy of the zone (ZoneMemory::Copy: copy-on-write when the
// zone's bytes are a ZoneBytes), its pointers relocated, is served as the
// game's memory through ProcessReader::AttachVirtual. The live code reads process-wide state
// (CoDAssets::Game, GameInstance and the string/image handlers), so a session swaps that
// state in, holds one process-wide lock for its lifetime, and restores everything on exit.

#include "CoDAssets.h"
#include "ZoneMemory.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class VirtualGameSession
{
public:
    // Far from any real address, so a relocated pointer can never be mistaken for one.
    static constexpr uintptr_t Base = 0x100000000000ull;
    // A 32-bit game's pointers hold 4 bytes: its zone is served below 4 GB (CoD4's biggest zone,
    // 181 MB, ends far below 0x80000000)
    static constexpr uintptr_t Base32 = 0x10000000ull;

    struct Handlers
    {
        LoadStringHandler Strings = nullptr;
        LoadXImageHandler Images = nullptr; // nullptr keeps the current handler
    };

    // PointerSize: the game's (8, or 4 for a 32-bit game: links are written 4 bytes wide, and the
    // zone is served at Base32)
    VirtualGameSession(const ICoDGame& Game, std::vector<uint8_t> Image, const Handlers& H, int PointerSize = 8);
    // The same, with the process-wide lock already taken (Acquire): what a zone does to prepare the
    // session's handlers runs under it too, while what comes before (decompressing and walking a
    // zone) runs on several threads at once (the install index's workers).
    VirtualGameSession(std::unique_lock<std::mutex> Taken, const ICoDGame& Game, std::vector<uint8_t> Image, const Handlers& H,
                       int PointerSize = 8);
    // The same over a copy made already (ZoneMemory::Copy of a zone's bytes, or one a parked session released)
    VirtualGameSession(std::unique_lock<std::mutex> Taken, const ICoDGame& Game, ZoneMemory::Copy Image, const Handlers& H,
                       int PointerSize = 8);
    // The process-wide lock every session holds for its lifetime, taken (waits for the session open now)
    static std::unique_lock<std::mutex> Acquire();
    ~VirtualGameSession();
    VirtualGameSession(const VirtualGameSession&) = delete;
    VirtualGameSession& operator=(const VirtualGameSession&) = delete;

    // The address the live code sees for a zone offset (a 64-bit game's session).
    static uint64_t Address(uint64_t Offset) { return Base + Offset; }
    // This session's: where its zone is served, and a zone offset's address there
    uint64_t Origin() const { return Origin_; }
    uint64_t Addr(uint64_t Offset) const { return Origin_ + Offset; }
    int PointerSize() const { return Ptr; }
    // What a string handler answers for script-string slot Index (bone and notetrack names are
    // slots): the zone's text there, or "bone_<Index>" when Slots is null or has none.
    static std::string SlotString(const std::vector<std::string>* Slots, uint64_t Index);
    // Points the pointer field at Field to the data at Target, both zone offsets.
    void Link(uint64_t Field, uint64_t Target);
    // Writes an address that is not in this zone (another zone served beside it, ProcessReader's
    // AddVirtual) into the pointer field at Field, a zone offset.
    void Poke(uint64_t Field, uint64_t Address);
    // The served bytes, relocations included.
    ByteView Image() const { return { Bytes.data(), Bytes.size() }; }
    // Hands them back to serve again later (a new session takes them); end this one right after.
    ZoneMemory::Copy Release() { return std::move(Bytes); }
    // True when V is an address inside the served image.
    bool Contains(uint64_t V) const { return V >= Origin_ && V < Origin_ + Bytes.size(); }

private:
    std::unique_lock<std::mutex> Lock;
    ZoneMemory::Copy Bytes;
    int Ptr = 8;
    uint64_t Origin_ = Base;
    const ICoDGame* SavedGame;
    std::unique_ptr<ProcessReader> SavedInstance;
    LoadStringHandler SavedStrings;
    LoadXImageHandler SavedImages;
};
