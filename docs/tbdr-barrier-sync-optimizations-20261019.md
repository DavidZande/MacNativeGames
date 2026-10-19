# TBDR Load/Store Elision, Barrier Narrowing, and Sync Poll Optimizations (2026-10-19)

## Overview

Building on the previous cache fast-path and compute warm-up work, this session implemented four targeted GPU and CPU/runtime improvements for *The Legend of Zelda: Tears of the Kingdom* (v1.0.0 on Apple Silicon, M1 Pro 16 GB RAM):

1. **Render Pass Coalescing & Tile Load/Store Elision** in `games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m`
2. **Command Buffer Segmentation & Vertex-Hazard-Aware Barrier Narrowing** in `games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m`
3. **Shader Reflection Output-Mask / Channel-Write-Mask Intersection** in `games/totk/third_party/d2r_gfx/gfx/nvn/nvn_refl.m` and `games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m`
4. **Zero-Timeout Sync Poll Fast-Path & Slow-Frame Phase Attribution** in `games/totk/third_party/d2r_gfx/gfx/nvn/hle_nvn.c` and `games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m`
5. **Unlimited FPS by Default** in `games/totk/app/launcher.m` and `games/totk/third_party/d2r_gfx/gfx/nvn/hle_nvn.c`

---

## Detailed Changes

### 1. Render Pass Coalescing & Tile Load/Store Elision (`nvn_mtl.m`)
Previously, `DM_NVN_TBDR` was globally disabled on TOTK because two sub-features (`defer_try` pass deferral across passes and `fuse_try` tile-read pass merging) did not track storage-buffer (`NVN_RB_SBUFP` / `NVN_RB_SBUFU`) writes or 2D-array layer slices (`R.lay`). That single master switch also turned off safe TBDR load/store optimizations, causing 78 out of 111 render passes per frame to perform full `MTLLoadActionLoad` + `MTLStoreActionStore` (~755 MB/frame of tile-to-DRAM traffic).

We decoupled the safe TBDR sub-features from `defer_try`/`fuse_try` via granular switches (all enabled by default unless `TOTK_TBDR_OPT=0`):
- **`TOTK_STORE_OPT` (`totk_store_opt_on`)**: Enables deferred `MTLStoreActionUnknown` -> `MTLStoreActionDontCare` in `sd_finish` for non-layered passes (`!g_lay.active`) via:
  - **Unwritten-attachment store elision (`udc_dead`)**: Attachments loaded with `MTLLoadActionLoad` that were never written by any draw or clear in the pass are ended with `MTLStoreActionDontCare`.
  - **Intra-submission overwrite lookahead (`sdx_decide`)**: Attachments whose next touch within the same `nvnQueueSubmitCommands` submission is a full-target, un-scissored `OP_CLEAR_COLOR` / `OP_CLEAR_DS` (before any `SDX_READ`, `SDX_NEED`, or `SDX_ATT`) are ended with `MTLStoreActionDontCare`.
  - Fixed `sdx_walk` and `mtl_sub_index` to:
    - Use `prog_get_cached` so building the submission index never blocks on uncompiled programs.
    - Initialize `g_pp_rt` from `R.has_rt` at the start of a submission when the first pass inherits render targets without an explicit `OP_SET_RT`.
    - Track `OP_SCISSOR` (`sdx_scissor_full`) and open passes (`g_sdx_rt_open`) so scissored clears or in-pass clears are treated conservatively as `SDX_ATT` rather than full-overwrite `SDX_CLEAR`.
    - Only skip `e->rtseq < P` for `SDX_CLEAR` events in `sdx_decide` so subsequent draws into a persistent render target are always recognized as `SDX_ATT`.
- **`TOTK_LOAD_COVER` (`totk_load_cover_on`)**: Enables `fz_load_covers` in `rp_begin_ex` for non-layered passes so attachments provably covered by an opaque fullscreen first draw use `MTLLoadActionDontCare` instead of `MTLLoadActionLoad`.
- **`TOTK_DS_OMIT` (`totk_ds_omit_on`)**: Enables `pass_ds_need()` in `draw_inner` to omit unused depth and/or stencil planes (`mode = 2, 3, 4`). Added `ds_mode_pso_ready(NvProg *pr, int mode)` to check whether the omitted-DS `MTLRenderPipelineState` is already cached in `g_pso`; if not, it queues background compilation via `pso_request_bg` and falls back to `mode = 0` for the current frame, achieving **41 omitted DS passes/frame with 0 skipped draws and 0 render-thread stalls**. Also updated `pso_get` so `g_enc_mode >= 2` passes do not skip secondary draws when a pass stays in `mode = 2, 3, 4`.
- **`TOTK_HOIST` (`totk_hoist_on`)**: Enables `hoist_try()` in `draw_inner` to hoist intervening pure-clear passes before the current render pass so `SetRenderTargets(A) -> Draw -> SetRenderTargets(B) -> Clear -> SetRenderTargets(A) -> Draw` merges the two `A` passes into one continuous Metal render pass.

