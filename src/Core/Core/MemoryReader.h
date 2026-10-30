#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

class MemoryReader
{
private:
    int8_t* DataPointer;
    uint64_t DataLength;
    uint64_t CurrentPosition;
    // Whether or not to keep the buffer alive
    bool KeepAlive;

public:
    __declspec(noinline) MemoryReader();
    __declspec(noinline) MemoryReader(int8_t* Stream, uint64_t StreamLength, bool KeepStreamAlive = false);
    ~MemoryReader();

    void Setup(int8_t* Stream, uint64_t StreamLength, bool KeepStreamAlive = false);

    void Close();

    uint64_t GetLength() const;
    uint64_t GetPosition() const;

    int8_t* GetCurrentStream() const;
    // Gets the current stream and advances the position. If the length is smaller, nullptr is returned.
    int8_t* GetCurrentStream(const size_t size);

    void SetPosition(uint64_t Offset);
    void Advance(uint64_t Length);

    template <class T>
    T Read()
    {
        if (DataPointer != nullptr)
        {
            T ResultValue;
            auto ResultSize = sizeof(ResultValue);
            std::memset(&ResultValue, 0, ResultSize);
            if ((CurrentPosition + ResultSize) <= DataLength)
            {
                ResultValue = *(reinterpret_cast<T*>(DataPointer + CurrentPosition));
                CurrentPosition += ResultSize;
                return ResultValue;
            }
        }
        throw std::runtime_error("No stream is open");
    }
    template <class T>
    T Read(uint64_t Offset)
    {
        if (DataPointer != nullptr)
        {
            SetPosition(Offset);
            return Read<T>();
        }
        throw std::runtime_error("No stream is open");
    }
    void Read(uint64_t Length, int8_t* Result);
    std::string ReadNullTerminatedString();
};