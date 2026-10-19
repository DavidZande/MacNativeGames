# Parallel replay: design notes (2026-10-19)

## Why the replay matters

Busy area (David's save, ~4,900 draws, ~104 render passes and ~365 inline uniform updates per frame; 60 cap with fine
pacing, 54-56 FPS standing):

- The game's render/submit thread (guest `0x1004`) calls `nvnQueueSubmitCommands` once per frame from the frame submit at
  `main+0x87ff24`, **while holding the game's graphics mutex** (`[*GOT 0x45586f0]+0xf8`). Our synchronous replay runs
  inside that call: ~61% of the thread's time, about 11 ms per frame.
- The game's main thread (`0x1001`) takes the same mutex in its per-frame GPU wait (`main+0xa91f98`), then waits on the
  frame's fence. `TOTK_WAITSTAT=all` shows it blocked on that mutex, held by `0x1004`, once per frame for ~5 ms
  (274 ms/s), and `0x1004` waiting ~8 ms per frame for `0x1001`.
- Game code is only 11-19% of any game thread's time, and the five job workers are idle 85-93% of the time. The GPU takes
  ~12.5 ms per frame. The 3-core limit is not what holds the frame.
- `TOTK_ASYNC_REPLAY=1` releases the mutex at once, but the main thread then waits on the frame's fence instead
  (translation + GPU), with no FPS change. So the chain to shorten is **handoff of frame N -> its GPU completion**, which
  is replay latency plus the GPU tail. Only a shorter replay helps; moving it to another thread does not.

Upper bound: if the replay overlapped the GPU completely, the chain would be about the GPU's 12.5 ms, which leaves room
for 60 FPS. That is the prize; the plan below is about how much of it can be reached at what risk.

## Where the replay thread's time goes

Profile `bench/20261019-121645` (after 97c4013), samples inside `submit_exec` on the render thread, approximate:

| Part | Share | Notes |
|---|---|---|
| Walk, state machine, draw / pass logic | ~21% | `draw_inner`, `encode_args`, `mtl_exec`, `state_op` self time |
| Lookups | ~15% | pool queries, uniform snapshots / shadows, pipelines, serial table, stage signatures, texture views |
| Copies | ~6% | uniform shadows with pending updates, inline updates |
| Objective-C / Metal calls made on this thread | ~8% | argument tables, blits, dispatches, commits |
| Waiting for the encode stage (`rs_sync`) and locks | ~5% | |
| Everything else (many small functions) | rest | |

The encode stage thread (B, `nvn_render_stream.h`) already makes the render encoder's Metal calls and is ~65% busy.

## Why "one worker per render pass" is not a first step

The walk (`mtl_exec`) is one stateful machine, and render-pass boundaries are decided while walking:

- Pass boundaries move at run time: same-target continuation and merging (`g_rt_merged`, `rt_range_*`), deferred and
  hoisted clear passes (`defer_*`, `hoist_*`), frozen / duplicated passes (`g_fz`), cube-face grouping (`g_cg`), layered
  passes (`g_lay`), and store look-ahead that scans later commands (`g_exec_cur` / `g_exec_next`).
- Order-dependent data flows across passes: the inline-update log and its shadows (`g_upd`, `g_shadow`), pending
  CPU-side writes (`pend_*`), GPU-written ranges (`g_gw`, `usnap_drop`), visibility counters and post operations,
  residency, the argument-table caches and encoder state caches.
- `nvn_mtl.m` has 437 file-scope variables; `rp_begin_ex` touches ~40 globals and `encode_args` ~44.

A per-pass worker needs every one of these either made per-worker (with the state at the pass boundary reproduced) or
made safe to share, and then merged back in order. That is weeks of work, and each mechanism is a source of rendering
bugs that only show in certain scenes.

## Staged plan

Each stage is behind a flag, verified with the GPU probes and a checked busy-area run, and measured with one profiled run.

1. **Resolve-ahead helper (days).** A helper thread walks the submission ahead of the replay thread with a light,
   read-only walk (the PSO predictor `pso_predict_rest` and the uniform-snapshot helpers already work this way) and fills
   per-command results the replay thread then only reads: programs for `BIND_PROGRAM`, texture / sampler views for bound
   handles, pool lookups for buffer bindings, pipeline keys. The replay thread checks a generation stamp and falls back
   to its own lookup on any mismatch, so a wrong guess costs time, never correctness. Expected: most of the ~15% lookup
   share, i.e. ~1-1.5 ms per frame off the critical path.
2. **Move more Metal work to the encode stage (days).** Argument-table writes, blits and dispatch encoding that only need
   values the replay thread already resolved can go into the packet stream, so the replay thread stops making those calls
   (~8%) and waits less for B (~5%). B has ~35% headroom; a second B (compute / copies) is possible if it fills up.
3. **Split the submission into ordered segments (weeks).** Two-phase replay: phase 1 (serial, cheap) walks the commands
   once to fix the pass boundaries and record the state at each boundary; phase 2 resolves and encodes segments on
   several workers, each into its own MTL4 command buffer, committed in order. Needs the state inventory above turned
   into a per-worker context. Only worth starting if stages 1-2 leave the replay well above the GPU time.

Stages 1 and 2 are independent of each other and keep the single walk, so they keep today's rendering behaviour by
construction. Stage 3 is the real "parallel replay by render pass".
