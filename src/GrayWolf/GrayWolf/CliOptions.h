#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Options accepted by the headless mode
struct CliOptions
{
    bool ExportAll = false;
    bool SelfTest = false;
    bool ListInstalls = false;     // list-installs
    std::string DecompressFF;
    std::string Identify;
    std::string Bo4DecryptHex;   // hex bytes, e.g. "bd07feccafb0b4a900" (ID+Size+ciphertext+NUL)
    std::string Bo4DecryptDiskHex;   // same encoding, routed through the on-disk cipher instead
    std::string Bo3Images;   // --bo3-images <ff>: BO3 zone images -> PNG, pixels from the .xpaks
    std::string Bo3Xpaks;    // --bo3-xpaks <dir>: where the packages are (default: beside the ff)
    std::string Bo3DumpXpakEntries; // --bo3-dump-xpak-entries <f1.xpak,f2.xpak,...>: dumps every
                                     // entry >= --bo3-xpak-min-mb to --out (Bo3XpakDump.h)
    int Bo3XpakMinMB = 2;    // --bo3-xpak-min-mb <n>: size floor for --bo3-dump-xpak-entries
    std::string Bo3DecryptMovie; // --bo3-decrypt-movie <zone>: Salsa20 cinematic decrypt, see
                                  // Bo3MovieDecrypt.h - key comes from the zone's own GSC, never hardcoded
    bool Salsa20SelfTest = false; // --salsa20-selftest: verify the hand-written cipher vs a test vector
    std::string IndexAssets;
    std::string QueryAssets;
    std::string FilterFile;
    std::string Install;
    std::string PreviewDir;
    std::string ModelsDir;
    std::string ContactSheet;
    std::string DecodeIwd;
    std::string CascList;    // --casc-list <dir>: list every file in a CASC storage matching --casc-pattern
    std::string XsubExtract; // xsub-extract <dir> --keys <hex,...>: Cold War .xsub objects, decompressed, into --out <dir>
    std::string XsubKeys;
    std::string XsubIndex;   // xsub-index <dir>: every Cold War .xsub's hash table as TSV into --out
    std::string XpakIndex;   // xpak-index <dir>: every .xpak's index (key, offset, size) as TSV into --out
    std::string CascPattern; // --casc-pattern <glob>: defaults to *.sab* when omitted
    std::string CascExtractDir;  // --casc-extract <dir> <name> <outfile>: pull one file out of CASC as-is
    std::string CascExtractName;
    std::string CascExtractOut;
    std::string ZoneDecompressIn;  // zone-decompress <in.ff> <outfile>: FastFile::Decompress a local fastfile to raw zone bytes
    std::string ZoneDecompressOut;
    std::string Bo4ExportLocalizeDiskIn;  // bo4-export-localize-disk <in.bin> <outfile>: offline Localize export
    std::string Bo4ExportLocalizeDiskOut;
    std::string Bo4ExportRawStringsDiskIn;  // bo4-export-rawstrings-disk <in.bin> <outfile>: offline dialogue/rawstring export
    std::string Bo4ExportRawStringsDiskOut;
    std::string RenderModel;
    std::string DumpModel;   // --dump-model: print a .semodel's bones/bbox
    std::string DumpGlb;     // --dump-glb: print a .glb's PreviewGeometry stats (LoadPreviewGeometryGlb)
    std::string RenderGlbPng; // --render-glb-png <in.glb>: headless PNG render, --out sets the path
    std::string AudioFormat; // "wav" (default), "flac" or "opus" (setting exportsnd, ExportWriters::WriteSound)
    std::string ModelFormat; // --model-format: cast|semodel|obj|ma|xna|xmodel_export|xmodel_bin|smd|gltf|glb|none, comma list
    std::string AnimFormat;  // --anim-format: cast|seanim|xanim_export|none, comma list
    std::string ImageFormat; // --image-format: png|tga|dds (sets exportimg)
    std::string UiRes;       // --ui-res: 4k|1080|all, which BO4 UI resolution zones to read (4k)
    std::string MapPng;      // --map-png: on|off, PNG copies of map textures for glTF tools (off)
    std::string MapCast;     // --map-cast: on|off, the map also as a Cast scene (off)
    std::string MaterialGdt; // --material-gdt: on|off, BO3 Mod Tools material GDT entries for the exported materials (off)
    std::string MapRadiant;  // --map-radiant: on|off, the map also as a Radiant .map for the mod tools (off)
    std::string SoundAliases;  // --sound-aliases: on|off, the sound kind also writes the zone's aliases as a BO3 mod tools CSV (off)
    std::string MapContent;  // --map-content: all|terrain, the map's terrain alone (all)
    std::string MapCollision;// --map-collision: on|off, the map's collision also as <map>_collision.json (off)
    std::string Decompile;   // --decompile[=on|off]: compiled scripts also decompiled (off)
    std::string MapTextures; // --map-textures: full|2k|1k, map textures above it copied smaller (full)
    std::string Language;    // --language: english|french|...: CoD1/CoD2, whose localized archives win (english)
    std::string Unit;        // --unit: Centimeter|Meter|Inch (model export scale)
    std::string NormalMap;   // --normal-map: none|opengl (flip normal-map green for Blender/DCC)
    int Turntable = 0;       // --turntable N: render N views around the model into one contact sheet
    std::string PreviewWindow; // --preview-window <parts>: open an interactive D3D11 orbit viewer
    std::string Ui;            // --ui <models-dir>: open the ImGui+D3D11 asset browser shell
    std::string AudioProbe;    // --audio-probe <file>: decode via AudioPlayer and print duration/rate/peaks
    std::string XAnimBinDump;  // --xanimbin-dump <file>: parse a Mod Tools .xanim_bin, print the resolved record stream
    std::string XAnimBinSelfTest; // --xanimbin-selftest <file>: assemble Layer 2, report bone/frame/notify counts
    std::string XAnimBinToSeAnimIn;  // --xanimbin-to-seanim <in.xanim_bin> <out.seanim>
    std::string XAnimBinToSeAnimOut;
    std::string XAnimBinExportDirIn;  // --xanimbin-export-dir <xanim_export dir> <out dir>: bulk
    std::string XAnimBinExportDirOut; // .xanim_bin -> .seanim, mirroring the source tree, so the
                                       // result is browsable with the existing --ui <dir>
    bool Lods = false;       // --lods: export every available model LOD
    bool Gpu = false;        // --gpu: use the offscreen D3D11 renderer for model previews
    bool CerberusSelfTest = false; // --cerberus-selftest: verify ported CRC32/Salsa20
    bool SeAnimSelfTest = false;   // --seanim-selftest: round-trip a synthetic .seanim + verify pose eval
    bool CastSelfTest = false;     // cast-selftest: a synthetic model/anim as .cast and SE, read back alike
    std::string MakeTestSeAnim;    // --make-test-seanim <bonename>: writes a spinning test .seanim, --out sets the path
    std::string RenderAnimModel;   // --render-anim <model>;<anim>;<frame>: skinned PNG at that frame, --out sets the path
    std::string Coverage;    // coverage <ff>: per-type disk-extraction coverage for one fastfile
    std::string OatExtract;  // oat-extract <ff>: in-process old-CoD extract (OAT's DumpZone)
    bool OatList = false;    // --oat-list: list the fastfile's assets instead of extracting
    std::string ActsExtract; // --acts-extract <ff>: extract a modern-CoD fastfile via ate47's acts
    std::string ActsExe;     // --acts-exe <exe>: path to acts.exe (else auto-located)
    std::string GscToolExe;  // --gsc-tool-exe <exe>: path to gsc-tool.exe (else auto-located)
    std::string GameExe;     // --game-exe <exe>: the game's dumped exe (script string decryption)
    std::string ActsHandler; // --acts-handler <h>: force a game handler (bo4/cw/mwiii/bo6/...)
    std::string OatFx;       // --oat-fx <ff>: per-element FX/material/blend detail, T5+T6, no game needed
    std::string OatXModelMtl; // --oat-xmodel-materials <ff>: per-xmodel-surface material/blend, T5+T6
    std::string OatWeapon;   // --oat-weapon <ff>: compiled WeaponDef splash/damage-falloff fields, T5+T6
    std::string OatRawfiles; // --oat-rawfiles <ff>: dump .gsc/.csc (T5) / plain rawfiles+.atr (T6) source to --out
    std::string OatFxExport; // --oat-fx-export <ff>: real per-effect JSON export to --out, T5+T6
    std::string GenDisc;     // --gen-disc <out.XMODEL_EXPORT>: flat N-gon disc, radius/segments/material below
    float GenDiscRadius = 50.0f;    // --gen-disc-radius <units>
    float GenDiscInnerRadius = 0.0f; // --gen-disc-inner-radius <units>: >0 makes an annulus (ring) instead of a filled disc
    int GenDiscSegments = 32;       // --gen-disc-segments <n>
    std::string GenDiscMaterial = "mc/gfx_crater_snow_grenade"; // --gen-disc-material <name>: existing alpha material to reference, no new asset needed
    std::string OatModelFmt; // --oat-model-format <fmt>: gltf|glb|obj|xmodel_export|xmodel_bin
    std::string OatImageFmt; // --oat-image-format <fmt>: dds|iwi
    bool OatGdt = false;     // --oat-gdt: also emit a source_data/<zone>.gdt
    bool WithSounds = false;  // --with-sounds: also extract the game's sounds OAT can't dump (T5/BO1)
    std::string GltfSelfTest; // --gltf-selftest <out.glb>: write a tiny GLB scene to verify the writer
    std::string ExportMapGlb;  // --export-map-glb <ff>: export a T5 map's GfxWorld to a Blender .glb
    bool MapNoTex = false;     // --map-no-tex: skip embedding textures (much smaller, geometry preview)
    std::string OpenMap;           // open-map <map or folder>: a map export opened in Blender (BlenderLauncher.h)
    bool OpenMapReimport = false;  // open-map --reimport: run the import script again, even with a .blend
    bool OpenMapBackground = false;// open-map --background: import and save the .blend without a window
    std::string NameFilter;      // --name-filter a,b,c: case-insensitive substrings, match any
    bool UpdateNameDb = false;   // --update-name-db: fetch echo000/cod-name-db's CSVs into package_index\*.wni
    std::string NameDbDir;       // --name-db-dir <dir>: override the package_index output dir
    bool Update = false;         // update: hash packages from the latest release, app reported
    bool UpdateCheck = false;    // update --check: report only
    bool UpdateApp = false;      // update --app: also replace GrayWolf once this run exits
    bool UpdateForce = false;    // update --force: packages even when not newer / hand-built / linked
    std::string UpdatePackageDir;// update --package-dir <dir>
    std::string UpdateRepo;      // update --repo owner/name
    std::string ApplyUpdate;     // apply-update <stage> (internal): the helper run
    std::string ApplyTarget;     // apply-update --target <dir>
    uint32_t ApplyWaitPid = 0;   // apply-update --wait-pid <pid>
    bool ApplyRelaunch = false;  // apply-update --relaunch
    std::string ImportHashCsv;   // --import-hash-csv <csv>: convert a local "hash,name" CSV to .wni
    std::string ImportHashName;  // paired with the above - the output .wni stem (no extension)
    std::string SabProbe;        // --sab-probe <file>: run SABSupport on a bank
    std::string Cache;
    std::string Out;
    bool Strings = false;
    std::string VerifyOpus;
    std::string File;
    std::string Report;
    std::string Type;
    uint32_t Threads = 0;
    uint32_t Limit = 0;
    // zone-walk / bo4-walk (CliCommands.ZoneWalk.cpp): zone list file, zonekit profile (types
    // and tables), or explicit reader table files and type names file (they override the profile)
    std::string ZoneWalkList;
    std::string ZoneWalkProfile;
    std::vector<std::string> ZoneWalkTables;
    std::string ZoneWalkTypes;
    bool ZoneWalkLayout = false;   // zone-walk --layout: also each read's stream memory place
    std::string ZoneWalkAssetsOut; // zone-walk --assets-out <dir>: per zone, its asset instances and image mip records
    // disk-export, its aliases and the games' own commands (GameCommand.h): input by command name
    std::map<std::string, std::string> DiskExportIn;
    std::string DiskGameId;   // disk-export --game (DiskGame.h); detected when empty
    bool EstimateKept = false;  // disk-estimate --kept: the samples kept from the last run, not measured again
    std::string EstimateZones; // disk-estimate --picked: the zones picked, comma-separated (as the panel lists them)
};

