#pragma once

// The image mips a game streams from its CDN and does not install (MW2019, MWIII, BO6, BO7), kept
// in %LOCALAPPDATA%\GrayWolf\cdn. Setting cdn_images: "ask" (default: the GUI asks at the first
// download and saves the answer; no GUI means off), "on", "off". GRAYWOLF_OFFLINE_IMAGES=1: no
// CDN, no xpak_cache.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace CdnObjects
{
    enum class Mode { Ask, On, Off };
    enum class Decision { Undecided, Yes, No };

    // the setting, read
    Mode Setting();
    // GRAYWOLF_OFFLINE_IMAGES=1: neither the CDN nor the game's on-demand cache
    bool Offline();
    // the CDN's URL a game's exe holds ("http://cod-assets.cdn.../pc/<build>"), empty if none
    std::string BaseInExe(const std::filesystem::path& Exe);
    // whether an object may be fetched now (Ask: the session's answer, asked of the GUI once; a
    // worker waits for it, the GUI's own thread does not and gets false until it is answered)
    bool Allowed();
    // an object the CDN holds and this run did not fetch (counted once per key)
    void NoteSkipped(uint64_t Key);
    size_t Skipped();

    // whether Key from Base was fetched before (kept in %LOCALAPPDATA%\GrayWolf\cdn)
    bool IsKept(const std::string& Base, uint64_t Key);

    // The object Key at Base + Path (as the game names it), its bytes as stored there: the kept
    // copy when Whole accepts it, else downloaded (kept when Whole accepts it). Nothing when not
    // Allowed (counted), or when the download fails or Whole refuses it.
    std::optional<std::vector<uint8_t>> Fetch(const std::string& Base, const std::string& Path, uint64_t Key,
                                              const std::function<bool(const std::vector<uint8_t>&)>& Whole);

    // The GUI: AttachAsker once from its thread; each frame, Question() says whether to show the
    // dialog (and for which host); Answer() settles it and saves it as the setting (on/off), so
    // it is asked once, at the first download ever.
    void AttachAsker();
    bool Question(std::string& Host);
    void Answer(bool Download);

    // The consent's state machine alone (self-test): what Allowed decides, given the setting,
    // whether a GUI asks, whether the call is on the GUI's thread and the session's answer so far;
    // Ask is set when the question is to be put.
    bool Decide(Mode M, bool HasAsker, bool OnGuiThread, Decision Session, bool& Ask);
    // failures of its assertions, one line each (empty: passed)
    std::string SelfTest();
}
