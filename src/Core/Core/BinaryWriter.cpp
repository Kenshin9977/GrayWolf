#include "stdafx.h"

#include "BinaryWriter.h"

#include <cstring>
#include <stdexcept>

BinaryWriter::BinaryWriter(BinaryWriter&& Other) noexcept
    : Stream(std::move(Other.Stream)), Buffer(std::move(Other.Buffer)), Buffered(Other.Buffered), Appending(Other.Appending)
{
    Other.Buffered = 0;
}

BinaryWriter::~BinaryWriter()
{
    Flush();
}

bool BinaryWriter::Create(const std::string& FileName)
{
    Close();
    Stream.open(FileName, std::ios::binary | std::ios::trunc);
    Appending = false;
    return Stream.is_open();
}

bool BinaryWriter::Open(const std::string& FileName)
{
    Close();
    Stream.open(FileName, std::ios::binary | std::ios::app);
    Appending = true;
    return Stream.is_open();
}

void BinaryWriter::Flush()
{
    if (Buffered == 0)
    {
        return;
    }
    if (Stream.is_open())
    {
        Stream.write(Buffer.get(), (std::streamsize)Buffered);
    }
    Buffered = 0;
}

void BinaryWriter::Close()
{
    Flush();
    if (Stream.is_open())
    {
        Stream.close();
    }
    Stream.clear();
}

uint64_t BinaryWriter::GetPosition()
{
    if (!Stream.is_open())
    {
        return 0;
    }
    // appended to, the stream's position moves to the end at each write: asked once they are written
    if (Appending)
    {
        Flush();
        return (uint64_t)Stream.tellp();
    }
    return (uint64_t)Stream.tellp() + Buffered;
}

uint64_t BinaryWriter::GetLength()
{
    if (!Stream.is_open())
    {
        return 0;
    }
    Flush();
    const auto Position = Stream.tellp();
    Stream.seekp(0, std::ios::end);
    const auto Length = (uint64_t)Stream.tellp();
    Stream.seekp(Position);
    return Length;
}

void BinaryWriter::SetPosition(uint64_t Offset)
{
    if (Stream.is_open())
    {
        Flush();
        Stream.seekp((std::streamoff)Offset, std::ios::beg);
    }
}

void BinaryWriter::WriteNullTerminatedString(const std::string& Value)
{
    WriteRaw(Value.c_str(), 1, Value.size() + 1);
}

void BinaryWriter::Write(const int8_t* Buffer, uint32_t Size)
{
    WriteRaw(Buffer, 1, Size);
}

void BinaryWriter::Write(const uint8_t* Buffer, uint32_t Size)
{
    WriteRaw(Buffer, 1, Size);
}

void BinaryWriter::WriteRaw(const void* Data, size_t ElementSize, size_t Count)
{
    if (!Stream.is_open())
    {
        throw std::runtime_error("BinaryWriter: no file is open");
    }
    const size_t Size = ElementSize * Count;
    if (Buffered + Size > BufferSize)
    {
        Flush();
    }
    // a block as large as the buffer goes to the file as it is
    if (Size >= BufferSize)
    {
        Stream.write(static_cast<const char*>(Data), (std::streamsize)Size);
        return;
    }
    if (!Buffer)
    {
        Buffer = std::make_unique_for_overwrite<char[]>(BufferSize);
    }
    std::memcpy(Buffer.get() + Buffered, Data, Size);
    Buffered += Size;
}
