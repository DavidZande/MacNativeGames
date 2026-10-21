# World-entry I/O stall and session findings — 2026-10-20

Companion to [play-hitches-20261019.md](https://github.com/DavidZande/MacNativeGames/blob/notes-2026-10-21/docs/play-hitches-20261019.md) (fixed hitches).
Everything here was measured on the M1 Pro bench
(900p-unl, `TOTK_build/totk` build of branch `mac-local-improvements`); bench IDs are relative to
`~/Documents/TearsOfKingdom/bench/`.

## 1. The world-entry 100–260 ms frames are disk I/O, not GPU or game logic

**Evidence chain (who) — run `20261020-123505` (TSV + timeline + LOCKSTAT + WAITLOG in one):**

- The stall frames repeat at `TL_XRUN` timeline frames ~15723/15727/15729/15731/15732/15737, which are
  the *same* frame numbers as the TSV `f15731=210 ms` row (the two counters match; see §4).
- In the 210 ms frame (`f15731`): guest main `0x1001` used **3.4 ms CPU** and guest-blocked only 2.3 ms;
  *all* guest threads together used **15.6 ms**; the frame had **+570 voluntary context switches** and
  +163 MB footprint growth. Something was waiting, not computing.
- The highest CPU consumers at entry are the usual recorder/worker threads `0x1037`/`0x1038`.
- Preview screenshots (`[长帧]` log) at the equivalent frames of neighboring runs show `读盘 N MB … ms`
  (disk-read counters in the frame) climbing exactly where the stall lands.

**The mechanism — `[totk-fs] read window` lines (`TOTK_FS_TIMING=1`), run `20261020-124944`:**

```
t=9270.617 calls=3880 bytes=139448417 wall_ms=278.160 prepare_ms=13.681
t=9271.617 calls=1228 bytes=107824596 wall_ms=196.927
```
- The game streams freshly-seen RomFS archives in **~40 KB reads** (176 MB / 4,261 calls ⇒ 41 KB average).
- With an `io_ms` column added (`rd_io_ns`): **wall_ms == io_ms exactly** — the entire wall is pread
  syscall time. No lock, no copy, no `dm_guest_write_prepare` (prepare is logged separately, ~13 ms).
- Per-call cost is ~80–390 µs — this is **per-call tail latency of cold reads against the 17 GB
  `romfs.bin`**, not SSD bandwidth (3 GB/s would be 60 µs per 40 KB, and warm re-reads are ~0.03 ms —
  an external python probe of the same file confirmed cached preads are instant).
- The game thread issuing these reads blocks in our `runtime_romfs_read` → the frame chain up the game's
  loader stalls; main eventually waits ~200 ms inside `nvnWindowAcquireTexture` (`acq_in→acq_out` 202 ms
  in the timeline) because the submit/present pipeline is starved behind the loader.

**Why "texture uploads" was the wrong lead:** the Metal upload side is fine — `@upload_ms` (+GPU copy)
during the burst is 5–18 ms for ~100+ MB. The cost is getting the bytes **from disk into guest RAM**
before any NVN call happens.

### Things that were tried and do not work

| Attempt | Result |
|---|---|
| `fcntl(F_RDADVISE)` ahead windows (build tree) | **APFS ignores it.** Wall unchanged (268 ms window with a 128 MB advs window). Dead end on macOS 27. |
| Background prewarm threads preading big ranges into the page cache (first `ra_thread` version) | No reduction: 261→288 ms. The read pattern is not "one handle marching forward" — see next table. |
| Mmap path | Irrelevant for the bench: the bench does **not** set `TOTK_ROMFS_MMAP`, so reads go through `src_pread`. The mmap copy path (`memcpy(image_map+…)`) would have the same per-page fault cost for cold regions. |

**Read-pattern tally** (last run of the day, `ra(fires,skips) / hits / fwd / back` MB per window):

```
t=11419.859 calls=3700 bytes=184403343 wall_ms=309.258 ra(fires,skips)=0/0 hits=0mb fwd=0mb back=175mb
```
`fwd=0 / back=175 MB` reads were tagged `back` when a handle's absolute read position was *behind* the
armed trigger. Meaning: **the entry burst is not a clean sequential march per handle** — handles are
re-opened, interleaved and re-read, so a per-handle trigger + round-robin slot assignment misses
essentially every read. Any fix must key the cache by *position*, not by handle-triggers.

### Where the code stands (uncommitted, `TOTK_build/totk/runtime/romfs.c` only)

An experimental **userspace streaming cache** is in the tree, incomplete and not yet effective:

- 8 global streams × 16 MB windows (`TOTK_ROMFS_RA`, 0=off) around `image_fd`, filled forward by
  detached workers that pread 4× window-size plans in big chunks; `ra_try_stream` serves small reads
  fully from a published window; counters `ra_fires/ra_skips/ra_hit_bytes/ra_fwd_bytes/ra_back_bytes`
  print inside the `TOTK_FS_TIMING` window line (currently the shipped format is
  `ra(fires,skips)=…/… hits=…mb fwd=…mb back=…mb`).
- It compiles and is bench-neutral when misses dominate, but the discriminator above shows it is not
  getting hits. **Next move for an inheritor:** key windows by lookup across all streams on *every*
  read (hit test is cheap), refill the window the read is actually in (i.e. re-center instead of
  branch-forward), consider a small LRU keyed by `(image offset >> 24)` 16 MB tiles instead of
  forward-plans. Also verify against the split-image phone layout (`nchunks>0`, `src_pread` already
  routes chunk offsets, but `readahead_fire` early-outs on `nchunks` today).
- Bench evidence: `20261020-124944` (open+trigger fires, no wins; also the one 8.7 s pipeline-storm
  run, §2), `20261020-125545`/later runs with the tally columns.

## 2. One-off 8.7 s stall: pipelines built in place holding the graphics lock (run `124944`)

```
[totk-gfx] [长帧] 帧 15585（83.65 s）整帧 8691.1 ms … 新建管线 19 条 … 8029.8 ms；
主线程等 锁 8120.5 ms … 系统缺页读盘 173
```
19 world-entry pipelines built **in place** (pending table full — the pre-existing "build-in-place on
full table" failure mode, previously 126 ms-class) during the I/O burst; each pipeline compile paid
cold reads too (缺页读盘 counters non-zero). It appeared exactly once across ~30 runs; the
pending-table widening from 384→1024 slots (commit 9c16454) evidently has a real-world worst case
left. Treat as a separate item if it recurs: consider (a) the pending-table admission policy at world
load, or (b) making first-use pipeline builds read their shader blobs through the streaming cache once
§1 works.

## 3. Guest mutex hot-spot: measured, bounded-spin tried, no win (committed)

- Lock `0x7033e52660` is the game's **recorder/worker job-queue mutex**, contended by guest main
  `0x1001` and recording job threads `0x1037`/`0x1038` (prio 44). Cost: ~5–7 ms/s of main's blocking
  at idle; peaks to ~124 ms/s only in `sample`-perturbed or load-phase runs
  (`bench/20261019-233251`, `20261020-115815`). Guest `lr` at the SVC is the common SDK lock helper
  (`sdk+0xdffd08`); no game symbols nearby.
- `TOTK_LOCK_DUMP=<addr,…>` (committed, `runtime/threads.c`) one-shot dumps 192 bytes around each
  address at the first main lock-wait report — attributed the lock to the job-queue cluster.
- `TOTK_LOCK_SPIN=<ns>` bounded spin before `ArbitrateLock` queueing (committed `191c2c9`, strtoul
  NULL-guard fix `12268a6`) — **no measurable gain** (idle 82.9 vs 82.8 FPS, pan 77.3 vs 76.9), so it
  stays default-off. The block→wake→core-handoff round trip it saves was not on the critical path. Do
  not re-derive without a new hypothesis.
- **Caution for anyone touching svc handlers:** in `svc_thread` (runtime/threads.c) `G` is **not held
  on entry** — the first spin patch unlocked `G` it did not hold and wedged the boot (freeze,
  `bench/20261020-120312`). Also `getenv`+`strtoul` needs a NULL guard (`bench/20261020-121206`,
  SIGSEGV in libsystem at the first contended ArbitrateLock).

## 4. Instrumentation decoder cheat-sheet (for future sessions)

**`TOTK_TIMELINE=<file>:<first frame>:<frames>`** — header `# t_ns kind tid frame a b`, tabular:
```
sub_in sub_out rep_begin rep_end commit gpu wait_in wait_out acq_in acq_out
pres_in pres_out finish_in finish_out fence rs_join gblock tcpu xrun
```
- **Timeline frame counter == TSV frame number** (`pres_in` gap == TSV `@frame_ms` of that frame).
  An earlier session mis-suspected a counter mismatch; the real mistake was a too-narrow window
  (world entry happens at ~frame 15500–15750 in these runs depending on boot variance).
- `gblock` (kind 17, emitted by runtime/threads.c `tl_gblock`): `a` = blocked-wall ns,
  `b` = `wait_kind | (thread << 8) | (waker << 32)`. Wait kinds: **1** = ArbitrateLock lock-wait,
  **2** = svc 0x34 WaitForAddress, **3** = condition variable (0x1C-family), **4** = WaitSynchronization
  sync-wait, **5** = SleepThread timed sleep (benign), 6 = core re-take (only when ≥ 20 ms, logged
  separately). Beware: the whole wait *including* core re-acquisition is one span.
- `tcpu` (kind 18): `a` = cumulative CPU ns (diff between consecutive samples per `b&0xffffffff`
  thread handle), `b` = `handle | wait_kind<<32 | current_core+1 <<40`. Sampled once per frame for
  every live thread — sum of deltas ≈ per-frame CPU of the game threads.
- `xrun` (kind 19, ≥1 ms translation-thread jobs): `t` = start, `a` = `wall_ns | queue_depth<<56`,
  `b` = `job_kind | cpu_ns<<8` (kind 1=submit, 2=present, 3=texrel).
- Time base: `t_ns` is CLOCK_UPTIME_RAW-since-boot; TSV `@t_s` is process-relative. Pair by frame
  number, never join by clock (same trap as the previous handoff note).

**`DM_NVN_TSV` rows** that matter for load/entry work: `@frame_ms`, `@upload_ms`/`@upload_gpu_ms`
(the Metal side of first-use uploads), `@mprotect_*`, `@footprint_mb`, `@reuploads`, `@pso_new`,
`@lib_new`. **Pitfall:** `@call_ms` is an **absolute timestamp** (ms since process start), not a
duration — diffing it across frames yields `frame_ms`, not a stall number.

**`TOTK_FS_TIMING=1`** — `[totk-fs] read window t=<uptime s> calls=N bytes=B wall_ms=… prepare_ms=…
io_ms=… ra(…)`. `wall` excludes `dm_guest_write_prepare` (that's `prepare_ms`). `io_ms` = time inside
the pread loop (0 for mmap-path copies). Open logging: `TOTK_FS_LOG=1` prints every RomFS open with
the time since start. The image default location (this machine):
`TOTK_build/totk/extracted/v0/romfs/romfs.bin` (16 GiB, sha in `build/romfs_config.h`), index
`runtime-index.bin`, verified via cached `RomfsProof` (mtime/size gated — no full re-hash per boot).

**General:**
- Native `sample` perturbs runs (up to 2x on p99); attribution only, never perf numbers.
- The bench harness itself is now shot-safe (`shot_staged`, commit `2555197`) — the historic
  "530 ms class-A stalls" were the *bench's own* in-present screenshots; verified not the game.
- Cached pread sanity probe (useful discriminator for any I/O theory):
  `python3 -c "import os,time; ..."` two sequential preads of the same 40 KB range from romfs.bin —
  second read should be ≥100x faster than the first.

## 5. Current bench reference points (today's quiet-machine numbers)

| phase | render_fps | p50 | p99 | max |
|---|---|---|---|---|
| title | ~108 | 9.2 | 10.7 | 11.2–14 |
| load | ~350 | 1.87 | 13.7 | 209 (entry burst) |
| idle | ~82 | 12.0 | 13.7 | 22–48 |
| pan | ~77 | 12.9 | 15.2 | 23–26 |

The load-phase `max` is dominated by the entry burst; idle/pan are the steady-state the branch has
held all day. `20261020-120803` (with TOTK_LOCK_SPIN) and `20261020-121415` (strtoul fix) are the
clean "everything else normal" references.
