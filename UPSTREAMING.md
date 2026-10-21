# Taking changes from this fork upstream

A map for the maintainers of [ly05010419/MacNativeGames](https://github.com/ly05010419/MacNativeGames): which commits
make up each improvement, what each one depends on, how it was measured, and how hard it is to bring over. Commit
hashes are this repository's `performance` branch.

**Please read first**

- **Tested on 1.0.0 only**, MacBook Pro M1 Pro (16 GB), macOS 27.2, 900p. 1.1.2 / 1.2.1 / 1.4.3, iOS and Breath of the
  Wild share most of this code but were not run here.
- **Nothing applies to upstream `main` as is.** Checked against `1cbab36` (2026-10-20): of 50 commits, only 7 apply
  cleanly on their own (docs and small fixes). The fork branched at `41e1e0c` (2026-10-18) and most features touch
  `nvn_mtl.m` / `hle_nvn.c`, which both sides changed. Each feature below is a port, not a cherry-pick.
- Every feature has an environment switch, so it can land default-off and be enabled after testing.
- Happy to prepare a branch per feature, rebased on current `main` and re-measured. Open an issue here and say which.

Paths below: `nvn/` = `games/totk/third_party/d2r_gfx/gfx/nvn/`, others are under `games/totk/`.

## Suggested order

| # | Feature | Gain | Depends on | Size |
|---|---|---|---|---|
| 1 | Late frame fence + glyph storage snapshot | busy area, 60 cap: 54 → 60 FPS | **both together** | small + medium |
| 2 | Play-time hitch fixes | > 50 ms frames 28 → ~16/min; spawn stalls 23 → 3–4 | none | medium |
| 3 | Decal feedback (copies) + framebuffer fetch | fixes ground/stone patches; fetch +11–16 % FPS | fetch needs the copy path as fallback | medium |
| 4 | TBDR pass optimizations + omit-pass pipeline fix | +9–12 % FPS | the pipeline fix only matters with the depth/stencil omission | large |
| 5 | RomFS read cache | world-entry read time −30–35 % | none | small, self-contained |
| 6 | Precompiled runtime shaders (`.metallib`) | after a cache clear: startup wait 74 → 17 s, no compiling during play | none (uses the iOS loading path) | small |
| 7 | Shader seeding additions | fewer first-use compiles | overlaps upstream's own background compile — compare first | medium |

## 1. One-frame-late frame fence + glyph storage snapshot

**What.** The game keeps one frame in flight: its main thread waits on the frame fence (`main+0xa91f98`) while the
render thread holds its graphics mutex across `nvnQueueSubmitCommands` (`main+0x87ff24`), which runs the whole Metal
replay. With the translation thread (`TOTK_ASYNC_REPLAY`, already upstream) the replay can overlap the next game frame
only if the fence wait returns early: `TOTK_FENCE_LAG` answers frame N's fence when frame N−1's is reached.

**Why the second part is required.** With the late fence, the CPU writes the next frame's data while the GPU still
replays the previous one. HUD glyph records (17 × 64 bytes in a read-only storage buffer) were overwritten before the
GPU read them: corrupted label letters in 10 of 40 frames. Submit-time snapshots of small read-only storage bindings
(≤ 64 KiB, `TOTK_STORAGE_SNAPSHOT`) fix it: 40 of 40 clean.

| Commits | Files | Switches |
|---|---|---|
| `a92fa74` experiment, `198669e` default on | `nvn/hle_nvn.c` | `TOTK_FENCE_LAG=0`, `TOTK_ASYNC_REPLAY=0` |
| `41de26e` (glyph snapshot part; also contains #3's copy path) | `nvn/nvn_mtl.m`, `nvn/hle_nvn.c`, `runtime/gpu_path_probe.m` | `TOTK_STORAGE_SNAPSHOT=0` |

**Evidence.** Busy area with the 60 FPS cap: 54 → 60.1 standing, 60.2 panning, 0 anomalies in ~7,700 frames. Records:
[`docs/practical-performance-20261019.md`](https://github.com/DavidZande/MacNativeGames/blob/notes-2026-10-21/docs/practical-performance-20261019.md), [`docs/ui-flicker-20261019.md`](https://github.com/DavidZande/MacNativeGames/blob/notes-2026-10-21/docs/ui-flicker-20261019.md) (8,712 GPU storage probe checks, negative
control fails as expected).

## 2. Play-time hitch fixes

| Fix | Commit | Switch | Result |
|---|---|---|---|
| ReadTexels / WriteTexels copy only the changed rectangle, not the whole mip level (a 5120×5120 texture was converted 6–29× a frame while moving) | `5352ab7` | `TOTK_TEXELS_RECT=0` | texel copy time → ~0 |
| "Hot" textures the CPU rewrites every frame skip write-protect fault tracking | `5352ab7` | `TOTK_PKG_HOT=0` | fault storms gone from slow frames |
| Pending pipeline table 384 → 1024; a full table skips the draw instead of building in place | `5352ab7`, `4b82486` (recycle) | `TOTK_PSO_FULL_SYNC=1`, `TOTK_PSO_RECYCLE=0` | no more 126 ms in-place builds |
| Large recordings handed over without copying; bulk inlining | `5352ab7` | `TOTK_REC_TAKE=0`, `TOTK_INLINE_BULK=0` | recording copies −40–50 % |
| GPU write-back of a pool invalidate batched into one command buffer (was one commit + wait per mip level) | `8c46687` | `TOTK_WB_BATCH=0` | ~110 ms post-spawn stalls 23 → 3–4 |
| Read-backs wait only for the uploads they depend on | `41de26e` | `TOTK_WB_UPWAIT=all` | in-world invalidations > 20 ms gone |
| Write-back runs inside an autorelease pool (same leak as upstream `635fb31`) | `4b18416` | — | no growth |

Combined: frames > 50 ms 28 → ~16 per minute of play. Probes: `tests/texels_rect`, `tests/dirty_race`. Record:
[`docs/play-hitches-20261019.md`](https://github.com/DavidZande/MacNativeGames/blob/notes-2026-10-21/docs/play-hitches-20261019.md).

## 3. Decal feedback: copies, then framebuffer fetch

**Bug.** Ground and stone show moving black / yellow patches. The decal / material pass (e.g. fragment shader
`e06b8a49…`) samples the albedo and packed-normal attachments it is rendering into. On a TBDR GPU the sample returns
memory, not the pass's tile contents. Same root cause as open PR #67 (ghosting while turning).

**Fix, step 1 (`41de26e`, `TOTK_RT_FEEDBACK_SNAPSHOT`).** Sampled attachments are copied before the draw, once per
interval between pass boundaries. Correct, but +1.2 ms GPU standing / +1.9 ms panning. GPU probe `rt-feedback`
(3,085 checks; disabling the fix fails the pixel control).

**Fix, step 2 (`4b18416`, `nvn/nvn_rt_fetch.h`, `TOTK_RT_FEEDBACK_FETCH`).** Where the MSL proves each sample reads the
fragment's own pixel, a variant reads `[[color(j)]]` (framebuffer fetch) instead: no copy, no pass break. The proof is
textual and conservative (sample at `M.xy / M.w` of one perspective input; the vertex shader writes that output once as
`((w + x)/2, (w − y)/2, −, w)` of its final clip position; sampled view = the attachment itself, full-target
viewport). Variants compile in the background; until ready, the copy path runs. On a roof test save 41 shader pairs
are proven, 11 refused (e.g. `.gather` reads) and keep copies.

| | copies | framebuffer fetch |
|---|---|---|
| standing | 80.9 FPS, GPU 12.18 ms | 89.8 FPS, GPU 10.98 ms |
| panning | 70.9 FPS, GPU 13.91 ms | 82.3 FPS, GPU 11.79 ms |
| footprint | 5.8–6.0 GB | 4.7–4.9 GB |

**Note for PR #91** (unwritten vertex outputs start at (0, 0, 0, 1)): it adds an initial vector store to every
written output; `fbf_vs_screen` in `nvn_rt_fetch.h` currently refuses vector writes, so it would need to accept that
leading initialisation, or every variant falls back to copies. Records: `docs/shadow-feedback-20261019.md`,
[`docs/perf-followups-20261020.md`](https://github.com/DavidZande/MacNativeGames/blob/notes-2026-10-21/docs/perf-followups-20261020.md).

## 4. TBDR pass optimizations

`907138d` (large: 25 files): tile load/store elision, unused depth/stencil omitted (`TOTK_DS_OMIT`), shader output masks
from reflection, fewer command-buffer splits and barriers, a zero-timeout sync fast path; uncapped FPS default.
`14142f8` fixes a regression it introduced (clouds losing their depth test across `OP_UPDATE_UB`) and a separate
shared-storage texture aliasing flicker. `4b18416` fixes a third: later draws in a depth-omitted pass built pipelines in place (up
to 1.08 s each; `TOTK_PSO_OMIT_SYNC=1` old behaviour).

Result: +8.7–11.6 % FPS, tile traffic 755 → 552 MB/frame, passes 111 → 101/frame. `DM_NVN_TBDR=0` turns the pass
rewriting off. Record: [`docs/tbdr-barrier-sync-optimizations-20261019.md`](https://github.com/DavidZande/MacNativeGames/blob/notes-2026-10-21/docs/tbdr-barrier-sync-optimizations-20261019.md). This is the hardest to port and the one
most worth re-validating on other versions.

## 5. RomFS read cache

`e6e9e6d`, only `runtime/romfs.c`. The world-entry burst is ~3,500 small files (median 2 KB) read whole by two loader
threads, contiguous in the image; every read was a cold `pread` of 80–390 µs. Reads < 256 KiB go through 256 KiB
position-keyed tiles (64 MB LRU) with read-ahead for a forward-moving reader. Burst read time 288 + 217 ms →
187 + 160 ms. `TOTK_ROMFS_CACHE=0` off, `TOTK_ROMFS_CACHE_CHECK=1` verifies every cached read against a direct read,
`TOTK_FS_TRACE=<file>` logs reads. The worst post-spawn frames are a submit-side stall, not disk reads; this does not
fix them. Record: `docs/world-entry-io-20261020.md`.

## 6. Precompiled runtime shaders

`2cb11a7`, `nvn/nvn_rt_metallib.h` (new) + hooks in `nvn/nvn_mtl.m` (`shader_load`, `warm_prime`, `warm_prime_only`),
`scripts/bake_metallibs.py`. On macOS every translated shader was compiled from MSL at each launch and relied on the
system's shared Metal cache, which gets cleared or rotated (one per user, ~1 GB, shared by all apps); the launch after
that compiled ~25,000 libraries again. The game now loads `<key>.metallib` next to `<key>.metal`, as the iOS port
already does, compiled by Xcode's `metal` with the runtime's options (default language version, safe math). The
output carries the source's modification time and is used only while it matches, so a retranslated source falls back
to source compiling and is baked again. After the warm-up the game bakes new sources on 2 background-QoS threads;
without the Metal Toolchain nothing changes. Measured at 1080p: the essential-shader wait went from 74 s to 17 s in
the app (0.3–1.7 s in the benchmark), the first picture from 113 s to 25 s; per-frame GPU time is identical on and
off. `TOTK_RT_METALLIB=0` off. Record: [`docs/next-performance-ideas-20261020.md`](https://github.com/DavidZande/MacNativeGames/blob/notes-2026-10-21/docs/next-performance-ideas-20261020.md) (last section).

## 7. Shader seeding (compare with upstream first)

Upstream has since built its own background compile, parallel warm-up and shared shader list, so these overlap:
`20dd61c` / `352fa67` / `689f5da` (background translation, `TOTK_ASYNC_XLATE`), `5fb8417` (`TOTK_ASYNC_SHLOAD`),
`116bc95` (14,950 effect / UI shaders pre-translated and compiled in the background), `2e45732`, `d14e57a`
(3,996 AGL `.sharcb` shaders added to the startup seed; `docs/runtime-shaders-agl-sharcb.md`).

## Smaller items

- `175f820` / `65e9cd0` / `bcca286`: the 60 FPS cap presents every tick and uses fine pacing on 120 Hz screens.
- `25e8dc3`: futex-style guest waits (FPS-neutral).
- `b484570`: cheaper per-draw lookups on the main thread.
- `4619e69` / `c4479ba` / `de225fd`: flight recorder (`TOTK_FLIGHT`): last ~20 s of frame timings written on a > 50 ms
  frame or F9, with a screenshot and per-pass GPU timings. `51ce4ab`: `TOTK_SUBMIT_SANITIZER` (debug) and `TOTK_VRR`
  (experimental; `c25dc76` turned it off again: it can lock the game at 40 FPS after a dip).
- `4e4d754`: Mac app quality of life (settings window, save backups, full screen, audio smoothing, macOS 27
  address-space fallback, RomFS kept outside the signed app).

Already upstream, no action needed: Opus slot reuse (#18 / #40), present texture residency (#30), compile-options
race (`35606fe`), predictor cached lookups (#28 / `c5b3ae7`), serial-history bitmaps (#34), cloth/hair clock
(`290ec9f`), stale optional target (`e8b5259`), present thread's CPU time across period changes (`e0216b2`, from
nxnative `04ea8f8`).

## Measuring

`scripts/bench.py` runs uncapped, offscreen phases (title, load, standing, panning) on an isolated copy of the
player's profile: `python3 scripts/bench.py --configs 900p-unl --seconds 10 [--env NAME=VALUE]`. Same-binary A/B runs
with a feature's switch on and off are the comparisons quoted above. Method: `docs/uncapped-benchmarks.md`.
