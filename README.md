# GrayWolf

A Call of Duty asset extractor for Windows. GrayWolf reads a game **offline, from its files**:
no running game is needed. It exports models, animations, images, sounds, raw files, data tables,
localized strings, scripts, effects and whole maps, from Call of Duty (2003) to Black Ops 7.

- **Zones read by tables**: each game's fast files are walked by *zonekit* reader tables
  (`zonekit/profiles/<game>`), generated from the game's own zone loaders and checked against
  them; the zone is then served to GrayWolf's asset readers.
- **Two front ends, one use case**: a Dear ImGui asset browser (Load Game panel, model, anim and
  map preview, image thumbnails, an audio player) and a command line
  (`GrayWolf.exe disk-export --game <id> --install <dir> --out <dir>`, `GrayWolf.exe --help`).
  Both run the same export.
- **Hashed names back**: name packages (`package_index`) turn the hashed asset names of the recent
  games back into names; they update from the releases (see [Updates](#updates)).

## Download

**[Download the latest GrayWolf](https://github.com/Kenshin9977/GrayWolf/releases/latest)**: take
`GrayWolf-<version>-x64.zip` from the release's assets, unzip it anywhere and run `GrayWolf.exe`
(Windows 10 or 11, x64; the exe is signed). It then updates itself from the releases (see
[Updates](#updates)).

Building it yourself: see [BUILDING.md](BUILDING.md).

## Games and what they export

✓ exported, ~ partly (see the notes), – not exported. Choose kinds with `--type` (`model`,
`anim`, `image`, `sound`, `rawfile`, `data`, `localize`, `map`, `script`, `gdt`, `fx`; the last three
only when named).

| Game | `--game` | Models | Anims | Images | Sounds | Rawfiles | Data | Localize | Maps | Scripts | GDT | FX |
|---|---|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
| Call of Duty | `cod1` | ~ | ✓ | ✓ | ✓ | – | – | ✓ | ✓ | ✓ | – | – |
| Call of Duty 2 | `cod2` | ~ | ✓ | ✓ | ✓ | – | – | ✓ | ✓ | ✓ | – | – |
| Call of Duty 4: Modern Warfare | `cod4` | ✓ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ | ✓ | ✓ | – | – |
| World at War | `waw` | ✓ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ | ✓ | ✓ | – | – |
| Modern Warfare 2 | `mw2` | ✓ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ | ✓ | ✓ | – | – |
| Modern Warfare 3 | `mw3` | ✓ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ | ✓ | ✓ | – | – |
| Black Ops | `bo1` | ✓ | ✓ | ✓ | ~ | ✓ | – | ✓ | ✓ | ✓ | – | ✓ |
| Black Ops II | `bo2` | ✓ | ✓ | ✓ | ✓ | ✓ | – | ✓ | ✓ | ✓ | – | ✓ |
| Ghosts | `ghosts` | ✓ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ | ✓ | ✓ | – | – |
| Call of Duty Online | `codol` | ✓ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ | ✓ | – | – | – |
| Advanced Warfare | `aw` | ✓ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ | ✓ | ✓ | – | – |
| Black Ops III | `bo3` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| Infinite Warfare | `iw` | ~ | ✓ | ✓ | ✓ | ~ | ~ | ✓ | ~ | ✓ | – | – |
| Modern Warfare Remastered | `mwr` | ✓ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ | ✓ | ✓ | – | – |
| WWII | `ww2` | ~ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ | ~ | ✓ | – | – |
| Black Ops 4 | `bo4` | ✓ | ✓ | ✓ | ~ | ✓ | ~ | ~ | ✓ | ~ | ~ | – |
| Modern Warfare (2019) | `mw2019` | ✓ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ | ~ | ~ | – | – |
| Black Ops Cold War | `coldwar` | ✓ | ✓ | ✓ | – | ~ | ~ | ~ | ✓ | ✓ | ~ | ~ |
| Modern Warfare III (2023) | `mwiii`, `mwiii_sp` | ~ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ | ~ | ✓ | – | – |
| Black Ops 6 | `bo6`, `bo6sp` | ✓ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ | ✓ | ✓ | – | – |
| Black Ops 7 | `bo7` | ✓ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ | ✓ | ✓ | – | – |

`_sp` / `sp` ids are the campaigns. Notes:
- **Models**: Call of Duty and Call of Duty 2 give LOD 0 only; Infinite Warfare and MWIII miss some
  models whose parts live in other zones (`_tr`); WWII misses some composite models.
- **Sounds**: Black Ops keeps a few ADPCM/WMA sounds unread; Black Ops 4 sound names are partly
  still hashes. In FLAC or Opus, a sound no decoder here reads (World at War's xWMA) is kept as it
  is.
- **Rawfiles**: the game's raw files (configs, scripts' sources, visions, animtrees, ...), inflated
  where the game compresses them, and its Lua files, as `.lua` (source) or `.luac` (bytecode, not
  decompiled). The recent games hash their names: names the name packages know, else
  `rawfile_<hash>` (with the extension the asset says, MWIII to Black Ops 7). Cold War leaves out
  its encrypted `rawfilepreproc` and `rawtextfile`; Infinite Warfare leaves out the few whose name
  the zone keeps elsewhere.
- **Data**: string tables as CSV (`data/stringtables/`; a typed cell of the recent games written as
  acts writes it: `#name` a hash, `&#name` a localized one, `TRUE`/`FALSE`...) for MW2/MW3, CoD4,
  World at War, Call of Duty Online and Ghosts to Black Ops 7 (MW2/MW3 under `data/`); Black Ops III materials, string tables, structured tables and
  DDL as JSON. Black Ops and Black Ops II data goes through `bo1-oat` / `bo2-oat` (below).
- **Localize**: Black Ops 4 and Cold War keys are partly still hashes. Modern Warfare III, Black
  Ops 6 and Black Ops 7 keep only a key's hash: `package_index\fnv1a_localize.wni` names the known
  ones (about half), the others are written `localize_<hash>`. Modern Warfare 2019, Cold War,
  Modern Warfare III, Black Ops 6 and Black Ops 7 encrypt the texts: they are decrypted only with
  the game's string cipher table (`zonekit/profiles/<game>/strings.cipher.txt`, not shipped),
  written empty without it.
- **Maps**: world geometry, static models, entities, lights, sky, collision (`--map-collision`) and,
  for Cold War, Black Ops 6/7 and MWIII, terrain. MW2019 has no scriptables and no collision;
  MWIII, BO6 and BO7 have no collision; MWIII terrain is its coarsest level only; WWII maps keep
  one UV layer; Infinite Warfare campaign maps miss the vertices of their `_tr` zones.
- **Scripts**: GSC/CSC, as compiled files or decompiled (`--decompile`) through
  [acts](https://github.com/ate47/atian-cod-tools) and [gsc-tool](https://github.com/xensik/gsc-tool)
  (both on `PATH`, or set in `graywolf.json`). The oldest games ship their scripts as source.
  Black Ops 4 names are partly hashes; MW2019 keeps its encrypted string literals.
- **GDT**: Black Ops III Mod Tools entries: Black Ops III's own assets, Black Ops 4 and Cold War
  weapons ported.
- **FX**: each effect's elements, timings and what they draw, as JSON. Cold War has no timings.

Black Ops, Black Ops II, CoD4, World at War, MW2 and MW3 can also go through
[OpenAssetTools](https://github.com/Laupetin/OpenAssetTools): `bo1-oat`, `bo2-oat`, `cod4-oat`,
`waw-oat`, `mw2-oat`, `mw3-oat` (models, anims, images, sounds, rawfiles, data, localize; maps for
Black Ops and Black Ops II).

### What each game reads

| Game | Input |
|---|---|
| CoD, CoD 2 | the install folder, or one `.pk3` / `.iwd` |
| CoD4, World at War, Black Ops | a `.ff` or a zone folder; images and sounds from `main\*.iwd` |
| Black Ops II | a `.ff` or a zone folder; images from the `.ipak`, sounds from `sound\` |
| MW2, MW3 | a `.ff` or a zone folder (multiplayer zones included); images from `main\*.iwd` |
| Call of Duty Online | the install folder, an `.ifs` archive or a `.ff` |
| Ghosts, Advanced Warfare, Infinite Warfare, MW Remastered, WWII | a `.ff` or the install folder, with the `.pak` / `.toc` files beside the zones |
| Black Ops III | a `.ff`, with the `.xpak` files beside it |
| Black Ops 4, Cold War | the Battle.net storage: `casc:<zone>.ff --install <game>` |
| Modern Warfare (2019) | a `.ff`, or `casc:<pattern> --install <game>` |
| MWIII, Black Ops 6, Black Ops 7 | a `.ff` with `--install <game>` for the streamed packages |

`--cache <dir>` keeps what a run builds once (the install index that resolves an asset another
zone defines, material books) for the next runs.

## Export formats

- **Models**: [Cast](https://github.com/dtzxporter/cast) by default (Blender and Maya plugins from
  its releases), or SEModel, glTF/GLB, OBJ, Maya `.ma`, XNALara, SMD, xmodel_export/xmodel_bin
  (`--model-format`). Each model gets a `<model>.materials.json` sidecar (textures per slot,
  shading settings); Cast files carry the same materials.
- **Anims**: Cast, SEAnim or xanim_export (`--anim-format`), with their notetracks.
- **Images**: PNG, TGA or DDS (`--image-format`).
- **Sounds**: WAV (the default), FLAC or Opus (`--audio-format`, or **Settings > Sounds**). Opus
  keeps the Opus of MWIII, Black Ops 6/7 and MW2019 as it is (repackaged in Ogg, not re-encoded);
  the other games' sounds are encoded (64 kb/s a channel, 48 kHz).
- **Maps**: a `.glb` with a Blender import script (instanced props, materials), or a Cast scene
  (`--map-cast`); `--map-radiant` writes a Radiant `.map` with its models, `--map-textures
  full|2k|1k` sizes the textures.
- **Black Ops III Mod Tools**: `--material-gdt` writes a GDT for the exported materials,
  `--sound-aliases` an alias CSV, `--type gdt` asset entries.

A `graywolf.json` from an earlier version keeps its formats; a format given on the command line
holds for that run only.

## Other tools in the command line

`GrayWolf.exe <command> --help` for each one.

- `list-installs`: the Call of Duty installs found on this machine.
- `identify`: a game or an asset from its name or hash. `coverage`: what one fast file gives,
  per asset type.
- Containers: `casc-list`, `casc-extract`, `xpak-index`, `xsub-index`, `xsub-extract`,
  `ifs-extract`, `zone-decompress`, `decompress-ff`, `oat-extract`.
- Rendering: `render-model` (software, `--gpu`, `--turntable N`), `render-anim`,
  `render-glb-png`, `preview-window`.
- `ui <dir>`: the asset browser on a folder of exports. `index-assets` / `query-assets`: a
  searchable index of an export and saved filters.
- Names: `update`, `update-name-db` (the community [cod-name-db](https://github.com/echo000/cod-name-db)),
  `import-hash-csv`.

## Opening a map in Blender

After a map export, the **Load Game** panel lists the exported maps under **Maps**, and the
**Viewport** shows the same buttons for a selected map: **Open in Blender** imports the map the
first time (its `<map>_import.py`) and saves `<map>.blend` beside the `.glb`; later it opens that
`.blend` directly. **Re-import** imports it again after a new export and overwrites the `.blend`.
Blender is found by itself (the `BLENDER` variable, the newest *Blender Foundation* install,
Steam, the registry, `PATH`); **Settings > Blender** sets another `blender.exe`. From the command
line: `GrayWolf.exe open-map <map .glb, .blend or folder> [--reimport] [--background]`
(`--background` imports and saves without opening Blender's window).

## Updates

At start, the GUI checks the latest release of [Kenshin9977/GrayWolf](https://github.com/Kenshin9977/GrayWolf)
in the background:

- **Hash name packages** (the `package_index` folder, which turns hashed asset names back into
  names) are downloaded, checked against the release's sha256 and swapped in. If anything fails,
  the previous files stay. A `package_index` you built yourself (one without
  `hash-packages.json`) is never overwritten: `GrayWolf.exe update --force` replaces it.
- **A newer GrayWolf** is only announced, in a bar under the menu: *Download*, then
  *Restart now*, replaces the program files (not your settings or `package_index`) once
  GrayWolf has closed. Or click *Later*.
- From the command line: `GrayWolf.exe update` updates the packages and reports a newer
  GrayWolf; `--check` only reports; `--app` also installs the newer GrayWolf.
  Exports and other commands never go online.

To turn the check off, untick **Settings > Updates > Check at startup**, set
`"auto_update": "false"` in `graywolf.json`, or set the environment variable
`GRAYWOLF_NO_UPDATE=1`. `update_repo` in `graywolf.json` picks another repository; a private one
needs a GitHub token: set `GRAYWOLF_GITHUB_TOKEN`, or tick **Use the GitHub CLI's token** (it runs
`gh auth token`). The token is sent only to the GitHub API and is never saved.

## Credits and license

GrayWolf is a modified version of [Greyhoundx](https://github.com/dest1yo/Greyhoundx) by dest1yo,
itself a fork of [Greyhound](https://github.com/Scobalula/Greyhound) by Scobalula, which is based
on Wraith Archon by DTZxPorter. It was renamed once little of the original code was left.
It uses [OpenAssetTools](https://github.com/Laupetin/OpenAssetTools) for the `*-oat` games and runs
[acts](https://github.com/ate47/atian-cod-tools) and [gsc-tool](https://github.com/xensik/gsc-tool)
to decompile scripts.

GrayWolf is distributed under the GNU General Public License v3.0, as the projects it comes from;
see [LICENSE](LICENSE).
