#pragma once

#include <string>
#include <vector>

// Audio player built on miniaudio: decodes the file to PCM in memory (WAV including MS/IMA ADPCM,
// FLAC and MP3 via miniaudio; Opus via our own decoder), plays it through the OS device, and
// exposes the playback cursor so the UI can show duration, progress and a waveform playhead.
//
// All miniaudio types stay behind a pimpl so this header pulls in nothing heavy.
class AudioPlayer
{
public:
    AudioPlayer();
    ~AudioPlayer();

    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    // Decodes `path` to PCM and prepares it for playback. `formatHint` is a lowercase
    // extension-style hint ("wav" / "flac" / "opus" / "mp3"); it only matters for Opus,
    // which we decode ourselves. Returns false if the file cannot be decoded.
    bool Load(const std::string& path, const std::string& formatHint);
    void Unload();
    // Only the waveform envelope (Columns min/max pairs) and the length, no playback: safe on any
    // thread, several at once.
    static bool ReadWaveform(const std::string& path, const std::string& formatHint, int columns,
                             std::vector<float>& outMin, std::vector<float>& outMax, double* seconds = nullptr);

    void Play();        // start, or resume from the current position (restarts if at the end)
    void Pause();       // pause and keep the cursor where it is
    void Stop();        // stop and rewind to the start
    void TogglePlay();  // Pause() if playing, Play() otherwise
    void Seek(double seconds);

    // ui-record: players loaded from now on open no device; their sound is pulled by ReadOffline,
    // stereo at Rate, the cursor moving only as it is read
    static void UseOfflineClock(unsigned rate);
    // the next frames of what plays, interleaved stereo f32 (silence where nothing does)
    void ReadOffline(float* out, unsigned long long frames);

    bool     IsLoaded()   const { return m_loaded; }
    bool     IsPlaying()  const;
    double   Duration()   const { return m_duration; }   // seconds
    double   Position()   const;                          // seconds
    unsigned SampleRate() const { return m_rate; }
    unsigned Channels()   const { return m_channels; }

    // Precomputed mono waveform envelope: min / max per column, in [-1, 1], same length.
    const std::vector<float>& PeaksMin() const { return m_peaksMin; }
    const std::vector<float>& PeaksMax() const { return m_peaksMax; }

private:
    struct Impl;
    Impl*  m_impl = nullptr;

    bool     m_loaded   = false;
    double   m_duration = 0.0;
    unsigned m_rate     = 0;
    unsigned m_channels = 0;
    std::vector<float> m_peaksMin, m_peaksMax;
};
