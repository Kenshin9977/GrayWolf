#include "stdafx.h"
#include "Siren.h"

#include <filesystem>
#include <stdexcept>

wolff::OodleDllLoader& Siren::Loader()
{
    // never destroyed: a worker may still decode while the process ends
    static wolff::OodleDllLoader* L = new wolff::OodleDllLoader();
    return *L;
}

void Siren::AddSearchDir(const std::wstring& Dir)
{
    Loader().AddSearchDir(std::filesystem::path(Dir));
}

const wolff::OodleDecompressor* Siren::Library(Names Dlls)
{
    for (const std::string_view Name : Dlls)
        if (const wolff::OodleDecompressor* D = Loader().Find(Name)) return D;
    return nullptr;
}

uintptr_t Siren::Decompress(Names Dlls, const uint8_t* Buffer, const uintptr_t BufferSize, uint8_t* OutputBuffer, const uintptr_t OutputBufferSize)
{
    const wolff::OodleDecompressor* D = Library(Dlls);
    if (D == nullptr)
    {
        std::string Listed;
        for (const std::string_view Name : Dlls) Listed += (Listed.empty() ? "" : " or ") + std::string(Name);
        throw std::runtime_error(wolff::OodleMissing(Listed, "The game's streamed models, images and sounds"));
    }
    return (uintptr_t)D->Decompress(Buffer, BufferSize, OutputBuffer, OutputBufferSize);
}
