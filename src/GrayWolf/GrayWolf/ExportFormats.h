#pragma once

// The formats an export writes models, anims, images and sounds in, as the GUI offers them: the
// settings the writers read (LiveExport, ExportWriters) and the CLI's --model-format,
// --anim-format, --image-format and --audio-format set for one run (ApplyExportOverrides).
// Models and anims: one on/off setting per format, several may be on. Images and sounds: one
// setting holding the format's name.

namespace ExportFormats
{
    struct Format
    {
        const char* Setting;   // the on/off setting ("export_castmdl"), or the family setting's value ("PNG")
        const char* Label;
        const char* Tip;
    };

    inline constexpr Format Models[] = {
        { "export_castmdl", "Cast", "Cast (.cast): bones, meshes, materials; Blender, Maya and 3ds Max through the Cast plugins (default)" },
        { "export_semodel", "SEModel", "SEModel (.semodel): SETools' format, read by its Maya and Blender plugins" },
        { "export_gltf", "glTF", "glTF (.gltf + .bin): read by most 3D tools" },
        { "export_glb", "GLB", "glTF binary (.glb): one file, read by most 3D tools" },
        { "export_obj", "OBJ", "Wavefront OBJ (.obj + .mtl): meshes only, no bones" },
        { "export_ma", "Maya", "Maya ASCII (.ma + .mel)" },
        { "export_xna", "XNALara", "XNALara / XPS (.ascii)" },
        { "export_smd", "SMD", "Valve SMD (.smd)" },
        { "export_xmexport", "XMODEL_EXPORT", "Call of Duty mod tools' text format (.xmodel_export)" },
        { "export_xmbin", "XMODEL_BIN", "Call of Duty mod tools' binary format (.xmodel_bin)" },
    };

    inline constexpr Format Anims[] = {
        { "export_castanim", "Cast", "Cast (.cast) (default)" },
        { "export_seanim", "SEAnim", "SEAnim (.seanim): SETools' format" },
        { "export_directxanim", "XANIM_EXPORT", "Call of Duty mod tools' text format (no extension)" },
    };

    // setting exportimg, PNG when unset (ExportWriters::ImageFormatSetting)
    inline constexpr const char* ImageSetting = "exportimg";
    inline constexpr const char* ImageDefault = "PNG";
    inline constexpr Format Images[] = {
        { "PNG", "PNG", "Decoded to PNG (default)" },
        { "TGA", "TGA", "Decoded to TGA (uncompressed, the largest)" },
        { "DDS", "DDS", "As the game stores them: block compressed, with their mips (the smallest, the fastest)" },
    };

    // setting exportsnd, Opus when unset (ExportWriters::SoundFormatSetting)
    inline constexpr const char* SoundSetting = "exportsnd";
    inline constexpr const char* SoundDefault = "OPUS";
    inline constexpr Format Sounds[] = {
        { "OPUS", "Opus (default: small, lossy)",
          "The game's own Opus packets kept as they are when it stores Opus; any other sound decoded and encoded "
          "(64 kb/s a channel, 48 kHz). About a tenth of WAV's size." },
        { "WAV", "WAV (lossless)", "As the game keeps them (PCM, ADPCM, MP3), or decoded to 16-bit PCM. The largest files." },
        { "FLAC", "FLAC (lossless, compressed)", "Lossless, about half of WAV's size." },
    };
}
