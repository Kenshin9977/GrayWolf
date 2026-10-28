#pragma once

// A zone's sound aliases as a Black Ops III Mod Tools alias CSV (share/raw/sound/aliases): volume,
// pitch, distances, looping, 2D/3D, bus and groups, secondaries, variants, subtitles, beside the
// audio. A game's reader fills game-neutral rows (one per variant, the alias name repeated) in
// the CSV's units through the conversions below; what it cannot fill stays empty. Columns in the
// mod tools' order; conversions after HydraX's BO3 exporter and OAT's T6 dumper (both GPL-3.0).
// Written with the sound kind when sound_aliases is on: sounds/<zone>.csv, FileSpec relative to
// that folder (standing for the mod tools' sound_assets), backslashes, the exported extension.

#include "DiskGame.h"
#include "ExportNaming.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace SoundAliasCsv
{
    // The columns, in the mod tools' order
    enum class Col : uint8_t
    {
        Name, Behavior, Storage, FileSpec, FileSpecSustain, FileSpecRelease, Template, Loadspec,
        Secondary, SustainAlias, ReleaseAlias, Bus, VolumeGroup, DuckGroup, Duck, ReverbSend,
        CenterSend, VolMin, VolMax, DistMin, DistMaxDry, DistMaxWet, DryMinCurve, DryMaxCurve,
        WetMinCurve, WetMaxCurve, LimitCount, LimitType, EntityLimitCount, EntityLimitType, PitchMin, PitchMax,
        PriorityMin, PriorityMax, PriorityThresholdMin, PriorityThresholdMax, AmplitudePriority, PanType, Pan, Futz,
        Looping, RandomizeType, Probability, StartDelay, EnvelopMin, EnvelopMax, EnvelopPercent, OcclusionLevel,
        IsBig, DistanceLpf, FluxType, FluxTime, Subtitle, Doppler, ContextType, ContextValue,
        ContextType1, ContextValue1, ContextType2, ContextValue2, ContextType3, ContextValue3, Timescale, IsMusic,
        IsCinematic, FadeIn, FadeOut, Pauseable, StopOnEntDeath, Compression, StopOnPlay, DopplerScale,
        FutzPatch, VoiceLimit, IgnoreMaxDist, NeverPlayTwice, ContinuousPan, FileSource, FileSourceSustain, FileSourceRelease,
        FileTarget, FileTargetSustain, FileTargetRelease, Platform, Language, OutputDevices, PlatformMask, WiiUMono,
        StopAlias, DistanceLpfMin, DistanceLpfMax, FacialAnimationName, RestartContextLoops, SilentInCPZ, ContextFailsafe, GPAD,
        GPADOnly, MuteVoice, MuteMusic, RowSourceFileName, RowSourceShortName, RowSourceLineNumber,
        Count
    };
    constexpr size_t Columns = static_cast<size_t>(Col::Count);
    const char* ColumnName(Col C);

    // One row: one variant of an alias, its cells as the CSV holds them ("" = not given)
    struct Alias
    {
        std::array<std::string, Columns> Cells;
        std::string& operator[](Col C) { return Cells[static_cast<size_t>(C)]; }
        const std::string& operator[](Col C) const { return Cells[static_cast<size_t>(C)]; }
    };

    // Conversions into the CSV's units; the u16/byte ones print the fewest decimals that convert
    // back to the stored value (OAT's way), so a value typed in the mod tools reads as typed.
    // a linear gain (0..1) as dBSPL, 0..100: 100 + 20 log10(gain), 0 below -95 dB (volumes, sends)
    std::string Volume(double Linear);
    // a 16-bit linear gain (65535 = 1), as Volume
    std::string Volume16(uint16_t Value);
    // a pitch ratio (1 = unchanged) as cents, 1200 log2(ratio), within the mod tools' -2400..1200
    // (Clamped set when it was not)
    std::string Cents(double Ratio, bool* Clamped = nullptr);
    // a distance in inches, rounded, within the 0..131070 BO3 stores (u16 halved)
    std::string Distance(double Inches, bool* Clamped = nullptr);
    // a u16 pitch whose ratio 1 is Scale (BO2, BO3: 32767), as cents
    std::string CentsU16(uint16_t Value, double Scale);
    // a byte as a 0..1 fraction (probability, occlusion, priority thresholds)
    std::string Fraction(uint8_t Value);
    // a plain number, the fewest decimals (at most 4)
    std::string Number(double Value);
    inline std::string YesNo(bool On) { return On ? "yes" : "no"; }
    // an enum value as the name a table gives it, "" when out of the table
    template <size_t N>
    std::string Named(uint32_t Value, const char* const (&Table)[N])
    {
        return Value < N ? Table[Value] : std::string();
    }

    // A relocated zone as an alias reader reads it (a ZonekitZone's Image, Off and StringAt)
    struct ZoneView
    {
        const ByteView* Image = nullptr;
        std::function<int64_t(uint64_t Address)> Off;            // zone offset, -1 outside
        std::function<std::string(uint64_t Address)> String;     // a string pointer's text, "" outside
        int PointerSize = 8;

        template <class T>
        T At(int64_t Pos) const { return ExportNaming::At<T>(*Image, Pos); }
        uint64_t Pointer(int64_t Pos) const { return PointerSize == 4 ? At<uint32_t>(Pos) : At<uint64_t>(Pos); }
        int64_t Follow(int64_t Pos) const { return Off(Pointer(Pos)); }        // where the pointer at Pos points, -1
        std::string Text(int64_t Pos) const { return String(Pointer(Pos)); }   // the string the pointer at Pos names
        // N chars at Pos up to the first zero
        std::string Chars(int64_t Pos, size_t N) const;
    };

    // What a game's reader gives back
    struct Rows
    {
        std::vector<Alias> Aliases;
        std::map<std::string, size_t> Counters;   // the reader's alias_* measures
    };

    // The CSV text: the header and the rows, CRLF, in the order given (a reader gives the zone's)
    std::string Text(const std::vector<Alias>& Aliases);

    // sound_aliases is on for this run
    bool Wanted(const DiskGame::Context& C);

    // Writes <sounds>/<Zone>.csv when there is a row and adds to the zone's counters the reader's
    // and the writer's: alias_csv_files, alias_rows, alias_names, alias_cells (cells filled),
    // alias_files (rows whose FileSpec names a file beside the CSV) and alias_files_elsewhere
    // (a FileSpec the export does not hold: another zone's, or a sound not read). A run limited to
    // some assets (Context::Only: a dependency pulled from this zone) keeps the rows playing a file
    // it wrote.
    void Write(const DiskGame::Context& C, DiskGame::Totals& T, const std::string& Zone, const Rows& R);
}
