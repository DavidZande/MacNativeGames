# Performance follow-ups from the handoff review — 2026-10-20 afternoon

Three items from `local-session-handoff.md`, in the agreed order. All source changes are uncommitted on
`mac-local-improvements`; evidence folders are under `~/Documents/TearsOfKingdom/bench/` (outside the checkout).

## 1. In-place pipeline builds at world entry (fixed)

Run `20261020-124944` had 36 render pipelines built in place on the submit thread at world entry (up to 1.08 s each,
one 8.7 s frame), although async pipeline compiling was on and the pending table was not full. Cause: the TBDR commit
20c44fd made `pso_get` bypass the async path for later draws in a pass opened without depth/stencil
(`g_enc_mode >= 2`), so those draws were never skipped, but they were built synchronously instead, queued behind the
background compiles.

Fix (`nvn_mtl.m`, `g_pso_reopen`): such a draw's omitted-depth/stencil pipeline is queued in the background, the pass is
ended and reopened with depth/stencil attached (mode 0), where the normal async path applies. `TOTK_PSO_OMIT_SYNC=1`
restores the old in-place build.

Check: `900p-unl-cold` (no warm-up, fresh binary path = cold Metal cache), bench `20261020-135401`: **0 in-place builds
of 1,782 new pipelines**; idle/pan 82.9 / 77.2 FPS (unchanged). The remaining cold-run entry frames are ~1,200 shader
libraries compiled from source on first use (cold-cache only).

## 2. Shadow/decal feedback through framebuffer fetch (new, default on)

The decal/material passes sample the albedo / packed-normal attachments they render into
(`shadow-feedback-20261019.md`). The copy correction cost +1.2 / +1.9 ms GPU. These shaders sample at the fragment's own
pixel, so on Apple GPUs the read is framebuffer fetch: no copy, no pass break.

`nvn_rt_fetch.h` (`fbf_try`) proves it per (vertex shader, fragment shader, register→attachment) set from the MSL:
every use of the register is `.sample(s, select(UV, 0, isnan(UV)))`, UV = `M.xy / M.w` of one perspective input, and the
vertex shader writes that output once as `((w + x)/2, (w − y)/2, −, w)` of its final clip position with one return.
At the draw it requires the sampled view to be the attachment itself (same Metal storage, level 0, one level, same
format/size, single sample) and a full-target viewport. The variant replaces those samples with `[[color(j)]]` inputs and
is compiled in the background; until it and its pipeline are ready the draw uses the copy path
(`g_fbf_fallback` → `draw_inner` redoes the draw with copies). `TOTK_RT_FEEDBACK_FETCH=0` turns it off.

Coverage on the roof save: 41 shader pairs proven, 11 refused correctly (e.g. `.gather` 2×2 reads, offset samples —
those keep the copies). Log lines: `[fb-fetch] … ready` / `copies kept (reason)` / periodic draw counts.

Evidence:
- Visual, roof save (2026-10-19 20:43 launch backup via `bench/roof_capture.py`): `shadow-20261020-140337-fetch`
  `view-0-6.png` matches the copy-corrected `shadow-20261019-212823-feedback` and is free of the baseline's patches
  (`shadow-20261019-204656-default`).
- Same-binary A/B, 900p-unl, 10 s phases (`20261020-140523` off → `20261020-140719` on):

  | phase | copies | framebuffer fetch |
  |---|---|---|
  | idle | 80.92 FPS, GPU 12.18 ms | **89.76 FPS, GPU 10.98 ms** |
  | pan | 70.85 FPS, GPU 13.91 ms | **82.32 FPS, GPU 11.79 ms** |
  | footprint | 5.8–6.0 GB | 4.7–4.9 GB |

Not yet done: a deterministic GPU probe for the fetch path (the copy path has `rt-feedback`), long play coverage.
Load-cover elision (`TOTK_LOAD_COVER`, `DM_NVN_TBDR`) is off by default; with it on, a fetch draw that opens a pass
would need its load kept. Merged-pass (`g_pso_lmap`) draws fall back to copies.

## 3. RomFS read cache (new, default on) — and what the entry frames really are

Trace (`TOTK_FS_TRACE=<file>`, new): the entry burst is 3,554 distinct small files in one second (median 2 KB, p90
33 KB), each read whole, by two loader threads (0x1008/0x1009), **contiguous in the image** (gap 0 sorted by offset).
The earlier per-handle readahead could never hit. Simulated fill units: 64 KiB tiles 1,065 preads (1.4× bytes),
**256 KiB 437 preads (2.4×)**, 1 MiB 222 (4.8×).

`romfs.c`: reads < 256 KiB go through 256 KiB position-keyed tiles (64 MB LRU), filled whole with one pread; a reader
moving forward (previous tile cached, read ending in the second half) queues the next two tiles for 3 background
threads. `TOTK_ROMFS_CACHE=0` off, `TOTK_ROMFS_CACHE_MB`, `TOTK_ROMFS_CACHE_CHECK=1` compares every cached read with a
direct read (passed: 5,000+ identical reads in two entries, `20261020-140947`, `20261020-141544`). The previous
uncommitted 16 MB-window experiment is saved at `TOTK_build/romfs-ra-experiment-20261020/romfs.c`.

Result (`20261020-142000`): the two burst windows cost 187 + 160 ms of read time vs 288 + 217 ms before
(`20261020-124944`), and loading-screen frames read faster (e.g. 39 MB in 58 ms → 34 MB in 22 ms). **But the worst
post-spawn frames did not change (~160–195 ms)**: they read 0 MB from disk. Frame 14559 of `20261020-141719`:
292 first-time texture uploads (102 MB, 7.9 ms CPU), submit+present 195 ms, main waiting on the graphics lock 181 ms.
That is a submit-side stall, not I/O — the next thing to diagnose (`TOTK_STALL_DUMP=60`, `TOTK_TIMELINE`), not more
read caching.
