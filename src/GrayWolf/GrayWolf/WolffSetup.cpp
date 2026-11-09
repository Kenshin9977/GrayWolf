#include "stdafx.h"

// Wolff's default context for the whole process, set before main runs: GrayWolf's one Oodle loader
// (Siren.cpp, whose search directories the install opened adds); no KeyProvider, Wolff finds the
// keys in the games' files itself (and caches them in %LOCALAPPDATA%\wolff\keys.json). Every
// FastFile call that takes no context uses it. And where the zonekit profiles (Wolff's) are:
// <exe>\zonekit\profiles (the build copies them there), then Wolff's own places (its installed
// share\wolff\profiles for a dev build).

#include "Siren.h"

#include <wolff/FastFile.h>
#include <wolff/ZoneTables.h>

#include <filesystem>

namespace
{
    struct WolffSetup
    {
        WolffSetup()
        {
            wolff::SetDefaultContext({ nullptr, &Siren::Loader() });
            wolff::RegisterBuiltins();

            wchar_t Buf[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, Buf, MAX_PATH);
            std::vector<std::filesystem::path> Roots{ std::filesystem::path(Buf).parent_path() / "zonekit" / "profiles" };
            for (auto& R : wolff::ZoneTables::DefaultProfileRoots()) Roots.push_back(std::move(R));
            wolff::ZoneTables::SetProfileRoots(std::move(Roots));
        }
    };
    const WolffSetup Setup;
}
