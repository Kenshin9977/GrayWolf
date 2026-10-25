#pragma once

#include <cstdint>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

// A game's memory as its readers see it, served from buffers (a relocated zone and the data it
// streams from other files) at the addresses the readers expect. Reads outside every buffer give
// zeros.
class ProcessReader
{
private:
    const uint8_t* VirtualData = nullptr;
    size_t VirtualSize = 0;
    uintptr_t VirtualBase = 0;
    // further buffers served beside the image (AddVirtual)
    struct Region
    {
        const uint8_t* Data;
        size_t Size;
        uintptr_t Base;
    };
    std::vector<Region> Extra;
    // copies what the buffers hold of [Offset, Offset + Length) into Buffer; the bytes read
    size_t ReadVirtual(uint8_t* Buffer, uintptr_t Offset, uintptr_t Length) const;

public:
    // Serve reads from a buffer mapped at Base; the buffer must outlive the reader.
    void AttachVirtual(const uint8_t* Data, size_t Size, uintptr_t Base);
    // A further buffer served at Base beside the image; it must outlive the reader.
    void AddVirtual(const uint8_t* Data, size_t Size, uintptr_t Base);
    // The buffer served at Base replaced by this one (added when none is there yet)
    void SetVirtual(const uint8_t* Data, size_t Size, uintptr_t Base);

    // True once an image is attached.
    bool IsRunning() const;

    template <class T>
    T Read(uintptr_t Offset)
    {
        if (VirtualData != nullptr)
        {
            T ResultValue;
            std::memset(&ResultValue, 0, sizeof(ResultValue));
            ReadVirtual(reinterpret_cast<uint8_t*>(&ResultValue), Offset, sizeof(ResultValue));
            return ResultValue;
        }
#ifdef _DEBUG
        throw new std::exception("No image attached");
#else
        throw new std::exception("");
#endif
    }
    std::string ReadNullTerminatedString(uintptr_t Offset);

    int8_t* Read(uintptr_t Offset, uintptr_t Length, uintptr_t& Result);
    size_t Read(uint8_t* Buffer, uintptr_t Offset, uintptr_t Length);
};
