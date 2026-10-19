# Play-time hitches: investigation and fixes, 2026-10-19 (afternoon and evening)

David's report: while playing, the game "drops from about 80 FPS to 20 or so" for short moments, and UI text
flickers. This records how the causes were found, what changed, what was measured and what is left. Machine: M1 Pro,
16 GB, macOS 27.2, TOTK 1.0.0, uncapped mode unless stated. The steady-state throughput record is
[practical-performance-20261019.md](practical-performance-20261019.md); this document is about single slow frames.

## Summary

| Cause | Evidence | Change | Result |
|---|---|---|---|
| ReadTexels / WriteTexels converted the whole mip level both ways for every call | Slow frames had 6–29 texel calls (normal frames 0.03); frames with ReadTexels were slow 19.4% of the time vs 1.0% | Rectangle-only copies (`nvn_deswizzle_rect` / `nvn_swizzle_rect` / `nvn_rect_span`) | Texel copy time ~0 ms; frames over 50 ms 28 → ~16 per minute of play |
| Write-protect fault storms on textures the game rewrites every frame | ~800–1,040 faults per frame in bursts of 0.3–1 s, aligned with the dips | Hot textures: content hash per submission instead of page faults | Storms gone from the slow frames |
| In-place pipeline builds when the pending table was full | 126 ms stall with one in-place build; "table full" in the log | Table 384 → 1024 slots; a full table skips the draw for a frame | No in-place build stalls seen since |
| Command-recording copies on the game's job threads | Largest `memmove` callers on the recording threads | Large recordings handed over without copying; bulk inlining on | Recording copy time −40–50% (in-session A/B); no visible effect alone |
| **GPU write-back on `nvnMemoryPoolInvalidateMappedRange`** | The ~110 ms stalls every 10 frames after spawning: a job thread sat in our write-back, one GPU round trip per mip level | Batched write-back (one command buffer, one wait) | Scripted spawn: frames over 50 ms 23 → 3–4, worst frame per second 113 → 38 ms |
| UI text flicker | Gone with `TOTK_FENCE_LAG=0`; Construct label corrupt in 10/40 frames with texture-only snapshots | Capture read-only glyph instance storage; late fence retained | Full label in 40/40 frames; see [UI follow-up](ui-flicker-20261019.md) |

Commits on `mac-local-improvements`: 9c16454 (first four rows and diagnostics), b453fed (write-back), plus upstream
pulls 923dafa, 9b75221, 9be6745, eaff858, c93494f (see `upstream-prs-2026-10-19.md` in the workspace).

## Method

The scripted benchmark (standing / panning in the busy area) never showed these hitches, so the first rounds were
**free-play sessions**: `bench.py --play` (new) starts the work tree's binary on screen with the app's uncapped
setup, on an isolated copy of the profile, and sends no input; David played and closed the window. Each session wrote:

- `perf.csv` (per second) and `totk.log` with the long-frame lines (`[长帧]`), which now also carry the main
  thread's waits, disk reads, hot-texture checks and page faults;
- a **per-frame timeline** with `DM_NVN_TSV=<file>`: every frame's frame time, submit (replay) time, GPU time,
  faults, re-uploads, texel calls, recording time, main-thread wait time by kind, process CPU, page faults
  (minor / disk), context switches, footprint and NVN call counts. Slow frames were compared field by field with
  normal frames; that comparison found the texel calls and later ruled out memory pressure and disk reads.

Later sessions added CPU profiles (`sample` on the game, `profile_guest.py --threads` for one guest thread) and
the Ghidra project (`TOTK_build/ghidra/TOTK.gpr`, read-only headless scripts, image base 0). The last cause was
then found **without a player**: the scripted benchmark reproduces the post-spawn stalls in its first ~10 s in the
world, so a fix can be checked in about three minutes.

## Findings in order

### 1. Fault storms and whole-level texel copies (first play log)

The 16:52 play log showed bursts of 35–90 ms frames with normal submit (~7 ms) and GPU (~11 ms) time and no
pipeline builds, each burst with ~800 write-protect faults per frame and ~18 MB of texture re-uploads. The game's
switch of its present interval 1 → 3 right after each burst is a symptom: uncapped mode ignores that interval.

