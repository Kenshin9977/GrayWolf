#pragma once

// What every offline exporter (BO3, BO4, Ghosts, ...) does once it has found an asset: write an
// image, run the live export code on a model, animation or sound without letting one crash end
// the run, and load a zonekit profile once. One place, so a fix or a speed-up reaches every game.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "LiveExport.h"
#include "ZoneTableWalker.h"

struct XImageDDS;
struct XImage_t;
struct XSound;
class CoDModel_t;
class CoDAnim_t;
class CoDSound_t;
class WraithModel;
struct XModel_t;

namespace ExportWriters
{
    // The image format the user chose (setting "exportimg": DDS, TGA, TIFF, PNG) and its extension.
    std::string ImageFormatSetting();
    std::string ImageExtension(const std::string& Format);

    // Writes one image now: the DDS bytes as they are, or decoded and encoded (PNG/TGA).
    bool WriteImage(const XImageDDS& Dds, const std::string& Path, const std::string& Format);

    struct ImageSinkStats
    {
        size_t Written = 0, Failed = 0, Linked = 0;
    };

    // Where the image sinks' time went, for the whole process, in microseconds: the run log reads
    // it around each kind. Convert is the pool's thread time (decode, encode and write a file).
    struct WriterTimes
    {
        std::atomic<uint64_t> ConvertUs{ 0 }, Converted{ 0 };
        std::atomic<uint64_t> Unchanged{ 0 };     // of Converted: the file already held those bytes
        std::atomic<uint64_t> QueueWaitUs{ 0 };   // the reading thread blocked on a full queue
        std::atomic<uint64_t> DrainUs{ 0 };       // Finish waiting for the pool
        std::atomic<uint64_t> LinkUs{ 0 };        // Finish making the links
        std::atomic<uint64_t> Workers{ 0 };       // the last sink's pool size
    };
    WriterTimes& Times();

    // Every thread's stack and the loaded modules, not the memory (a zone run holds gigabytes).
    bool WriteMinidump(const std::string& Path);

    // Images written by a pool of converter threads behind a bounded queue, so the thread that
    // reads (in disk order) never waits on encoding; and identical images written once: an image
    // whose content key was already written in this process becomes a hard link to that file, and
    // a DDS whose file already holds exactly its bytes (an earlier run) is left as it is.
    // GH_CONVERT_THREADS=<n> (legacy GH_BO4_CONVERT_THREADS) sets the pool (0 = inline);
    // GH_NODEDUP=1 (legacy GH_BO4_NODEDUP) turns the dedup off.
    class ImageSink
    {
    public:
        explicit ImageSink(std::string Format = ImageFormatSetting());
        ~ImageSink();
        ImageSink(const ImageSink&) = delete;
        ImageSink& operator=(const ImageSink&) = delete;

        const std::string& Format() const { return Fmt; }
        const std::string& Extension() const { return Ext; }

        // Content key already seen (this run, any zone): Path becomes a link Finish makes (nothing
        // when it is that very file), and true means the caller need not read the image. A new key
        // makes Path its target and returns false: the caller then Writes it. Key 0: no dedup.
        bool LinkIfKnown(uint64_t Key, const std::string& Path, size_t Tag = 0);
        // Queues a decoded image for the pool.
        void Write(std::unique_ptr<XImageDDS> Dds, const std::string& Path);

        // Waits for the pool, then makes the links. A link that cannot be made (target failed, is
        // being written by another thread's zone, NTFS's 1023-link limit) is rebuilt with
        // Rebuild(Tag) and written normally; Rebuild may return nullptr (counted in Missing).
        ImageSinkStats Finish(const std::function<std::unique_ptr<XImageDDS>(size_t Tag)>& Rebuild,
                              size_t& Missing, const std::function<uint64_t(size_t Tag)>& KeyOf = nullptr);

    private:
        struct State;
        std::unique_ptr<State> St;
        std::string Fmt, Ext;
        bool UseDedup = true;
    };

    // Writes Size bytes to P as they are, its folders made first; false when the file is not written
    bool WriteBytes(const std::filesystem::path& P, const uint8_t* Data, size_t Size);

