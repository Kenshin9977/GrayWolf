# Vanguard (`Games/S4`)

Status, 2026-11-20: **images, sounds, models with their materials, animations and the level export
from disk.** The Steam copy (appid 1985820, buildid 25235735) is read offline: its zones decompress
(Wolff), are walked by the `s4` zonekit tables (`wolff/profiles/s4`, built from Cordycep 2.9.4.0's
dump of `Vanguard.exe`, PE of 2026-08-19), relocated, and served to the readers here. The working
state is `C:\tmp\vanguard\STATUS.txt`.

## What reads today

| Piece | Where | State |
|---|---|---|
| `.ff` (IWffa100 v11, 0x98-byte header) + `.fp` (IWffd100 v6, VCDIFF) | Wolff `Iwff.cpp` (second v11 layout) | every zone tried (877) decodes; byte-identical to a Python decoder |
| Detection | Wolff `Games.cpp` (S4 row, header claim), `GameInstalls.cpp` (Steam app 1985820, `Vanguard.exe`) | `list-installs`, `FindGameModuleForFile` |
| Zone header | Wolff `formats/IW8.cpp` (profile `zone_format: iw8`) | the same 0x20-byte XAssetList as Modern Warfare 2019 |
| Zone walk | `wolff/profiles/s4` (1 344 generated tables, 5 by hand) | the 55 sample zones walked to their end in C++ |
| Strings | the profile's `strings.cipher.txt` (git-ignored, made by `graywolf-tools/scripts/s4_string_cipher.py` from DecryptString 0x34658a0) | script strings (bone names, notetracks) and localize texts: 26 504 of 26 504 sample texts as the game's function gives them |
| Images | `DiskGame.cpp` | GfxImage 0x50, its mips' `.xsub` keys, the game's DXGI table 0x7812a10 |
| Sounds | `DiskGame.cpp`, `LiveGame.cpp` | SAB v17 banks behind the soundbank's streamkey; streamed entries as `.xsub` objects; Opus |
| Models | `LiveGame.cpp`, `S4Assets.h`; facts counted by `CheckModels` (`model_check_*`) | XModel 0x188, LODs, surfaces 0xd0, meshes in the zone or `.xsub` |
| Materials | `LiveGame.cpp` (`S4Readers::ReadXMaterial`), `MaterialAdapter.cpp`, books (`IWMaterialBooks`) | a transient zone's `",mo/mtl_..."` headers from `global_stream_mp`'s book |
| Animations | `LiveGame.cpp` (`ReadXAnim`) | XAnimParts 0xa8, 4-byte bone names, 64-bit deltas, 16-byte notetracks |
| Level | `DiskGame.cpp` (`ReadMap`) | world surfaces from the transient zone's vertex bytes, static models, map_ents |
| Install index | `DefinesAt` (images too), `IndexKey` `_images1` | 5 797 zones, about 18 min to build once (resumable) |

## Facts (each read off the tables or checked on the sample's instances)

