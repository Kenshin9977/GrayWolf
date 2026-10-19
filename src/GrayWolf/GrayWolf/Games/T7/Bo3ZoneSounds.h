#pragma once

#include "DiskGame.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

// BO3 (T7) Sound (XSound) export, offline, per zone. Audio bytes are never in the .ff: they live in
// `.sabl`/`.sabs` bank files (both read) under `zone/snd/<locale>/`, named after the zone stem.
// Decodes through SABSupport::LoadSound; the bank scan is local because SABSupport::ParseSAB mutates
// global CoDAssets state, unsafe for the parallel per-zone workers. Entries are FLAC files; output
// is WAV by default, "opus"/"flac" select the other two. Written, when given: each written (or
// already written) entry's key, the hash an alias's file spec holds, to its file relative to OutDir
// with backslashes (the alias CSV's FileSpec). Headers, when set: a dry run, each entry told from
// its bank header (FLAC, rate, channels, frames, size) and nothing read or written.
// The sound format a BO3 run writes: AudioFormat (wav|flac|opus) when it names one, else the
// setting every game's sounds follow (ExportWriters::SoundFormatSetting)
std::string Bo3SoundFormat(const std::string& AudioFormat);

size_t Bo3ExportZoneSounds(const std::string& FastFilePath, const std::string& OutDir,
                            const std::string& NameFilter, const std::string& AudioFormat,
                            std::string& Report,
                            std::unordered_map<uint32_t, std::string>* Written = nullptr,
                            const std::function<void(const DiskGame::SoundHeader&)>& Headers = nullptr);
