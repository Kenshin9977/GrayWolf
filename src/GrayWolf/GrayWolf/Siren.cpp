#include "stdafx.h"
#include "Siren.h"

#include <atomic>
#include <filesystem>

namespace
{
    // the library Initialize last made current
    std::atomic<const wolff::OodleDecompressor*> Current{ nullptr };

    std::string Narrow(const TCHAR* Name)
    {
        std::string Out;
        for (const TCHAR* C = Name; *C != 0; C++)
            Out += static_cast<char>(*C);   // the DLL names are ASCII
        return Out;
    }
}

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

SirenStatus Siren::Initialize(const TCHAR* DllPath)
{
    const std::string Name = DllPath != nullptr ? Narrow(DllPath) : std::string("oo2core_5_win64.dll");
    const wolff::OodleDecompressor* D = Loader().Find(Name);
    if (D == nullptr)
        return SirenStatus::MissingDll;
    Current.store(D);
    return SirenStatus::Success;
}

void Siren::Shutdown()
{
}

uintptr_t Siren::Decompress(const uint8_t* Buffer, const uintptr_t BufferSize, uint8_t* OutputBuffer, const uintptr_t OutputBufferSize)
{
    const wolff::OodleDecompressor* D = Current.load();
    return D != nullptr ? (uintptr_t)D->Decompress(Buffer, BufferSize, OutputBuffer, OutputBufferSize) : 0;
}
