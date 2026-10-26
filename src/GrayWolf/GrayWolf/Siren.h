#pragma once

#include <Windows.h>
#include <cstdint>
#include <tchar.h>
#include <string>

enum class SirenStatus
{
    Success,
    MissingDll,
    InvalidArch,
    OutdatedOrInvalid
};

class Siren
{
public:

    // Initialize Siren library loading the latest version of the Oodle dll ('oo2core_5_win64.dll')
    // DllPath may be set if the library is not located locally to the program (Direct path)
    static SirenStatus Initialize(const TCHAR* DllPath = nullptr);
    // A game's folder: a DLL named without a folder that is not beside the exe is looked for there
    // and in its parents (the games ship their own Oodle, which GrayWolf cannot redistribute)
    static void AddSearchDir(const std::wstring& Dir);
    // Shuts down the Siren library, cleaning up all allocated resources, and unloading the dll
    static void Shutdown();

    static uintptr_t Decompress(const uint8_t* Buffer, const uintptr_t BufferSize, uint8_t* OutputBuffer, const uintptr_t OutputBufferSize);

private:

    static uintptr_t OodleLZ_Compress;
    static uintptr_t OodleLZ_Decompress;

    static HMODULE OodleHandle;
};