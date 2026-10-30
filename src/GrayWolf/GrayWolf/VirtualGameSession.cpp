#include "stdafx.h"

#include "VirtualGameSession.h"

#include "ProcessReader.h"

#include <cstring>

namespace
{
    std::mutex SessionLock;
}

std::string VirtualGameSession::SlotString(const std::vector<std::string>* Slots, uint64_t Index)
{
    if (Slots && Index < Slots->size() && !(*Slots)[Index].empty()) return (*Slots)[Index];
    return "bone_" + std::to_string(Index);
}

std::unique_lock<std::mutex> VirtualGameSession::Acquire()
{
    return std::unique_lock<std::mutex>(SessionLock);
}

VirtualGameSession::VirtualGameSession(const ICoDGame& Game, std::vector<uint8_t> Image, const Handlers& H, int PointerSize)
    : VirtualGameSession(Acquire(), Game, std::move(Image), H, PointerSize)
{
}

VirtualGameSession::VirtualGameSession(std::unique_lock<std::mutex> Taken, const ICoDGame& Game, std::vector<uint8_t> Image,
                                       const Handlers& H, int PointerSize)
    : VirtualGameSession(std::move(Taken), Game, ZoneMemory::Copy(std::move(Image)), H, PointerSize)
{
}

VirtualGameSession::VirtualGameSession(std::unique_lock<std::mutex> Taken, const ICoDGame& Game, ZoneMemory::Copy Image,
                                       const Handlers& H, int PointerSize)
    : Lock(std::move(Taken)), Bytes(std::move(Image)), Ptr(PointerSize == 4 ? 4 : 8), Origin_(PointerSize == 4 ? Base32 : Base),
      ServedGame(CoDAssets::Game, &Game), Strings(CoDAssets::GameStringHandler, H.Strings),
      Images(CoDAssets::GameXImageHandler, H.Images != nullptr ? H.Images : CoDAssets::GameXImageHandler),
      Instance(CoDAssets::GameInstance, Attached(Bytes, Origin_))
{
}

std::shared_ptr<ProcessReader> VirtualGameSession::Attached(ZoneMemory::Copy& Bytes, uint64_t Origin)
{
    auto Reader = std::make_shared<ProcessReader>();
    Reader->AttachVirtual(Bytes.data(), Bytes.size(), Origin);
    return Reader;
}

VirtualGameSession::~VirtualGameSession() = default;

void VirtualGameSession::Poke(uint64_t Field, uint64_t Address)
{
    if (Field + Ptr > Bytes.size()) return;
    std::memcpy(Bytes.data() + Field, &Address, (size_t)Ptr);
}

void VirtualGameSession::Link(uint64_t Field, uint64_t Target)
{
    if (Field + Ptr > Bytes.size()) return;
    const uint64_t Value = Addr(Target);
    std::memcpy(Bytes.data() + Field, &Value, (size_t)Ptr);
}
