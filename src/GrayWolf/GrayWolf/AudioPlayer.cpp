#include "stdafx.h"

// The single translation unit that compiles the miniaudio implementation. No other
// file may define MINIAUDIO_IMPLEMENTATION. We do not need the node graph / generation
// helpers, so trim the build a little.
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_GENERATION
#include "miniaudio.h"

#include "AudioPlayer.h"
#include "Sound.h"          // Opus -> WAV decode (miniaudio has no Opus backend)

#include <filesystem>
#include <algorithm>
#include <atomic>

struct AudioPlayer::Impl
{
    ma_engine       Engine{};
    ma_audio_buffer Buffer{};
    ma_sound        Sound{};
    bool            EngineInit = false;
    bool            BufferInit = false;
    bool            SoundInit  = false;

    std::vector<float> Pcm;        // interleaved f32, kept alive for the audio buffer to reference
    ma_uint64          Frames = 0; // frames per channel
};

AudioPlayer::AudioPlayer() : m_impl(new Impl()) {}

AudioPlayer::~AudioPlayer()
{
    Unload();
    delete m_impl;
    m_impl = nullptr;
}

void AudioPlayer::Unload()
{
    if (!m_impl) return;
    if (m_impl->SoundInit)  { ma_sound_uninit(&m_impl->Sound);        m_impl->SoundInit  = false; }
    if (m_impl->BufferInit) { ma_audio_buffer_uninit(&m_impl->Buffer); m_impl->BufferInit = false; }
    if (m_impl->EngineInit) { ma_engine_uninit(&m_impl->Engine);       m_impl->EngineInit = false; }
    m_impl->Pcm.clear();
    m_impl->Frames = 0;

    m_loaded   = false;
    m_duration = 0.0;
    m_rate = 0;
    m_channels = 0;
    m_peaksMin.clear();
    m_peaksMax.clear();
}

// Builds a mono min/max envelope from the interleaved PCM so the UI can draw a waveform.
static void BuildPeaks(const std::vector<float>& Pcm, unsigned Channels, ma_uint64 Frames,
                       std::vector<float>& OutMin, std::vector<float>& OutMax, int Columns = 720)
{
    OutMin.assign(Columns, 0.0f);
    OutMax.assign(Columns, 0.0f);
    if (Frames == 0 || Channels == 0) return;

    for (int c = 0; c < Columns; ++c)
    {
        const ma_uint64 Start = (ma_uint64)(( (double)c      / Columns) * Frames);
        ma_uint64       End   = (ma_uint64)(( (double)(c + 1) / Columns) * Frames);
        if (End <= Start) End = Start + 1;
        if (End > Frames) End = Frames;

        float Lo = 1.0f, Hi = -1.0f;
        for (ma_uint64 f = Start; f < End; ++f)
        {
            // Mix channels down to mono for the envelope.
            float Sum = 0.0f;
            const float* Frame = &Pcm[(size_t)f * Channels];
            for (unsigned ch = 0; ch < Channels; ++ch) Sum += Frame[ch];
            const float S = Sum / (float)Channels;
            if (S < Lo) Lo = S;
            if (S > Hi) Hi = S;
        }
        if (Lo > Hi) { Lo = 0.0f; Hi = 0.0f; }
        OutMin[c] = std::max(-1.0f, Lo);
        OutMax[c] = std::min( 1.0f, Hi);
    }
}