### 2. Command Buffer Segmentation & Barrier Narrowing (`nvn_mtl.m`)
- **Vertex-Hazard-Aware Barrier Narrowing (`TOTK_BARRIER_NARROW=1` default)**:
  - Previously, every `NVN_BARRIER_INVALIDATE_TEXTURE` / `INVALIDATE_SHADER` barrier after a render pass emitted a `MTLStagesFragment -> (MTLStagesVertex | MTLStagesFragment)` barrier, forcing Apple GPU tile-based vertex tiling of pass $N+1$ to wait for fragment completion of pass $N$.
  - Extended `sdx_walk` to record the earliest attachment write (`g_bar_vs_first_write`) and latest vertex-stage texture/image read (`g_bar_vs_last_read`), while conservatively falling back if any fragment shader writes to an image or storage buffer (`NVN_RB_IMG`, `NVN_RB_SBUFP`, `NVN_RB_SBUFU` with `rw == 1`).
  - `bar_narrow_active(uint32_t seq)` narrows consumer stages to `MTLStagesFragment` whenever `seq < g_bar_vs_first_write || seq > g_bar_vs_last_read`, allowing vertex tiling to overlap with prior fragment execution.
- **Command Buffer Chunking (`chunk_cfg`)**:
  - Increased default `TOTK_CHUNK_FIRST` from `60` to `120` draws and `TOTK_CHUNK_DRAWS` from `75` to `200` draws, cutting extra `MTL4CommandBuffer` commits per frame in half (`13.5 -> 6.6` per frame) while preserving early first-chunk kickoff.

### 3. Shader Reflection Output Mask & Channel Write Mask Intersection (`nvn_refl.m`, `nvn_mtl.m`)
- **Root Cause Fixed in `nvn_refl.m`**:
  - `nvn_refl_from_json` previously populated `o->color_mask` only from `j[@"raster"][@"colors"]` (used by the older D2R offline pipeline). In TOTK's `totk_xlate` runtime translator, `j[@"raster"]` is `{}` and fragment color outputs are recorded in `j[@"deps"][@"fout"]` (verified 100% match against `[[color(N)]]` across all 4,923 fragment shaders in the cache).
  - Updated `nvn_refl_from_json` to populate `o->color_mask` from `j[@"deps"][@"fout"]`, and updated `refl_mark_stores` in `nvn_mtl.m` to also scan `[[color(N)]]`, `discard_fragment()`, and `[[sample_mask]]` directly from the MSL source.
- **Exact Write Detection in `udc_note_draw` (`nvn_mtl.m`)**:
  - Changed color write detection from `||` to `&&` so color attachment `i` is marked written only when **both** `(S.chmask.mask >> (4 * i)) & 0xf` is non-zero **and** `(shader_color_mask >> i) & 1` is non-zero.
  - Changed depth write detection to `(S.ds.dt && S.ds.dw) || fs_writes_depth` so passes with `depthWriteEnabled = 1` but `depthTestEnabled = 0` (and no `[[depth(any)]]` fragment output) do not falsely mark depth as written.

### 4. Zero-Timeout Sync Poll Fast-Path & Slow-Frame Attribution (`hle_nvn.c`, `nvn_mtl.m`)
- **Zero-Timeout Poll Fast-Path**:
  - `nvnSyncWait` polls `fence_wait(..., 0)` / `mtl_wait(..., 0)` with `timeout = 0` twice per frame fence wait (once for `TOTK_FENCE_LAG` and once to check `ALREADY_SIGNALED`).
  - Previously, when `timeout_ns == 0` and the fence was not yet reached, both `fence_wait` and `mtl_wait` called `clock_gettime(CLOCK_REALTIME, &dl)` and entered `pthread_cond_timedwait` (`__psynch_cvwait` XNU kernel trap) with an already-expired deadline.
  - Added immediate `!timeout_ns` fast paths in both `fence_wait` (`hle_nvn.c`) and `mtl_wait` (`nvn_mtl.m`), eliminating these kernel syscalls and mutex/condvar round-trips on non-blocking polls.
- **Slow-Frame Phase Attribution (`[长帧]`)**:
  - Extended `lf_rec` and the `[长帧]` log in `nvnQueuePresentTexture` (`hle_nvn.c`) to break down `提交+出图` and `上传贴图` into `等栅栏` (fence sync wait), `取图` (window acquire wait), `采UBO` (UBO snapshot + deferred handle resolution), `缺页` (write-watch page faults), and `护页` (`mprotect` duration).