    // ---- Sounds: every game's sounds written by WriteSound, in the format the user chose (setting
    // "exportsnd", --audio-format): WAV (the default), FLAC or OPUS.
    std::string SoundFormatSetting();
    // A sound as a game holds it.
    struct SoundSource
    {
        enum class Form
        {
            File,    // a whole sound file (Extension says which: ".wav" PCM or ADPCM, ".flac", ".mp3")
            Pcm16,   // interleaved 16-bit samples, Rate and Channels
            Opus,    // the game's Opus packets, {u16 size, packet} until Size (the last may end past
                     // it, up to Available), Rate, Channels and Frames (samples per channel, 0 unknown)
        };
        Form Is = Form::File;
        const uint8_t* Data = nullptr;
        size_t Size = 0, Available = 0;
        uint32_t Rate = 0, Channels = 0, Frames = 0;
        std::string Extension = ".wav";
        // WAV chosen: the file is written as the game keeps it, whatever it holds (Black Ops II's
        // FLAC bank entries stay .flac). Without it, only a WAV or a file that is no FLAC is (an .mp3
        // stays one); FLAC is decoded to WAV.
        bool KeptUnderWav = false;

        // A whole file, what it is said by its name's extension (none: a WAV)
        static SoundSource OfFile(const uint8_t* Data, size_t Size, const std::string& Name, bool KeptUnderWav = false)
        {
            SoundSource S;
            S.Data = Data, S.Size = S.Available = Size;
            const size_t Dot = Name.rfind('.'), Slash = Name.find_last_of("/\\");
            const bool Has = Dot != std::string::npos && (Slash == std::string::npos || Dot > Slash) && Dot + 1 < Name.size();
            S.Extension = Has ? Name.substr(Dot) : std::string(".wav");
            S.KeptUnderWav = KeptUnderWav;
            return S;
        }
    };
    // The extension WriteSound gives a sound in Format: ".flac", ".opus", or under WAV ".wav" or the
    // file's own when it is kept as it is (SoundSource::KeptUnderWav, or not a FLAC)
    std::string SoundExtension(const std::string& Format, const std::string& SourceExtension = ".wav", bool KeptUnderWav = false);
    // A sound file's name (a game's "sound/x/y.wav") as WriteSound writes that file in Format
    std::string SoundFileName(const std::string& Name, const std::string& Format, bool KeptUnderWav = false);
    // One sound written at Path (its extension SoundExtension's) in Format: kept as it is when it
    // already is that format; Opus packets remuxed into an Ogg Opus file without re-encoding;
    // anything else decoded to 16-bit PCM (WAV with or without ADPCM, FLAC, MP3, the game's Opus)
    // and encoded (Opus: 64 kb/s a channel, resampled to 48 kHz when its rate is not one Opus
    // takes). A file nothing here decodes (xWMA) is written as it is under its own extension
    // (counted sounds_kept_undecoded). Why says what failed.
    bool WriteSound(const SoundSource& S, const std::filesystem::path& Path, const std::string& Format, std::string* Why = nullptr);
    // While one lives, WriteSound on this thread writes nothing and decodes nothing: it hands each
    // sound to Run and says it was written (a size estimate's dry run, DiskGameSizes.h)
    class DrySounds
    {
    public:
        using Sink = std::function<void(const SoundSource&, const std::filesystem::path&)>;
        explicit DrySounds(Sink Run);
        ~DrySounds();
        DrySounds(const DrySounds&) = delete;
        DrySounds& operator=(const DrySounds&) = delete;

    private:
        Sink Previous;
    };
    // The SoundSource a live reader's XSound is (its WAV, FLAC or Opus packets)
    SoundSource SourceOf(const XSound& Sound);
    // Measures of the sounds written in this process (sounds_opus_remuxed, sounds_resampled ...),
    // for a run's counters
    std::map<std::string, size_t> SoundCounters();

    // An ImageSink content key, mixed from what an image's file is made of (where its pixels are,
    // its shape and format, the extension); never 0 (no dedup).
    class ContentKey
    {
    public:
        explicit ContentKey(uint64_t Seed = 0x9E3779B97F4A7C15ull) : H(Seed) {}
        ContentKey& Mix(uint64_t V)
        {
            H = (H ^ V) * 0xFF51AFD7ED558CCDull;
            H ^= H >> 32;
            return *this;
        }
        ContentKey& Mix(const std::string& Bytes)
        {
            for (const char Ch : Bytes) Mix((uint8_t)Ch);
            return *this;
        }
        uint64_t Key() const { return H ? H : 1; }

    private:
        uint64_t H;
    };

    // Where a model's textures go and how the model file names them.
    struct ModelTextures
    {
        std::string ImagesDir;   // where the image files are written
        std::string Relative;    // how the model file refers to that folder (escaped backslashes)
        std::string Extension;   // ImageExtension(ImageFormatSetting()) unless overridden
        // When set, each texture is handed to it (renamed as it says, dropped when it returns
        // false) and written by the caller, all in ImagesDir: the image kind's own writer.
        std::function<bool(XImage_t& Image)> Take;
    };
    // The pointer of a texture known by name only: a material another zone defines named it, and
    // this zone holds no image for it (Take hands it on, the zone that defines it writes it).
    constexpr uint64_t ImageElsewhere = 1;

