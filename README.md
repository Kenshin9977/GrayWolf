<p align="center">
  <img src=".github/assets/graywolf-icon.png" width="128" height="128" alt="GrayWolf icon: a grey wireframe wolf head with orange eyes on a dark tile">
</p>

<h1 align="center">GrayWolf</h1>

<p align="center">
  <b>A Call of Duty asset extractor that reads every game offline, from its files.</b>
</p>

<p align="center">
  Models, animations, images, sounds, raw files, data tables, localized strings, scripts, effects
  and whole maps, from Call of Duty (2003) to Black Ops 7. No running game needed: a Dear ImGui
  asset browser and a command line run the same export.
</p>

<p align="center">
  <a href="https://github.com/Kenshin9977/GrayWolf/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/Kenshin9977/GrayWolf?style=flat-square&color=f5a524"></a>
  <a href="https://github.com/Kenshin9977/GrayWolf/releases"><img alt="Downloads" src="https://img.shields.io/github/downloads/Kenshin9977/GrayWolf/total?style=flat-square&color=f5a524"></a>
  <a href="https://github.com/Kenshin9977/GrayWolf/actions/workflows/build.yml"><img alt="Build" src="https://img.shields.io/github/actions/workflow/status/Kenshin9977/GrayWolf/build.yml?style=flat-square"></a>
  <br>
  <img alt="License: GPL-3.0" src="https://img.shields.io/badge/license-GPL--3.0-blue?style=flat-square">
  <img alt="Written in C++" src="https://img.shields.io/badge/written%20in-C%2B%2B-00599c?style=flat-square">
  <img alt="Platform: Windows x64" src="https://img.shields.io/badge/platform-Windows%2010%20%C2%B7%2011%20x64-555?style=flat-square">
  <img alt="Games: CoD 2003 to Black Ops 7" src="https://img.shields.io/badge/games-CoD%20(2003)%20%E2%86%92%20Black%20Ops%207-2b2d31?style=flat-square">
</p>

<p align="center">
  <a href="https://github.com/Kenshin9977/GrayWolf/releases/latest"><img alt="Download GrayWolf" src="https://img.shields.io/badge/Download-GrayWolf-f5a524?style=for-the-badge&logo=github&logoColor=white" height="40"></a>
</p>

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

