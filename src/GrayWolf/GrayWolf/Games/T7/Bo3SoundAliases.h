#pragma once

// Black Ops III's sound aliases (the zone's SndBank, type sound) as mod tools alias rows: the
// format's own game, so every column it stores goes back to the CSV it was built from. Layout
// and conversions: HydraX's Sound.cs (GPL-3.0).

#include "SoundAliasCsv.h"

#include <cstdint>
#include <string>
#include <unordered_map>

// The aliases of the SndBank at Bank; Exported maps a file spec's hash (a bank entry's key) to the
// file the sound kind wrote, relative to the sounds folder. A file spec it does not hold keeps
// the alias's own path. Extension: the exported audio's (".wav").
void Bo3ReadSoundAliases(const SoundAliasCsv::ZoneView& Z, int64_t Bank, const std::unordered_map<uint32_t, std::string>& Exported,
                         const std::string& Extension, SoundAliasCsv::Rows& Out);

// The file specs of the SndBank at Bank: each spec's hash (a bank entry's key) to its path, e.g.
// "fr\vox\scripted\zod\vox_zod_plr_1_ammo_low_2_s": the name of a bank entry no name index holds
void Bo3ReadSoundFileSpecs(const SoundAliasCsv::ZoneView& Z, int64_t Bank, std::unordered_map<uint32_t, std::string>& Out);
