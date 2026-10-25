#include "stdafx.h"

#include "ProcessReader.h"

void ProcessReader::AttachVirtual(const uint8_t* Data, size_t Size, uintptr_t Base)
{
    VirtualData = Data;
    VirtualSize = Size;
    VirtualBase = Base;
}

void ProcessReader::AddVirtual(const uint8_t* Data, size_t Size, uintptr_t Base)
{
    Extra.push_back({ Data, Size, Base });
}

void ProcessReader::SetVirtual(const uint8_t* Data, size_t Size, uintptr_t Base)
{
    for (auto& R : Extra)
        if (R.Base == Base)
        {
            R.Data = Data, R.Size = Size;
            return;
        }
    AddVirtual(Data, Size, Base);
}

size_t ProcessReader::ReadVirtual(uint8_t* Buffer, uintptr_t Offset, uintptr_t Length) const
{
    const uint8_t* Data = VirtualData;
    size_t Size = VirtualSize;
    uintptr_t Base = VirtualBase;
    if (Offset < Base || Offset - Base >= Size)
    {
        Data = nullptr;
        for (const auto& R : Extra)
            if (Offset >= R.Base && Offset - R.Base < R.Size)
            {
                Data = R.Data, Size = R.Size, Base = R.Base;
                break;
            }
        if (Data == nullptr)
            return 0;
    }
    const size_t At = Offset - Base;
    const size_t N = (size_t)(std::min<uintptr_t>)(Length, Size - At);
    std::memcpy(Buffer, Data + At, N);
    return N;
}

bool ProcessReader::IsRunning() const
{
    return VirtualData != nullptr;
}

std::string ProcessReader::ReadNullTerminatedString(uintptr_t Offset)
{
    if (VirtualData != nullptr)
    {
        uintptr_t Address = Offset;
        std::string Result;
        Result.reserve(32);
        // Read a block at a time: most strings fit in one
        char Buffer[32];
        std::memset(Buffer, 0, sizeof(Buffer));

        while (true)
        {
            auto size = Read((uint8_t*)&Buffer, Address, sizeof(Buffer));
            // Outside every buffer: what was read so far
            if (size == 0)
                break;
            for (size_t i = 0; i < size; i++)
            {
                if (Buffer[i] == 0)
                    return Result;
                Result += Buffer[i];
            }
            Address += size;
        }
        return Result;
    }
#ifdef _DEBUG
    throw new std::exception("No image attached");
#else
    throw new std::exception("");
#endif
}

int8_t* ProcessReader::Read(uintptr_t Offset, uintptr_t Length, uintptr_t& Result)
{
    if (VirtualData == nullptr)
        return nullptr;

    int8_t* ResultBlock = new int8_t[Length];
    Result = ReadVirtual(reinterpret_cast<uint8_t*>(ResultBlock), Offset, Length);
    if (Result < Length)
        std::memset(ResultBlock + Result, 0, Length - Result);
    return ResultBlock;
}

size_t ProcessReader::Read(uint8_t* Buffer, uintptr_t Offset, uintptr_t Length)
{
    if (VirtualData == nullptr)
        return 0;
    return ReadVirtual(Buffer, Offset, Length);
}