The faulting pages belonged to textures the game rewrites on the CPU every frame. The writer ring (now on by
default, `DM_NVN_DIRTYREC=0` off) named the writers in the log (`[hot]`, `[缺页风暴]` lines). One of them is a
5120×5120 texture (format 0x49, 17 MB) written by `nvnTextureWriteTexels` in small tiles. In the next session the
per-frame timeline showed the slow frames carrying 6–29 texel calls: `mtl_write_texels` / `mtl_read_texels`
deswizzled and reswizzled the **whole level** for every small tile, unprotected all 17 MB and re-uploaded it.

### 2. Pipeline stalls and recording copies

A 126 ms frame was a single pipeline built in place on the replay thread because the pending table (384 slots)
was full. In the spawn-window CPU profile, the two largest `memmove` callers on the job threads were our
`EndRecording` copy and the record-by-record inlining of called lists. Both were made cheaper; an in-session A/B
(flags switched every 15 s through the input file) measured recording copy time 1.5–1.7 → 0.8–1.2 ms per frame and
no change in hitches, so these were not the cause of the remaining stalls.

### 3. The ~110 ms stalls after spawning

What remained: frames of 106–113 ms exactly every 10 frames for ~15 s after spawning (and after some later events),
the main thread waiting on one address. The same length at 80 and at 30 FPS (David tried the app's 30 FPS setting:
"30 → 9 FPS") ruled out game workload, which would vary.

- Ghidra: the main thread waits in the game's job scheduler (`FUN_00813358`, `WaitLightEvent` at `main+0x8136c8`),
  called by the frame-update stage (`FUN_007a7654` → `FUN_007a806c`, "wait for the job group while helping").
- `TOTK_WAITLOG=1` (new): no guest wait ran into a timeout; the main thread and job thread 0x1037 were both woken
  after ~106.8 ms by job thread **0x1038**.
- Timeline with per-thread CPU (`tcpu`, new): during a 180.9 ms wait, 0x1038 blocked on nothing in the guest and
  used **3.9 ms** of CPU. So it was blocked in host code.
- `TOTK_STALL_DUMP=60` (new) sampled the stacks of threads not in a guest wait while the main thread waited: every
  in-world stall had a job thread in `waitUntilCompleted` inside `mtl_pool_invalidate`, called from game
  `main+0xa90a74 ← 0xa8fb90 ← 0xa8f9c4`.
- The `[invalidate]` timing line (new) split one invalidate (~33 ms): drain ~4.5 ms, GPU flush ~17 ms,
  **9 write-back blits 11.3 ms** — one commit and wait per mip level, ~1.25 ms each, almost all of it start delay.
  The game does this about three times in a row in a job the main thread waits for.

Fix: all levels / layers / textures of one invalidate are copied in one command buffer with one wait, from a batch
on the caller's stack (several job threads invalidate at once); the invalidate waits for the fence of the newest
rendering into those textures (it is the newest submission here, so the flush is still ~17 ms).

Scripted spawn, first 10 s in the world (same build, `TOTK_WB_BATCH` on / off):

| | Frames ≥ 50 ms | Worst frame per second |
|---|---|---|
| Before | 23 (113 ms in 6 of 10 seconds) | up to 113 ms, FPS 51–53 in those seconds |
| After (two runs) | 4 and 3, all at world entry | ≤ 38 ms, FPS 79–92 |

Screenshot luminance of the standing frame across all comparable runs: 187.7–190.3 (one run 176.0 with a passing
cloud; its repeat 188.8), so the batched write-back does not change the read-back result visibly.

## Flicker

The UI text flicker predates these changes (David confirmed) and disappears with `TOTK_FENCE_LAG=0`: with the frame
fence answered one frame late, the game rewrites CPU textures (UI text) while the previous frame's replay still has
to read them. Turning the late fence off costs ~20 FPS in busy areas. A write barrier (a game write to a protected
texture page waits for the queued replays) reduced the flicker little, waited ~6 ms per frame and caused a 2.5 s
freeze once; it is kept as an experiment, **off** (`TOTK_WRITE_BARRIER=1`).

Follow-up: [glyph instance storage capture](ui-flicker-20261019.md) preserves the per-character position / glyph data
of each queued submission. The initial broad texture snapshot experiment did not fix the Construct label and was
removed. The storage correction keeps the late-fence performance mode enabled without using the write barrier.

## Switches

