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
|---|---|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
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
| Black Ops Cold War | `coldwar` | ✓ | ✓ | ✓ | ~ | ~ | ~ | ~ | ✓ | ✓ | ~ | ~ |
| Modern Warfare III (2023) | `mwiii`, `mwiii_sp` | ~ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ | ~ | ✓ | – | – |
| Black Ops 6 | `bo6`, `bo6sp` | ✓ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ | ✓ | ✓ | – | – |
| Black Ops 7 | `bo7` | ✓ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ | ✓ | ✓ | – | – |

`_sp` / `sp` ids are the campaigns. Notes:
- **Models**: Call of Duty and Call of Duty 2 give LOD 0 only; Infinite Warfare and MWIII miss some
  models whose parts live in other zones (`_tr`); WWII misses some composite models.
- **Sounds**: Black Ops keeps a few ADPCM/WMA sounds unread; Black Ops 4 and Cold War sound names
  are partly still hashes (`xsound_<hash>`), and Cold War leaves out its few `rev/` vehicle engine
  models (not audio). In FLAC or Opus, a sound no decoder here reads (World at War's xWMA) is kept as it
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

CoD and CoD 2: `weapons` is the game's own weapon files (`weapons/mp/<name>`, `weapons/sp/<name>` from
the archives), `weaponstats` read from their named fields, display names from the `.str` files.
CoD4, World at War, MW2, Black Ops, MW3 and Black Ops II (their own readers and the `-oat` ones): `weapons` is
the weapon files as OpenAssetTools writes them (the mod tools' `weapons/<name>`, attachments and camos beside
them), `weaponstats` read from their named fields (the game's weapon field table), the README beside the table.
Their display names are in another zone exported into the same `--out`: World at War `localized_common_mp`
(multiplayer) or the language folder's `common` (campaign, zombies); Black Ops and Black Ops II
`<lang>_code_post_gfx_mp`, for zombies Black Ops II's `<lang>_code_post_gfx_zm`, Black Ops' `<lang>_code_post_gfx`;
CoD4, MW2 and MW3 `localized_code_post_gfx_mp` (multiplayer) or the language folder's `common` (campaign).
Black Ops II's attachments are also merged: `weapon_attachments.csv`, each weapon with each attachment its file
lists, the attachment's scales applied (clip sizes match the published ones: MP7 54, PDW-57 67 with Extended Mags).
MW3's too, by the rules read in its exe (an ammunition, ADS or damage block replacing the weapon's value, then the
attachment's scales; the weapon's animation overrides for fire and reload times): Extended Mags M4A1 45, MP7 60;
the Suppressor's 0.75 shortens the ranges; the grenade launcher and shotgun belong to the alternate mode.
Black Ops III: `weaponstats` from each weapon's GDT entry as the `gdt` kind reads it (keys named as the older weapon
files' fields), equal to `--type gdt` on every compared value; names from `<lang>_core_patch`, `<lang>_zm_patch`.
Its `weapons` are the `gdt` kind's entries (`--type gdt`).
Ghosts, Advanced Warfare, MW Remastered and WWII: `weapons` is the weapon and attachment assets as struct dumps,
`weaponstats` the older games' weapon file columns, read from each game's WeaponDef (x64-zt's layouts for the
first three, their damage checked against the code; WWII's read off its exe, fewer columns: its damage stops
named by the exe's own GetWeaponDamage, start and max ammo, the linear fall-off flags), the README beside
the table. Infinite Warfare likewise (x64-zt's layout, the damage stops read off the exe); its reload, raise,
drop and sprint-out times come from the weapon's animation package at the offsets the exe's time getter is
called with, joined by the package's name when another zone holds it (`global_core_mp`'s guns,
`common_core_mp`'s packages: export both into the same `--out`).
Call of Duty Online: `weapons` is the weapon (one 0x101c WeaponDef) and weaponattach assets as struct dumps,
`weaponstats` read from that WeaponDef at the offsets its decrypted exe reads (MW2's field names; six damage
stops; no start or max ammo, which the game mode's table sets), the README beside the table. Its guns are in
`common_mp`, names in `localized_code_post_gfx_mp`, and `weapon_attachments.csv` needs `code_post_gfx_mp`
(`mp/coWeaponAttachmentTable.csv`, each weapon's attachments), the overrides applied by the exe's rules
(Extended Mags AK-47 45, Fast Mags its reload x 0.7): export the three into the same `--out`.
~: multiplayer and zombies only, not the campaign. MW2019's install is multiplayer only, and its
`weaponstats` leaves out what its exe does not prove yet (ADS, raise and sprint times); its hit multipliers
are read since the zone layout of `global_stream_mp` matches the game's (checked against its loaders,
emulated). MWIII's damage table and hit multipliers are read too. Black Ops 7: a package time of 0xffff (none)
is an empty cell, not 65535. Zombies (for Rifling's scaling): Black Ops 6's `t10_zm_<gun>` and `_pack`
attachments keep the Pack-a-Punch tiers behind 0x8d8 (f32 each) and the rarity multipliers behind 0x8e0 (6 f32), read
by the exe's object fields the game's scripts use (Black Ops 7: 0xa00, 0xa08); MWIII has no such fields.
Black Ops 4 and Cold War: `mapinfo` is their map tables and `aitypes` their AI types as struct dumps (no
settings bundle in either); `weaponstats` comes from the weapon layouts the `gdt` kind ports, their
names matched against Black Ops III's (its units), not read in the game's code (the README beside the table).
Black Ops 4's guns are in `core_ui` (names in `<lang>_core_ui`); Cold War's in `core_common` (its zombies' and
multiplayer variants in `zm_common`, `mp_common`; names in `<lang>_core_common`).

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
to decompile scripts.

GrayWolf is distributed under the GNU General Public License v3.0, as the projects it comes from;
see [LICENSE](LICENSE).