- **XModel** (0x188, the tables' 0x6c7c7f0): surface count u16 8 (the 8-byte material handles at
  0xc0), LOD count u8 0xa (the 0x40-byte LOD records behind the pointer 0xc8), bones u8 0x18, root
  bones u8 0x19, unnamed bones u16 0x1a; bone names (u32 script strings) 0x80, parents (u16,
  relative) 0x88, rotations (4 x i16) 0x90, translations (3 x f32) 0x98, base matrices (0x20) 0xa8.
  The handles run LOD after LOD (their count is the sum of the LODs' surface counts).
- **LOD** (0x40): its xmodelsurfs 0, the surfaces the game copies at load time 8 (empty in the
  zone: the reader takes the xmodelsurfs' 8), distance f32 0x10, surface count u16 0x18 (= the
  xmodelsurfs' u16 0x28).
- **XModelSurfs** (0x50): surfaces 8 (0xd0 each), the mesh's `.xsub` key 0x10, its buffer record 0x20
  ({bytes 0, u32 size 8, u8 0xc & 1 streamed}; every surface's 0x58 is the same record).
- **Surface** (0xd0): vertices u16 2, triangles u16 4, rigid runs u8 8 (8 bytes each at 0x68: bone,
  vertex count), skinned counts by influence u16 0x1c..0x2b (their column-by-column weights at 0x70,
  u32 0x2c bytes), a scale of its own f32 0x30 (-1: the box at 0xa0..0xb7, as MW 2019), the streams'
  offsets in the mesh buffer u32 0x34 + 4 k: faces 3 (u16 x 3), then positions 0 (21-bit, 8 bytes),
  a 4-byte stream 5, UVs 1 (half floats), normals 2 (MW 2019's packed quaternion), [7], [8], colours 6
  (-1: none), back to back. Checked on every surface of the zones exported (the mag zone: 60 of 60;
  polina's body: 59 of 59, 44 skinned with the columns' size).
- **Material** (0x80): image count u8 0x1c, techset 0x30, slots {u32 semantic, image* 8} at 0x38.
  Semantics, eight layers per map: 0-7 colour (`_c&_s`), 8-15 nog (`_n&_u&_g`), 16-23 emissive, 24-31
  opacity (`loot_opacity_0_a`), 32-39 reveal; 0x9f a `_cm` map. The `techsets_*` zones define no
  material (their books are empty); a transient zone's materials are headers its global zone
  (`global_stream_mp`) defines, and its images too (found by the install index).
- **XAnimParts** (0xa8): S4Assets.h; frame rate f32 0x6c, its frequency (rate / frames) f32 0x70,
  frames u16 0x7e (indices two bytes from 256), bone counts by class u8 0x81..0x8a, notetracks u8
  0x8b, type u8 0x8c (6 additive), delta parts 0x50.
- **Level**: GfxWorld 0x7a80; GfxWorldDraw inline at 0x88 (surfaces 32 bytes at 0xe0, materials
  0x178, surface data 128 bytes at 0x188); a surface's u32 0 is its first position's byte offset in
  its data's stream (the data's own in all 4 762 surfaces of mp_shipment_s4), its indices count from
  there; the data's positions are 21-bit integers x scale (1/16) + its corner (not a model's box);
  streams at 0x24 + 4 k after the vertex count u32 0x20. Static models: GfxWorldDpvsStatic inline at
  0x340 (collections of placements, MW 2019's packed placements). The brush model list holds one
  model whose record is not MW 2019's: every surface is the world.
- **Blend shapes**: the model's at 0x110 (the tables' 0x6c7d550: name count u16 2, names u32 at 8,
  map count u16 4, the 8-byte map {u16 name, u16 shape, f32} at 0x10), a surface's at 0xc0
  (0x6c728f0: records and deltas at 0, u32 0x10 bytes; extended and plain vertex counts u16 0x18,
  0x1a; 8-byte records {u16 vertex, u16 shapes, u32 offset}, deltas {u16 shape, 3 halves} at offset
  x 16 or x 8). Checked on head_mp_f_l_bailey_13 (27 shapes): jawOpen moves 2 552 vertices down
  (mean z -0.79, the teeth -2.1), lookUp's lids up (z +0.14 to +0.27), lookDn's and blinkSL's down.
- **Lights** (com_map 0x6c1f020): count u32 0x30, 0xa0-byte records at 0x38, the definition's name
  at 0x98 (the tables), the rest MW 2019's record; mp_shipment_s4's 42: types 1 to 3, unit
  directions, origins in the yard (-1 133, 7 238, -396), radii 200 to 400, cos half fov 0.09 to 0.71,
  definitions light_point_linear. The record's other fields read off the values only.
- **Volumes**: GfxImage's depth u16 0x24 (the image description 0x42f31d5 copies width, height,
  depth and levels from 0x20, 0x22, 0x24, 0x26): `global`'s two grading CLUTs are 32 x 32 x 32 RGBA16
  (DXGI 11, 262 144 bytes in the zone), written as DDS volumes, red, green and blue rising along x, y
  and z.

## Layout of the zones no recording covers

The big zones (`common_mp`, `common_base_mp`, the levels) are not recorded: their back references
land where the s4 layout (`blocks.txt`) places the reads, which the recorded zones alone left wrong
for actions they never ran. The layout now takes every loader's alignment from the code
(`graywolf-tools/scripts/s4_layout_proven.py`, `blocks.proven.txt`, `empties.proven.txt`): the 42
recorded zones stay exact and the game's loaders emulated over the whole of `common_mp` (869 423
reads) and `mp_shipment_s4` (48 148) place every read where the walker does. Before it, 10
viewmodel animations of `common_mp` / `common_base_mp` read another animation's data (6 crashed the
translator, 4 had non-unit rotations) and 2 449 material slots of `mp_shipment_s4` pointed at no
image; now none.

## Not yet

- The light record's fields past its type and definition proven in the code (read off the values).

## The old reader, kept unbuilt (`Legacy/`)

The Battle.net build's memory reader (2021-2024), restored as `.in` files outside the build glob:
the starting point of the readers above. Its structs are still in `DBGameAssets.h` (`VGX*`); the
ones the readers use are `S4Assets.h`'s, each offset checked against the tables.

Other sources: C2M's `Vanguard.cs` (maps; its world position decode and surface data layout agree
with the zone), Cordycep's `CoDVGHandler.toml`.