| Switch | Default | Effect |
|---|---|---|
| `TOTK_STORAGE_SNAPSHOT` | on | Capture small read-only storage inputs, including per-glyph instance records |
| `TOTK_TEXELS_RECT` | on | ReadTexels / WriteTexels on CPU-pool textures copy only the rectangle |
| `TOTK_PKG_HOT` | on | Textures re-uploaded 3 frames running are checked by content, not page faults |
| `TOTK_PSO_FULL_SYNC` | off | `=1`: a full pending table builds in place again |
| `TOTK_PSO_PREDICT_SCAN` / `_KEYS` / `_US` | 8192 / 32 / 2000 | Prediction scan budgets (upstream c5b3ae7 port) |
| `TOTK_REC_TAKE`, `TOTK_INLINE_BULK` | on | Recording hand-over without copying; bulk inlining |
| `TOTK_WB_BATCH` | on | Batched GPU write-back on invalidate |
| `TOTK_WRITE_BARRIER` | off | Experimental flicker barrier |
| `DM_NVN_DIRTYREC` | on | Writer ring for the `[hot]` / fault-storm logs |

Diagnostics (all opt-in): `DM_NVN_TSV=<file>` (per-frame timeline), `TOTK_WAITLOG=1`, `TOTK_STALL_DUMP=<ms>`,
`TOTK_TIMELINE=<file>:<first frame>:<frames>` (now with per-thread CPU, event `tcpu`), `TOTK_WAITSTAT=…`.
Tools: `bench.py --play` (accepts `--env` pacing switches such as `TOTK_FPS60=0 DM_PRESENT_HZ=60`),
`profile_guest.py --threads 0x1001,0x1037`.

## Tests

`make probe-texels-rect` (28,407 random checks of the rectangle copies against the whole-level copies, with negative
controls), `make probe-dirty-race` (17 checks, barrier hooks included), `scripts/verify_gfx.py` (inline-bulk probe
22 checks with negative 16; prediction negatives 24, 26, 27), `make probe-thread`, `make test-opus-codec`.

## What is left

1. **UI flicker follow-up.** Glyph storage snapshots fix the reproduced Construct label; see [verification and remaining play coverage](ui-flicker-20261019.md).
2. **The remaining ~22 ms per invalidate**: ~17 ms waiting for the frame the game just rendered and ~4 ms drain,
   about three times in a row after spawning. Frames now stay under ~40 ms there; a deeper change would need the
   readback to be answered from data the GPU produced earlier.
3. **World-entry frames** (130–260 ms, first frames after the loading screen) and occasional first-use pipeline
   builds.
4. **Game job time.** About two-thirds of job-thread time is the game's own code; our recording layer is ~13%
   and its global copy lock ~3% (spawn-window profile, before the copy changes).
5. `TOTK_FREE_CORES=1` stops at the first submit ("command handle … is not a live recording"): command recording
   relies on the three-core arbitration. Not usable as is.

Raw evidence: `bench/` in the workspace (play sessions `20261019-17*` / `18*`, scripted runs `20261019-19*`,
per-frame timelines `play-20261019-frames-*.tsv` / `spawn-frames-*.tsv`, thread timelines `spawn-timeline-*.tsv`,
CPU samples `*/sample-spawn.txt`).


## 2026-10-19 late continuation: experimental producer readback & targeted upload wait

See [the current shadow/readback handoff](shadow-feedback-20261019.md) for implementation, GPU probe evidence and walking captures.
- Invalidate hitch cause resolved: previously, `mtl_pool_invalidate` waited for all pending GPU upload command buffers (`g_up_pending`), meaning a readback was delayed 20–30 ms by unrelated background texture uploads queued behind in-flight frames. Implemented targeted dependency tracking `g_up_inflight`: invalidations now only wait if the texture being read back is actually a pending upload destination.
- All 10 GPU probes pass (readback prefetch passes 71,006 checks, including 2-texture/shared-page regression). All 45 Python unit tests and all native probes pass.
- In the latest walking benchmark (`bench/shadow-20261019-221609-walk`), 92 readback prefetch snapshots were consumed, and in-world invalidation stalls > 20 ms were eliminated: walk-1 p99 19.37 ms (max 25.50 ms, 1 frame >= 25 ms, 0 >= 33 ms); walk-2 p99 17.64 ms (max 24.78 ms, 0 frames >= 25 ms, 0 >= 33 ms).
- Installed app remains unchanged while the candidate undergoes final user review.
