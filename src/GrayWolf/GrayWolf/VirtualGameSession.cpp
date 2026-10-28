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
    : Lock(std::move(Taken)), Bytes(std::move(Image)), Ptr(PointerSize == 4 ? 4 : 8), Origin_(PointerSize == 4 ? Base32 : Base)
{
    SavedGame = CoDAssets::Game;
    SavedInstance = std::move(CoDAssets::GameInstance);
    SavedStrings = CoDAssets::GameStringHandler;
    SavedImages = CoDAssets::GameXImageHandler;

    CoDAssets::Game = &Game;
    CoDAssets::GameStringHandler = H.Strings;
    if (H.Images != nullptr) CoDAssets::GameXImageHandler = H.Images;
    CoDAssets::GameInstance = std::make_unique<ProcessReader>();
    CoDAssets::GameInstance->AttachVirtual(Bytes.data(), Bytes.size(), Origin_);
}

VirtualGameSession::~VirtualGameSession()
{
    CoDAssets::GameInstance = std::move(SavedInstance);
    CoDAssets::Game = SavedGame;
    CoDAssets::GameStringHandler = SavedStrings;
    CoDAssets::GameXImageHandler = SavedImages;
}

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
