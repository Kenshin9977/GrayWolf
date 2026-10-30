#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>

// A file written as a sequence of typed values. Writing to a file that did not open throws.
// The values gather in a buffer of the writer's own, handed to the file a block at a time (on
// Close, on a seek, when the writer goes away): a stream write per value cost a model file more
// than building it (the stream's lock and sentry per call).
class BinaryWriter
{
private:
    static constexpr size_t BufferSize = 256 << 10;   // under the heap's large-block threshold: no fresh pages per file

    std::ofstream Stream;
    std::unique_ptr<char[]> Buffer;
    size_t Buffered = 0;
    bool Appending = false;

    // Count elements of ElementSize bytes to the file, which must be open
    void WriteRaw(const void* Data, size_t ElementSize, size_t Count);
    // What the buffer holds, to the file
    void Flush();

public:
    BinaryWriter() = default;
    BinaryWriter(BinaryWriter&& Other) noexcept;
    BinaryWriter& operator=(BinaryWriter&&) = delete;
    ~BinaryWriter();

    // Creates or empties the file
    bool Create(const std::string& FileName);
    // Opens the file (created when missing) to append to it: every write goes to its end
    bool Open(const std::string& FileName);

    void Close();

    uint64_t GetPosition();
    uint64_t GetLength();
    // Where the next write goes, to rewrite what is there
    void SetPosition(uint64_t Offset);

    void WriteNullTerminatedString(const std::string& Value);

    template<class T>
    void Write(T Value)
    {
        WriteRaw(&Value, sizeof(T), 1);
    }

    template<uint32_t Size>
    void Write(const char(&Value)[Size])
    {
        WriteRaw(&Value, 1, Size);
    }

    template<uint32_t Size>
    void Write(const uint8_t(&Value)[Size])
    {
        WriteRaw(&Value, 1, Size);
    }

    void Write(const int8_t* Buffer, uint32_t Size);
    void Write(const uint8_t* Buffer, uint32_t Size);
};
