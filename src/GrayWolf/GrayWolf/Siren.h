#pragma once

#include <Windows.h>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <wolff/Context.h>

// GrayWolf's Oodle: the games' own oo2core DLLs, all through Wolff's one OodleDllLoader (Loader), which
// finds each DLL by its name once and keeps it, beside the exe else in a search directory and up to
// three folders above it, and looks for a missing one again only once a folder has been added. The
// fast files reach it through Wolff's default context (WolffSetup.cpp; FastFile::Decompress adds the
// fast file's folder or the context's install itself); the package caches (XPAK, XSUB) through
// Decompress, naming their DLLs on every block, and add their own folder when they open
// (CoDPackageCache), so no caller has to add the install first.
class Siren
{
public:
    // The DLLs one container's blocks decode with, the first found used (a later Oodle decodes the
    // earlier ones' blocks)
    using Names = std::span<const std::string_view>;

    // The first of Names found, nullptr when none is
    static const wolff::OodleDecompressor* Library(Names Dlls);
    // A game's folder: a DLL named without a folder that is not beside the exe is looked for there
    // and in its parents (the games ship their own Oodle, which GrayWolf cannot redistribute)
    static void AddSearchDir(const std::wstring& Dir);

    // Buffer decoded by the first of Dlls found: the bytes produced, 0 when the block does not
    // decode. None found throws std::runtime_error with wolff::OodleMissing's one line (the DLL and
    // where to copy it): a block is never left empty in silence.
    static uintptr_t Decompress(Names Dlls, const uint8_t* Buffer, const uintptr_t BufferSize, uint8_t* OutputBuffer,
                                const uintptr_t OutputBufferSize);

    // The loader every Oodle library of the process comes from
    static wolff::OodleDllLoader& Loader();
};
