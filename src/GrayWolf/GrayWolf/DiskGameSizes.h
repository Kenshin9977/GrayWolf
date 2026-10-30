#pragma once

// DiskGame::Sizes: about how much an export writes, for the Load Game panel. A sample of each kind
// is exported for real (DiskGame::Export: the zone's own assets, no dependency pulled) into a
// temporary folder and measured file by file; the estimate is then the install index's counts
// times what a sampled asset wrote. It is computed again from those measures, never by reading a
// zone, whenever the kinds, the zones or the formats change: a format the sample was not written
// in is converted by a typical ratio, and said to be an approximation.

#include "DiskGame.h"
#include "ExportWriters.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace DiskGame::Sizes
{
    // What the sample of one kind wrote
    struct Sample
    {
        // the install index lists this kind (IDiskZone::Defines): the zones' counts are known, the
        // estimate is bytes per asset times them; otherwise per byte of zone footprint
        bool Listed = false;
        size_t Assets = 0;                       // the assets its exporter went through (Totals::Seen)
        std::map<std::string, uint64_t> Bytes;   // by file extension, lower case (".png", ".cast"), in its kind's folder
        uint64_t Textures = 0;                   // a model sample: the images it wrote beside it
        uint64_t ListedTextures = 0;             // ... of them, those the index lists as its zone's images
        // the sampled zone: its footprint, how many of the kind it holds (the exporter's count,
        // 0 when it gave none) and what the kind wrote over it whole (extrapolated when Cut)
        uint64_t ZoneFootprint = 0, ZoneBytes = 0;
        size_t ZoneAssets = 0;
        bool Cut = false;                        // stopped at the sample's size or time limit
        double Seconds = 0;
        std::string Zone;
    };

    // What a sample wrote in its kind's folder
    uint64_t Written(const Sample& S);

    // A sound as its game's headers give it (a dry run of the sound kind, Context::OnSoundHeader),
    // its name hashed: what each format writes for it is worked out from this, no sample decoded
    struct SoundRow
    {
        uint64_t Key = 0;   // FNV-1a 64 of its file's name, lower case: a file written once
        SoundHeader::Form Is = SoundHeader::Form::Wav;
        bool KeptUnderWav = false;
        uint32_t Rate = 0, Channels = 0;
        uint64_t Stored = 0, Frames = 0;
        uint32_t Bits = 16;
    };
    SoundRow RowOf(const SoundHeader& H);
    // A FLAC file's STREAMINFO from its first bytes (42 suffice): false when they are no FLAC
    bool FlacStreamInfo(const uint8_t* Data, size_t Size, uint32_t& Rate, uint32_t& Channels, uint32_t& Bits, uint64_t& Frames);
    // A sound as a reader hands it to WriteSound (ExportWriters::SoundSource), its length read
    // from its WAV or FLAC header: what a dry run of a reader without a header path of its own gives
    SoundHeader HeaderOf(const ExportWriters::SoundSource& S, const std::string& Name);
    // What WriteSound writes for it in Format (WAV, FLAC, OPUS), mirroring its rules; Exact false
    // when that is an encoder's output (a typical ratio) or a length the header does not give
    uint64_t SoundBytes(const SoundRow& R, const std::string& Format, bool& Exact);

    // An image's weight (DiskGame::Weight): the pixel data its reader writes behind a DDS header
    // (Bytes), its size, its faces or slices
    Weight ImageWeight(uint64_t Bytes, uint32_t Width, uint32_t Height, uint32_t Layers = 1);
    // ... its pixel data worked out from its DXGI format, the mips its reader writes from the largest
    Weight ImageWeight(uint32_t Width, uint32_t Height, uint32_t DxgiFormat, uint32_t Mips, uint32_t Layers);
    // A streamed image whose record has Width, Height, MipLevels[3] (each a Width and Height),
    // ImageFormat (a DXGI format) and MapType (5: a cube): its largest level, as
    // GameReaders::TranslatePakImage writes it (Ghosts, Advanced Warfare, Modern Warfare Remastered)
    template <class TImage>
    Weight PakImageWeight(const TImage& Info)
    {
        uint32_t Width = Info.Width, Height = Info.Height;
        for (const auto& M : Info.MipLevels)
            if (M.Width > Width) Width = M.Width, Height = M.Height;
        return ImageWeight(Width, Height, Info.ImageFormat, 1, Info.MapType == 5 ? 6 : 1);
    }
    // An image its reader reads from an .iwi file of FileBytes (Call of Duty 4, World at War, Modern
    // Warfare 2 and 3: an .iwd archive's): its largest mip, about three quarters of the file (its
    // header and smaller mips the rest), an approximation
    Weight IwiWeight(uint64_t FileBytes, uint32_t Width = 0, uint32_t Height = 0);
    // An image whose record says the memory its mips take (CardBytes: Black Ops II's, Call of Duty
    // Online's cardMemory): its largest mip, all of it when that is a whole number of bytes per
    // pixel (no smaller mips), else about three quarters
    Weight CardMemoryWeight(uint64_t CardBytes, uint32_t Width, uint32_t Height);
    // A sound's, from what its header says, and back
    Weight SoundWeight(const SoundHeader& H);
    SoundRow RowOf(const Weight& W);
    // What an asset of weight W writes in Formats (FormatsNow()'s): exact for an image in DDS (its
    // shape known) and a sound whose SoundBytes is; else a typical ratio (Exact false); 0 when its
    // weight is not known
    uint64_t Predict(Kind K, const Weight& W, const std::string& Formats, bool& Exact);

    struct Calibration
    {
        std::string Formats;   // FormatsNow() when measured
        std::string Settings;  // the game's settings then (ExportOptions::Settings), "key=value;..."
        std::map<Kind, Sample> Kinds;
        // the zones whose sounds were read from their headers, by name (as the index lists them),
        // kept from one count to the next; SoundHeaders: the game's readers give them
        std::map<std::string, std::vector<SoundRow>> Sounds;
        std::map<std::string, uint64_t> SoundFootprints;
        bool SoundHeaders = false;
        bool Empty() const { return Kinds.empty() && Sounds.empty(); }
    };

    // The format settings an export writes with: image and sound formats, the model and anim
    // formats ticked, all LODs, model textures ("exportimg=PNG;exportsnd=WAV;export_castmdl=true;...")
    std::string FormatsNow();

    // Kept beside the install index (KeptFile(..., ".sizes")); false when there is none
    bool Load(const std::string& File, Calibration& Out);
    void Save(const std::string& File, const Calibration& C);

    // A sample of each of Kinds the game exports (Map left out: a level takes minutes), taken from
    // one of Candidates (Ix's zones; all of them when empty): for a kind the index lists, the
    // smallest zone holding enough of it, a dozen of its assets spread over their names; for
    // another, a zone of middling footprint, its kind exported until 24 assets or 20 seconds.
    // A level (Map) is exported whole from the smallest of Levels (the zones known to be levels;
    // empty: unknown, a few zones tried). Sounds are first read from their headers, every candidate
    // zone in a dry run (those Previous read already kept), for 2 minutes at most; a game whose
    // readers give no header has them sampled as another kind. Step says what is measured; Cancel
    // stops between and within samples.
    Calibration Measure(const IDiskGame& Game, const ExportOptions& Options, const std::string& Install, const ZoneIndex& Ix,
                        const std::vector<size_t>& Candidates, const std::set<size_t>& Levels, const std::set<Kind>& Kinds,
                        const std::function<void(const std::string&)>& Step, const std::atomic<bool>* Cancel,
                        const Calibration* Previous = nullptr);

    // What the install index's weights make of the picked zones' assets of a kind, each once
    // (WeighPicks): how many there are, how many it weighed, what those write (Predict)
    struct Weighed
    {
        size_t Assets = 0, Known = 0;
        uint64_t Bytes = 0;
        bool Exact = true;   // each one's Predict exact
    };
    // ... of Kinds over Zones of Ix, in Formats (FormatsNow()'s). An image or a sound only: what
    // another kind writes is not known from its weight yet (Predict gives its bytes in the zone).
    // Spoken: a sound of another language (its name's first folder, Languages::OfAssetName) left out
    std::map<Kind, Weighed> WeighPicks(const ZoneIndex& Ix, const std::vector<size_t>& Zones, const std::set<Kind>& Kinds,
                                       const std::string& Formats, const Languages::Choice& Spoken = {});

    // What an estimate is for: the kinds ticked and the zones picked
    struct Picks
    {
        std::map<Kind, Weighed> Weights;     // WeighPicks', when the index weighs them
        std::set<Kind> Kinds;
        std::map<Kind, size_t> Counts;       // the install index's counts over the picked zones
        size_t Zones = 0;
        uint64_t Footprint = 0;              // the picked zones' footprints summed
        size_t Levels = 0;                   // the picked zones that are levels (a level name), and theirs
        uint64_t LevelFootprint = 0;
        bool LevelsKnown = false;            // the game names its levels: else every picked zone may be one
        std::map<std::string, std::string> Settings;   // the game's settings (map textures...)
        std::string Formats;                 // FormatsNow()
        std::vector<std::pair<std::string, uint64_t>> Picked;   // the picked zones and their footprints
    };
    struct KindEstimate
    {
        uint64_t Bytes = 0;
        bool Approx = false;   // not counts times a measured size in the format chosen
        bool Unknown = false;  // not measured at all
        std::string Basis;     // how, in a line
    };
    struct Estimate
    {
        std::map<Kind, KindEstimate> Kinds;
        uint64_t Bytes = 0;
        bool Approx = false, Unknown = false;
    };
    Estimate Compute(const Calibration& C, const Picks& S);

    // 1536 as "1.5 KB", 12.4e9 as "11.5 GB" (binary units, as Explorer shows them)
    std::string Text(uint64_t Bytes);
    // The free bytes of the drive Folder is on (its nearest existing parent); 0 when unknown
    uint64_t FreeBytes(const std::string& Folder);
}
