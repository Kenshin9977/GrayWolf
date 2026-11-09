#pragma once

#include <Windows.h>
#include <cstdint>
#include <tchar.h>
#include <string>

#include <wolff/Context.h>

enum class SirenStatus
{
    Success,
    MissingDll,
    InvalidArch,
    OutdatedOrInvalid
};

// GrayWolf's Oodle: the games' own oo2core DLLs, loaded by Wolff's OodleDllLoader (one slot per DLL,
// never one for the process). The fast files reach it through Wolff's default context
// (WolffSetup.cpp); the package caches (XPAK, XSUB) through the calls below.
class Siren
{
public:

    // Makes DllPath ('oo2core_5_win64.dll' when none) the library Decompress uses, loading it when
    // it is not yet (beside the exe, else in a search directory)
    static SirenStatus Initialize(const TCHAR* DllPath = nullptr);
    // A game's folder: a DLL named without a folder that is not beside the exe is looked for there
    // and in its parents (the games ship their own Oodle, which GrayWolf cannot redistribute)
    static void AddSearchDir(const std::wstring& Dir);
    // Kept for its callers: the libraries stay loaded (other threads may be decoding with them)
    static void Shutdown();

    static uintptr_t Decompress(const uint8_t* Buffer, const uintptr_t BufferSize, uint8_t* OutputBuffer, const uintptr_t OutputBufferSize);

    // The loader every Oodle library of the process comes from
    static wolff::OodleDllLoader& Loader();
};