    // The live export code, guarded: the ExportGameResult as an int, or -1 when it crashed, with
    // Crash naming the exception (C++ type, or SEH code and module offset). GH_EXPORT_CRASHDUMP
    // (legacy GH_BO4_CRASHDUMP) = <file> writes a full minidump of the first crash.
    int ExportModel(const CoDModel_t& Model, const std::string& Dir, const ModelTextures& Textures, std::string& Crash);
    int ExportAnim(const CoDAnim_t& Anim, const std::string& Dir, std::string& Crash);
    int ExportSound(const CoDSound_t& Sound, const std::string& Dir, const std::string& Extension, std::string& Crash);

    // A model as the live reader gives it: its generic form (materials, LODs) and the LOD an
    // export would write (the biggest loaded one) as a mesh.
    struct LiveModel
    {
        LiveModel();
        ~LiveModel();
        std::unique_ptr<XModel_t> Generic;
        std::unique_ptr<WraithModel> Mesh;
        int Lod = -1;
    };
    // The live model reader, guarded like ExportModel: 0 with Out filled, 1 without a loaded LOD,
    // -1 when it crashed (Crash names it).
    int ReadModel(const CoDModel_t& Model, LiveModel& Out, std::string& Crash);

    // Images as PNG (what a glTF file may refer to): each Source (a file the image kind wrote) to
    // its Target through an ImageSink, a PNG source linked, a Target already there kept. Sources
    // seen before in this process are linked to their first copy. The number written or kept.
    size_t PngCopies(const std::vector<std::pair<std::string, std::string>>& SourceTarget, size_t& Failed);

    // Signed DDS (BC4/BC5 SNORM, R8G8(B8A8) SNORM: Ghosts' normals) that Blender cannot read,
    // copied to Target as the UNORM format, remapped to v * 0.5 + 0.5 without decoding: +128 on
    // each block endpoint or texel. Other formats are skipped. The number written or kept.
    size_t UnsignedDdsCopies(const std::vector<std::pair<std::string, std::string>>& SourceTarget, size_t& Failed);

    // DDS larger than MaxSize (either side) copied to Target at most MaxSize, with a full mip chain,
    // in the source's format (BC6H/BC7 compressed on the GPU with DirectCompute when a D3D11 device
    // exists, as texconv does; BC1-5 on the CPU). A Target already there is the source (an unsigned
    // copy) and is replaced. The number written; Seconds, the time spent.
    size_t ResizedDdsCopies(const std::vector<std::pair<std::string, std::string>>& SourceTarget, uint32_t MaxSize,
                            size_t& Failed, double& Seconds, bool& Gpu);

    // RFC4180-ish: a field quoted only when it needs it (comma/quote/newline), quotes doubled.
    std::string CsvField(const std::string& S);
    // A string table as CSV, every game's: Rows lines of Columns cells (Cells row by row, a missing
    // cell empty), each field CsvField's, CRLF, in <DataDir>/stringtables/<the name, '/' made '_'>
    // (.csv added unless it ends so)
    bool WriteStringTable(const std::string& DataDir, const std::string& Name, uint32_t Columns, uint32_t Rows,
                          const std::vector<std::string>& Cells);
    // A game's text as UTF-8: kept when it already is, else read in CodePage (CODOL's GBK, 936), or
    // as Windows-1252 (the older games') when it is 0
    std::string Utf8(const std::string& S, unsigned CodePage = 0);
    // The code page the older games (CoD1 to Black Ops) keep a language's text in, by its name in a
    // path or a language setting ("zone/russian/", "localized_polish_"): 1251 Cyrillic, 1250
    // Central European, 0 (Windows-1252) for the others
    unsigned LanguageCodePage(const std::string& PathOrLanguage);
    // A zone's localized strings as one CSV: "key,text" then a row per entry, CRLF, in UTF-8 (a
    // field that is not is read in CodePage, as Utf8 does).
    bool WriteLocalize(const std::string& Path, const std::vector<std::pair<std::string, std::string>>& Rows,
                       unsigned CodePage = 0);

    // A zonekit profile and its compiled tables, loaded once per name for the process.
    struct LoadedProfile
    {
        ZoneTables::Profile Profile;
        ZoneTables::Tables* Tables = nullptr;
    };
    const LoadedProfile* ZoneProfile(const std::string& Name, std::string& Error);
}