### 5. Unlimited FPS by Default (`app/launcher.m`, `hle_nvn.c`)
- Updated `uncapped_interval_on()` in `hle_nvn.c` to default to `1` (unless `TOTK_UNCAPPED_INTERVAL=0`), and updated `present_cfg` to allow `TOTK_UNCAPPED` on 120 Hz ProMotion displays (`g_hz <= UNCAPPED_HZ`).
- Updated `app/launcher.m` to explicitly export `TOTK_UNCAPPED=1` and `TOTK_UNCAPPED_INTERVAL=1` when `fpsCap` is `unlimited` (the default).

---

## Visually Verified Benchmark Results (`900p-unl`, M1 Pro 16 GB)

Benchmark command:
```bash
python3.14 scripts/bench.py --configs 900p-unl --seconds 15 --env DM_NVN_TSV=/tmp/totk_nvn.tsv --env TOTK_SYNC_LOG=1
```

### Frame Rate & Frame-Time Distribution

| Phase | Metric | Baseline (`20261019-142601`) | New (`20261019-154846`) | Delta |
| :--- | :--- | ---: | ---: | :--- |
| **Title** | **Render FPS** | 85.94 FPS | **93.42 FPS** | **+8.7% (+7.48 FPS)** |
| | **GPU Time (avg)** | 11.462 ms | **10.521 ms** | **-8.2% (-0.94 ms)** |
| | **Frame p50 / p95 / p99** | 11.56 / 12.65 / 13.49 ms | **10.55 / 12.38 / 13.87 ms** | **-8.7% p50** |
| **World — Idle** | **Render FPS** | 76.19 FPS | **83.00 FPS** | **+8.9% (+6.81 FPS)** |
| | **GPU Time (avg)** | 13.065 ms | **11.760 ms** | **-10.0% (-1.31 ms)** |
| | **Frame p50 / p95 / p99** | 12.69 / 15.86 / 18.89 ms | **12.00 / 13.30 / 14.29 ms** | **-5.5% / -16.1% / -24.3%** |
| | **Max Frame Time** | 74.513 ms | **22.895 ms** | **-69.3% (-51.62 ms)** |
| | **Hitches > 50 ms (`long50`)** | 3 | **0** | **Eliminated** |
| **World — Camera Pan** | **Render FPS** | 76.64 FPS | **85.51 FPS** | **+11.6% (+8.87 FPS)** |
| | **GPU Time (avg)** | 13.073 ms | **11.475 ms** | **-12.2% (-1.60 ms)** |
| | **Frame p50 / p95 / p99** | 12.70 / 15.53 / 17.57 ms | **11.46 / 13.87 / 16.41 ms** | **-9.8% / -10.7% / -6.6%** |
| | **Max Frame Time** | 83.105 ms | **38.492 ms** | **-53.7% (-44.61 ms)** |
| | **Hitches > 50 ms (`long50`)** | 1 | **0** | **Eliminated** |
| | **Skipped Async Draws** | 0 | **0** | **0 visual drops** |

### Per-Frame NVN/Metal Telemetry (`DM_NVN_TSV`, 4,332 World Frames)

| Telemetry Metric | Baseline (p50) | New (p50) | Improvement |
| :--- | ---: | ---: | :--- |
| **Tile Store Bandwidth (`@rt_store_mb`)** | 391.42 MB/frame | **287.03 MB/frame** | **-26.7% (-104.4 MB/frame)** |
| **Tile Load Bandwidth (`@rt_load_mb`)** | 363.26 MB/frame | **264.77 MB/frame** | **-27.1% (-98.5 MB/frame)** |
| **Total Tile Load + Store Traffic** | 754.68 MB/frame | **551.80 MB/frame** | **-26.9% (-202.9 MB/frame, ~17 GB/s saved)** |
| **Unwritten Store Elision (`@unwritten_dc_mb`)** | 0.00 MB/frame | **43.26 MB/frame** | **+43.3 MB/frame elided** |
| **Unused Depth/Stencil Omitted (`@ds_omit`)** | 0 passes/frame | **41 passes/frame** | **41 passes/frame stripped of dead DS** |
| **Metal Render Passes (`@passes`)** | 111 passes/frame | **101 passes/frame** | **-10 passes/frame hoisted/merged** |
| **Full Load+Store Passes (`@pass_full_rw`)** | 78 passes/frame | **55 passes/frame** | **-29.5%** |
| **Command Buffer Chunks (`分段提交`)** | 13.5 extra/frame | **6.6 extra/frame** | **-51.1% fewer CB splits** |