From the [latest release](https://github.com/Kenshin9977/GrayWolf/releases/latest)'s assets
(Windows 10 or 11, x64; signed), take either:

- **The installer**, `GrayWolf-<version>-x64-setup.exe`: installs for your user only, no
  administrator rights, in `%LocalAppData%\Programs\GrayWolf`, with a Start menu entry and an
  uninstaller (your `graywolf.json` settings are kept when uninstalling).
- **The portable zip**, `GrayWolf-<version>-x64.zip`: unzip it anywhere you can write to (not
  `Program Files`) and run `GrayWolf.exe`.

Either way, GrayWolf then updates itself from the releases (see [Updates](#updates)).

Building it yourself: see [BUILDING.md](BUILDING.md).

## Games and what they export

✓ exported, ~ partly (see the notes), – not exported. Choose kinds with `--type` (`model`,
`anim`, `image`, `sound`, `rawfile`, `data`, `localize`, `map`, `script`, `gdt`, `fx`; the last three
only when named; the data dumps [below](#data-dumps) too).

| Game | `--game` | Models | Anims | Images | Sounds | Rawfiles | Data | Localize | Maps | Scripts | GDT | FX |
|---|---|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
| Call of Duty | `cod1` | ✓ | ✓ | ✓ | ✓ | – | – | ✓ | ✓ | ✓ | – | ✓ |
| Call of Duty 2 | `cod2` | ✓ | ✓ | ✓ | ✓ | – | – | ✓ | ✓ | ✓ | – | ✓ |
| Call of Duty 4: Modern Warfare | `cod4` | ✓ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ | ✓ | ✓ | – | ✓ |
| World at War | `waw` | ✓ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ | ✓ | ✓ | – | ✓ |
| Modern Warfare 2 | `mw2` | ✓ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ | ✓ | ✓ | – | ✓ |
| Modern Warfare 3 | `mw3` | ✓ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ | ✓ | ✓ | – | ✓ |
| Black Ops | `bo1` | ✓ | ✓ | ✓ | ~ | ✓ | ~ | ✓ | ✓ | ✓ | – | ✓ |
| Black Ops II | `bo2` | ✓ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ | ✓ | ✓ | – | ✓ |
| Ghosts | `ghosts` | ✓ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ | ✓ | ✓ | – | ✓ |
| Call of Duty Online | `codol` | ✓ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ | ✓ | – | – | ✓ |
| Advanced Warfare | `aw` | ✓ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ | ✓ | ✓ | – | ✓ |
| Black Ops III | `bo3` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| Infinite Warfare | `iw` | ~ | ✓ | ✓ | ✓ | ~ | ~ | ✓ | ✓ | ✓ | – | ✓ |
| Modern Warfare Remastered | `mwr` | ✓ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ | ✓ | ✓ | – | ✓ |
| WWII | `ww2` | ✓ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ | ~ | ✓ | – | ✓ |
| Black Ops 4 | `bo4` | ✓ | ✓ | ✓ | ~ | ✓ | ~ | ~ | ✓ | ~ | ~ | ✓ |
| Modern Warfare (2019) | `mw2019` | ✓ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ | ~ | ~ | – | ~ |
| Black Ops Cold War | `coldwar` | ✓ | ✓ | ✓ | ~ | ~ | ~ | ~ | ✓ | ✓ | ~ | ~ |
| Modern Warfare III (2023) | `mwiii`, `mwiii_sp` | ~ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ | ~ | ✓ | – | ~ |
| Black Ops 6 | `bo6`, `bo6sp` | ✓ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ | ✓ | ✓ | – | ~ |
| Black Ops 7 | `bo7` | ✓ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ | ✓ | ✓ | – | ~ |

`_sp` / `sp` ids are the campaigns. Notes:
- **Models**: MWIII multiplayer reads a model's LODs
  from the zones that hold them (the install index lists the `_tr` zones' meshes); its campaign does
  not. A few Infinite Warfare level models have no mesh (mp_rivet: 30 of 981). WWII writes a composite
  model (weapons, heads) whole, from the parts its zone holds.
- **Sounds**: the WMA sounds (World at War's xWMA, Black Ops' WMA loaded sounds) are decoded by
  [FFmpeg](https://ffmpeg.org) when one is found (`ffmpeg.exe` on `PATH`, `ffmpeg_exe` in
  `graywolf.json` or `--ffmpeg-exe`); without it World at War's stay as the game keeps them and Black
  Ops' are left out. Black Ops 4 and Cold War sound names are partly still hashes (`xsound_<hash>`),
  and Cold War leaves out its few `rev/` vehicle engine models (not audio).
- **Rawfiles**: the game's raw files (configs, scripts' sources, visions, animtrees, ...), inflated
  where the game compresses them, and its Lua files, as `.lua` (source) or `.luac` (bytecode).
  With `--decompile` the bytecode is decompiled to `.lua` through
  [cod-luajit-decompiler](https://github.com/Nico-Posada/cod-luajit-decompiler) (Cold War, MWIII,
  Black Ops 6/7) and [CoDLuaDecompiler](https://github.com/Nico-Posada/CoDLuaDecompiler) (Havok
  Script: Black Ops III and 4, Ghosts to WWII; MW2019; the files the first one does not read), on `PATH` or
  set in `graywolf.json` (`cod_luajit_decompiler_exe`, `cod_lua_decompiler_exe`); a file neither
  reads stays `.luac`. Both name hashes from a `PackageIndex` folder beside their own exe (a junction
  to GrayWolf's `package_index` works). Black Ops 6 decompiles about a quarter of its files: on a
  Windows whose decimal separator is a comma, cod-luajit-decompiler fails every file holding a
  fractional number. The recent games hash their names: names the name packages know, else
  `rawfile_<hash>` (with the extension the asset says, MWIII to Black Ops 7). Cold War leaves out
  its encrypted `rawfilepreproc` and `rawtextfile`; Infinite Warfare leaves out the few whose name
  the zone keeps elsewhere.
- **Data**: string tables as CSV (`data/stringtables/`; a typed cell of the recent games written as
  acts writes it: `#name` a hash, `&#name` a localized one, `TRUE`/`FALSE`...) for MW2/MW3, CoD4,
  World at War, Black Ops, Black Ops II, Call of Duty Online and Ghosts to Black Ops 7 (MW2/MW3 under `data/`); Black Ops III materials, string tables, structured tables and
  DDL as JSON. Black Ops and Black Ops II's other data goes through `bo1-oat` / `bo2-oat` (below).
- **Localize**: Black Ops 4 and Cold War keys are partly still hashes. Modern Warfare III, Black
  Ops 6 and Black Ops 7 keep only a key's hash: `package_index\fnv1a_localize.wni` names the known
  ones (about half), the others are written `localize_<hash>`. Modern Warfare 2019, Cold War,
  Modern Warfare III, Black Ops 6 and Black Ops 7 encrypt the texts: they are decrypted only with
  the game's string cipher table (`zonekit/profiles/<game>/strings.cipher.txt`, not shipped),
  written empty without it.
- **Maps**: world geometry, static models, entities, lights, sky, collision (`--map-collision`) and,
  for Cold War, Black Ops 6/7 and MWIII, terrain. MW2019 places each scriptable's first model;
  MW2019, MWIII, BO6 and BO7 have no collision; MWIII terrain is its coarsest level only. WWII's
  blended surfaces carry each layer's UV set (glTF `TEXCOORD_1`.., Cast `u1`..).
- **Scripts**: GSC/CSC, as compiled files or decompiled (`--decompile`) through
  [acts](https://github.com/ate47/atian-cod-tools) and [gsc-tool](https://github.com/xensik/gsc-tool)
  (both on `PATH`, or set in `graywolf.json`). The oldest games ship their scripts as source.
  Black Ops 4 names are partly hashes; MW2019 keeps its encrypted string literals.
- **GDT**: Black Ops III Mod Tools entries: Black Ops III's own assets, Black Ops 4 and Cold War
  weapons ported.
- **FX**: each effect's elements, timings and what they draw, as JSON. Cold War's give their spawn, spawn delay
  and life span timings only; Black Ops 4's effects, materials and sounds are partly hashes.
  Spark fountains (CoD4 to WWII) play as ballistic sparks, their fields' meaning read from their
  names and values, not the games' code; trails are drawn with their profile. Ghosts', Advanced
  Warfare's, MWR's and WWII's particle simulations and vector fields are written, not played; smoke the games light from the light grid is lit by a neutral sky light;
  three WWII element types are written by number (15, 19, 20). Call of Duty Online's materials have no techset in the JSON (their blend
  factors only).
  Call of Duty and Call of Duty 2 read their `.efx` text (no compiled form): the graphs are sampled
  into the same JSON, with assumed units (rates per second, gravity in units/s², an emitter's
  density per 100 units: the JSON's `notes`); CoD1's animated shaders (`animMap`) give their frames
  and rate (played as an atlas), but the install ships none of their images; cylinders play as a
  ring wall (size and size2 its radii, length its height, along the effect's forward: assumed from
  the fields' names), camera shakes are written, not played.
  Infinite Warfare's particle systems (`vfx`) are written as their emitters, states and modules
  (graphs as curves); the preview leaves out vector fields, attractors, GPU particle sims, mesh
  spawning and collisions. Whether a vfx sprite's size is its width or half of it is not settled
  (the vertices are made on the GPU): the preview takes it as the width.
  The Assets tab plays them in 3D (FX), textured when the images are exported with them
  (`--type fx,image`, the common zone's too); `render-fx <json>` draws one offscreen.
  Black Ops 6 (multiplayer and zombies) and 7: every module of the particle systems, as JSON; the preview
  leaves out what it has no model for (GPU simulations, vector fields, meshes, collisions).
  MW2019 and MWIII (`~`): their particle systems' modules as JSON; the preview leaves out GPU
  simulations, vector fields and mesh spawns; MWIII's effect names are hashes, its blend read from
  its images' usages (its techsets are hashed).

Black Ops, Black Ops II, CoD4, World at War, MW2 and MW3 can also go through
[OpenAssetTools](https://github.com/Laupetin/OpenAssetTools): `bo1-oat`, `bo2-oat`, `cod4-oat`,
`waw-oat`, `mw2-oat`, `mw3-oat` (models, anims, images, sounds, rawfiles, data, localize; maps for
Black Ops and Black Ops II).

### Data dumps

Kinds for naming hashed assets or reading the game's numbers rather than for its assets, each
exported only when named in `--type` (in the GUI, under **Data dumps**). One file per zone, or
per asset under its zone.

| `--type` | What | CoD | CoD 2 | CoD4 | WaW | MW2 | Black Ops | MW3 | Black Ops II | Ghosts | AW | CoD Online | Black Ops III | MWR | IW | WWII | MW2019 | MWIII | Black Ops 4 | Cold War | Black Ops 6 | Black Ops 7 |
|---|---|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
| `matlinks` | the images each material names, the materials of each model (CSV) | – | – | – | – | – | – | – | – | – | – | – | – | – | – | – | – | ✓ | ✓ | – | ✓ | ✓ |
| `sndlinks` | the sound files each sound alias plays (TSV) | – | – | – | – | – | – | – | – | – | – | – | – | – | – | – | – | ✓ | ✓ | – | ✓ | ✓ |
| `refs` | what each asset of a zone references: hashes, pointers, strings (CSV) | – | – | – | – | – | – | – | – | – | – | – | – | – | – | – | – | ✓ | ✓ | ✓ | – | ✓ |
| `techsets` | techsets, their shaders (bytecode and reflection), the materials' bindings | – | – | – | – | – | – | – | – | – | – | – | – | – | – | – | – | ~ | – | – | ~ | ✓ |
| `bundles` | script bundles, the settings scripts read with `getscriptbundle` (JSON) | – | – | – | – | – | – | – | – | – | – | – | – | – | – | – | – | ~ | ✓ | ✓ | ~ | ✓ |
| `mapents` | a map's entities, one key/value pair a line (TSV) | – | – | – | – | – | – | – | – | – | – | – | – | – | – | – | – | ✓ | ✓ | ✓ | ✓ | ✓ |
| `mapinfo` | each map's settings asset (JSON) | – | – | – | – | – | – | – | – | – | – | – | – | – | – | – | – | ~ | ~ | ~ | ~ | ✓ |
| `aitypes` | each AI type and the name of its settings bundle (JSON) | – | – | – | – | – | – | – | – | – | – | – | – | – | – | – | – | ~ | ~ | ~ | ~ | ✓ |
| `scriptables` | scriptable assets as struct dumps (JSON) | – | – | – | – | – | – | – | – | – | – | – | – | – | – | – | – | ✓ | – | – | ✓ | ✓ |
| `weapons` | the weapon assets (tunables, accuracy, attachments...) as struct dumps (JSON) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | – | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| `weaponstats` | each weapon's stats in game units, with their units and formulas (CSV, JSON) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ~ | ~ | ~ | ~ | ~ | ✓ |

~: multiplayer and zombies only, not the campaign.

#### Weapons, game by game

`weapons` is the raw weapon data, `weaponstats` one table of each weapon's stats, with a README beside it
(units, formulas). Display names live in a localize zone: export it into the same `--out`.

| Game | `weapons` | `weaponstats` read from | Names in |
|---|---|---|---|
| CoD, CoD 2 | the game's weapon files (`weapons/mp/`, `weapons/sp/`) | their named fields | the `.str` files |
| CoD4, MW2, MW3 | the weapon files as OpenAssetTools writes them | their named fields | `localized_code_post_gfx_mp`, or the language folder's `common` (campaign) |
| World at War | same | same | `localized_common_mp`, or the language folder's `common` (campaign, zombies) |
| Black Ops, Black Ops II | same | same | `<lang>_code_post_gfx_mp`; zombies: `<lang>_code_post_gfx` (BO), `<lang>_code_post_gfx_zm` (BO2) |
| Black Ops III | the `gdt` kind's entries | each weapon's GDT entry | `<lang>_core_patch`, `<lang>_zm_patch` |
| Ghosts, AW, MWR, WWII | struct dumps of weapons and attachments | the game's WeaponDef (WWII: fewer columns) | |
| Infinite Warfare | same | same, plus animation times from `common_core_mp` (export it with `global_core_mp`) | |
| CoD Online | struct dumps of weapons and attachments | its WeaponDef (no start or max ammo: the game mode sets them) | `localized_code_post_gfx_mp` (guns in `common_mp`) |
| Black Ops 4 | struct dumps | the weapon layouts the `gdt` kind ports, in Black Ops III's units | `<lang>_core_ui` (guns in `core_ui`) |
| Cold War | struct dumps | same | `<lang>_core_common` (guns in `core_common`, `zm_common`, `mp_common`) |

Notes:
- **Attachments applied** (`weapon_attachments.csv`, each weapon with each attachment it takes):
  - Black Ops II: MP7 54 rounds, PDW-57 67 with Extended Mags, as published.
  - MW3, by its exe's rules: Extended Mags M4A1 45, MP7 60.
  - Call of Duty Online: also export `code_post_gfx_mp` (its attachment table) into the same `--out`.
    Extended Mags AK-47 45; Fast Mags reload x 0.7.
- **MW2019**: multiplayer only; ADS, raise and sprint times are left out until its exe confirms them.
  MW2019 and MWIII include hit multipliers; MWIII also has its damage table.
- **Black Ops 7**: an animation time the game marks as absent is an empty cell.
- **Zombies**: Black Ops 6 and Black Ops 7 add the Pack-a-Punch tiers and rarity multipliers. MWIII
  has no such data.
- **Black Ops 4 and Cold War**: `mapinfo` is their map tables, `aitypes` their AI types (no settings
  bundle in either). Their `weaponstats` are matched by name against Black Ops III's fields, not
  read from the game's code.

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
  keeps the Opus of Cold War, MWIII, Black Ops 6/7 and MW2019 as it is (repackaged in Ogg, not re-encoded);
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
  GrayWolf has closed. Or click *Later*. An installed GrayWolf updates the same way, and Windows'
  installed apps list shows the new version.
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
to decompile scripts, [cod-luajit-decompiler](https://github.com/Nico-Posada/cod-luajit-decompiler)
and [CoDLuaDecompiler](https://github.com/Nico-Posada/CoDLuaDecompiler) to decompile Lua files,
and [FFmpeg](https://ffmpeg.org) to decode WMA sounds.

GrayWolf is distributed under the GNU General Public License v3.0, as the projects it comes from;
see [LICENSE](LICENSE).