// Decodes path to interleaved f32 PCM at its own rate and channel count. miniaudio decodes WAV
// (incl. MS/IMA ADPCM), FLAC and MP3 natively; Opus has no backend, so it goes through a temp WAV
// first (named per call: the asset grid's waveforms decode on workers while the player loads).
static bool DecodePcm(const std::string& path, const std::string& formatHint, std::vector<float>& pcm,
                      unsigned& channels, unsigned& rate, ma_uint64& framesRead)
{
    std::string decodePath = path;
    std::string tempFile;
    if (formatHint == "opus")
    {
        static std::atomic<unsigned> Serial{ 0 };
        tempFile = (std::filesystem::temp_directory_path() /
                    ("greyhound_ap_opus_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(Serial++) + ".wav")).string();
        if (!Sound::DecodeOpusToWav(path, tempFile))
            return false;
        decodePath = tempFile;
    }

    ma_decoder_config dcfg = ma_decoder_config_init(ma_format_f32, 0, 0); // keep native channels / rate
    ma_decoder decoder;
    ma_result r = ma_decoder_init_file(decodePath.c_str(), &dcfg, &decoder);
    if (r != MA_SUCCESS)
    {
        if (!tempFile.empty()) { std::error_code ec; std::filesystem::remove(tempFile, ec); }
        return false;
    }

    channels = decoder.outputChannels;
    rate     = decoder.outputSampleRate;

    ma_uint64 total = 0;
    ma_decoder_get_length_in_pcm_frames(&decoder, &total);

    pcm.clear();
    framesRead = 0;
    if (total > 0)
    {
        pcm.resize((size_t)total * channels);
        ma_decoder_read_pcm_frames(&decoder, pcm.data(), total, &framesRead);
    }
    else
    {
        // Length unknown up front (some streams): read in chunks until exhausted.
        const ma_uint64 Chunk = 4096;
        std::vector<float> tmp((size_t)Chunk * channels);
        ma_uint64 got = 0;
        while (ma_decoder_read_pcm_frames(&decoder, tmp.data(), Chunk, &got) == MA_SUCCESS && got > 0)
        {
            pcm.insert(pcm.end(), tmp.begin(), tmp.begin() + (size_t)got * channels);
            framesRead += got;
            if (got < Chunk) break;
        }
    }
    ma_decoder_uninit(&decoder);
    if (!tempFile.empty()) { std::error_code ec; std::filesystem::remove(tempFile, ec); }

    if (framesRead == 0 || channels == 0)
        return false;
    pcm.resize((size_t)framesRead * channels);
    return true;
}

bool AudioPlayer::ReadWaveform(const std::string& path, const std::string& formatHint, int columns,
                               std::vector<float>& outMin, std::vector<float>& outMax, double* seconds)
{
    std::vector<float> pcm;
    unsigned channels = 0, rate = 0;
    ma_uint64 frames = 0;
    if (!DecodePcm(path, formatHint, pcm, channels, rate, frames)) return false;
    BuildPeaks(pcm, channels, frames, outMin, outMax, columns);
    if (seconds) *seconds = rate ? (double)frames / rate : 0.0;
    return true;
}

bool AudioPlayer::Load(const std::string& path, const std::string& formatHint)
{
    Unload();

    std::vector<float> pcm;
    unsigned channels = 0, rate = 0;
    ma_uint64 framesRead = 0;
    if (!DecodePcm(path, formatHint, pcm, channels, rate, framesRead))
        return false;

    // Move the PCM into the impl so the audio buffer can reference it for the object's lifetime.
    m_impl->Pcm    = std::move(pcm);
    m_impl->Frames = framesRead;

    ma_audio_buffer_config abcfg =
        ma_audio_buffer_config_init(ma_format_f32, channels, framesRead, m_impl->Pcm.data(), nullptr);
    abcfg.sampleRate = rate;
    if (ma_audio_buffer_init(&abcfg, &m_impl->Buffer) != MA_SUCCESS)
    {
        m_impl->Pcm.clear();
        return false;
    }
    m_impl->BufferInit = true;

    if (ma_engine_init(nullptr, &m_impl->Engine) != MA_SUCCESS)
    {
        ma_audio_buffer_uninit(&m_impl->Buffer); m_impl->BufferInit = false;
        m_impl->Pcm.clear();
        return false;
    }
    m_impl->EngineInit = true;

    if (ma_sound_init_from_data_source(&m_impl->Engine, &m_impl->Buffer,
                                       MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, &m_impl->Sound) != MA_SUCCESS)
    {
        ma_engine_uninit(&m_impl->Engine);       m_impl->EngineInit = false;
        ma_audio_buffer_uninit(&m_impl->Buffer); m_impl->BufferInit = false;
        m_impl->Pcm.clear();
        return false;
    }
    m_impl->SoundInit = true;

    m_rate     = rate;
    m_channels = channels;
    m_duration = (double)framesRead / (double)rate;
    BuildPeaks(m_impl->Pcm, channels, framesRead, m_peaksMin, m_peaksMax);

    m_loaded     = true;
    return true;
}

bool AudioPlayer::IsPlaying() const
{
    if (!m_loaded || !m_impl->SoundInit) return false;
    return ma_sound_is_playing(&m_impl->Sound) == MA_TRUE;
}

double AudioPlayer::Position() const
{
    if (!m_loaded || !m_impl->SoundInit || m_rate == 0) return 0.0;
    ma_uint64 cursor = 0;
    if (ma_sound_get_cursor_in_pcm_frames(&m_impl->Sound, &cursor) != MA_SUCCESS) return 0.0;
    return (double)cursor / (double)m_rate;
}

void AudioPlayer::Play()
{
    if (!m_loaded || !m_impl->SoundInit) return;
    // Restart from the top if the cursor is sitting at (or past) the end.
    ma_uint64 cursor = 0;
    ma_sound_get_cursor_in_pcm_frames(&m_impl->Sound, &cursor);
    if (cursor >= m_impl->Frames)
        ma_sound_seek_to_pcm_frame(&m_impl->Sound, 0);
    ma_sound_start(&m_impl->Sound);
}

void AudioPlayer::Pause()
{
    if (!m_loaded || !m_impl->SoundInit) return;
    ma_sound_stop(&m_impl->Sound); // keeps the cursor in place
}

void AudioPlayer::Stop()
{
    if (!m_loaded || !m_impl->SoundInit) return;
    ma_sound_stop(&m_impl->Sound);
    ma_sound_seek_to_pcm_frame(&m_impl->Sound, 0);
}

void AudioPlayer::TogglePlay()
{
    if (IsPlaying()) Pause();
    else             Play();
}

void AudioPlayer::Seek(double seconds)
{
    if (!m_loaded || !m_impl->SoundInit || m_rate == 0) return;
    if (seconds < 0.0) seconds = 0.0;
    ma_uint64 frame = (ma_uint64)(seconds * m_rate);
    if (frame > m_impl->Frames) frame = m_impl->Frames;
    ma_sound_seek_to_pcm_frame(&m_impl->Sound, frame);
}
