# The "runtime-built" shaders are AGL .sharcb shaders (TOTK 1.0.0)

Findings from 2026-10-18, for the thread that owns the ahead-of-time pass (`prepare_shader_seed.py`, `nvn_mtl.m`,
`totk_shader_rt.m`, the build). No owned file was edited, nothing was rebuilt, and the game was not launched.

## Short answer

The game builds no shaders at runtime. Almost all of the shaders that looked runtime-built come from **AGL shader
archives (`.sharcb`, magic `BAHS`, version 9)**. They hold complete, precompiled GLSLC output (a 0x880-byte control
section plus NVN code) for every compiled variation, in AGL's own container format rather than BNSH. The seed pass only
looks for `BNSH` inside `FSHA`/`BNSH` data, so it misses all of them.

- The user's translation cache has 9,777 shaders. 417 of them are not in the 9,360 seed.
- 207 of those 417 are in Effect/UI BNSH files, which the `--extra` pass already covers (192 Effect, 15 UI).
- 3 have the same code as a file shader but a different control section (2 from `material.bfsha` ps, 1 from the UI
  `Common` `__ArchiveShader.bnsh` vs). That suggests the game edits control bytes after loading. This was inferred, not traced.
- **193 of the remaining 207 are in `.sharcb` archives**, matched byte for byte, with keys recomputed exactly.
- **14 are not stored in any searchable game file** (see "Unplaced" below).

## Where the archives are (all in RomFS, nested inside zstd SARCs)

| SARC | .sharcb members | stage binaries |
|---|---|---:|
| `Lib/agl/agl_resource.Nin_NX_NVN.release.sarc.zs` | agl_technique_pfx, _shdw, _lght, _lght_lpp, _proc, agl_technique, agl_common | 3,474 |
| `Shader/ApplicationPackage.Nin_NX_NVN.release.sarc.zs` | AglShader, AglLightShader, GrassShader | 420 |
| `Lib/Terrain/tera_resource.Nin_NX_NVN.release.sarc.zs` | tera_common | 86 |
| `Lib/gsys/gsys_resource.Nin_NX_NVN.release.sarc.zs` | common | 76 |

In total there are **3,996 unique shaders** (3,392 ps, 545 vs, 59 cs). None of them is in the 9,360 seed or in any of
the 24,341 BNSH programs across the whole RomFS, so they are purely additive. A scan of every decompressed RomFS byte for
`BAHS\x09\0\0\0` found exactly these 12 archives. `gsys_resource` also carries two Wii U `Gfx2` files
(`Compressor.gsh`, `FFLShader.gsh`); they are irrelevant here.

## Format (reverse-engineered, enough to enumerate exactly)

- header u32 at `0x1c` is the code region base.
- Stage record (4-byte aligned): `u32 code_off` (from the code base), `u32 0`, `u32 control_size = 0x880`, and
  `s32 control_rel` (relative to the record's own offset).
- The control is a GLSLC control section with magic `0x98761234`. The code length is at `+0x704`, `c1` is at `+0x700`,
  and the stage is at `+0x714` (0 vs, 1 ps, 5 cs). Identical controls are shared between records, so do not pair by order.
- The code is at base + code_off, with NVN header magic `0x12345678` for graphics or `0x12345679` for compute.
- Key = `sha256(control[0:0x880] + code[0:length])`, the same as `prog_get` in `nvn_mtl.m`. This was verified for all 193.

## How to add them to the ahead-of-time pass

`games/totk/scripts/enumerate_agl_sharcb.py` (new, standalone, read-only on RomFS) implements the above.
`stages(data)` yields `(key, stage, control, code, info)`, already in the shape `bnsh_shaders()` yields. A minimal
integration in `prepare_shader_seed.inventory()`:

1. Add the three `Lib/...release.sarc.zs` archives to the startup names. `Shader/ApplicationPackage...` is already
   visited, but its `BAHS` members are skipped.
2. In `visit()`, add `elif data[:4] == b'BAHS':` and put each `stages(data)` result into `shaders` under its key.

Running it against the verified 1.0.0 RomFS gives 3,996 unique shaders and covers all 193:

```sh
python3.14 games/totk/scripts/enumerate_agl_sharcb.py <work>/totk/extracted/v0/romfs --json <out>.json
```

A key list is saved outside the repo at `TOTK_build/totk/analysis/agl-sharcb-shaders-v1.0.0.json`. It contains hashes
and offsets only. 1.1.2 and 1.4.3 were not checked. The layout probably holds, but the counts will differ.

Variants work as before. These are binding-independent sources, so the seed's default-answer translation applies. Draws
with different texture, attribute or output types still create new `.variants`, the same as BNSH shaders.

## Which game code uses them

AGL (`agl::` from Nintendo's sead/agl engine layer) loads the archives through `agl::detail::ShaderHolder`. These are
string references in `main.elf` (1.0.0, module-relative VMA; the function starts are estimated from prologues):

- `0xec3590`: uses `"AglShader"` and `"%s.sharcb"` (it loads the ApplicationPackage archives).
- `0xea8a28`: uses `"agl::detail::ShaderHolder"` and `"sharcb"`.
- `0x110e8b8`: builds the `Lib/agl/agl_resource.` and `Lib/gsys/gsys_resource.` paths.
- `0x129b310`: uses `"ApplicationPackage"`.
- `0x2434270`: uses `"Lib/Terrain/tera_resource.Nin_NX_NVN.release.sarc"`.

`main.elf` also has weak imports for the GLSLC online compiler (`glslcCompile`, `glslcCompileSpecialized`, and others).
No shipped module exports them, and the only user, at `0x31aa8`, just fills a table of the eight pointers and reports
whether all are non-null. Its caller at `0x23fc798` then reads the GLSLC version bounds from the device (NVN device info
46 to 49). So no runtime compilation path is reachable. The code-level evidence is from `llvm-objdump`, because REA had
no decompiler configured (Ghidra, Hopper, IDA and Kuna were unavailable).

## Unplaced (14 shaders, 11 ps and 3 vs)

Their exact code bytes appear nowhere in the decompressed RomFS (nested SARC and zstd included) or in
`main`/`sdk`/`subsdk0`. Most are small, and their nearest matches are UI `__ArchiveShader.bnsh` programs. For example,
`2117f58b…` matches 249 of 346 instruction words of a `Common` UI vertex shader of the same size, so these look like
other compiled variations of UI or effect shaders. Two places were not searched: the 12,392 `Model/*.bfres.mc` files
(MCPK compression, no decoder available) and anything the game generates in memory. The list is at
`TOTK_build/totk/analysis/unplaced-runtime-shaders-v1.0.0.txt`. To settle these, log the guest return address and the
source buffer of the code pointer in `nvnProgramSetShaders` when a key is not in the seed. That hook lives in
`nvn_mtl.m`, which the owning thread controls.

## Method (reproducible)

1. Compare the cache `.variants` names with the bundled `shader-seed`, which leaves 417 runtime-only keys. Each one has
   a `.code` dump (graphics: `code[0x30:len]`, compute: `code[0:len]`).
2. Walk all 304,207 RomFS files (zstd with the ZsDic dictionaries, nested SARC), parse every embedded BNSH at any
   control size, and match by key and by code hash.
3. Search the leftovers byte for byte, which found the `.sharcb` archives. Then decode the record layout and confirm that
   the recomputed keys match exactly.
