# Practical performance findings and current status, 2026-10-19

The first uncapped comparisons identify GPU work as the main steady-state limit
in this save. They do not demonstrate a new same-quality FPS improvement from
the runtime cleanup. These are offscreen rendering measurements, not visible
presentation FPS or a guarantee for other areas of the game.

This is the consolidated record for David's M1 Pro / 16 GB, TOTK 1.0.0
workspace. It includes the later binding-cache experiments. Earlier M3/M4 and
1.4.3 records remain separate; their results are not acceptance for this build.

## Current conclusions

| Finding / change | Evidence and decision |
| --- | --- |
| Existing async replay + one-frame-late frame fence | Earlier committed work removed the roughly 29 FPS capped behavior. Retain it; later offscreen tests measure capacity beyond 60, not an additional code-induced jump from 60 to 84. |
| 900p uncapped world | About 83–84 FPS standing and 89 FPS in the 20-second scripted pan. GPU time closely tracks total frame time. |
| 720p + spatial MetalFX | 92.02 FPS standing / 99.59 FPS panning: a resolution/quality tradeoff, not a same-quality runtime improvement. |
| Allocation observer bypass | Removes 21,689 routine log writes in a comparable load, but no demonstrated steady FPS gain. Enabled in the packaged app; diagnostics remain opt-in. |
| Additional compute warm-up | 39 → 220 compute pipelines, about three extra startup seconds; hitch benefit not exercised on this route. `TOTK_WARM_COMPUTE` remains off. |
| Argument cache + texture plans | Quick pooled off/on results 82.86 → 81.36 FPS; no consistent benefit. Both remain off by default. |
| Texture plans alone | 83.59 → 83.73 FPS, CPU effectively unchanged. The roughly +0.16% difference does not establish a gain. |
| Guest event-pacing hypothesis | Only five matched waits, all at frame zero; no evidence to patch this path for this scene. |
| 100% warm-up screen during benchmarks | Stale offscreen-test overlay fixed. The window shows phases and still previews between measurement windows. |

Current measurement commands and limits: [uncapped benchmarks](uncapped-benchmarks.md).
Detailed trials: [compute warm-up](compute-pipeline-warmup.md) and
[binding fast paths / GPU capture](binding-fastpaths-experiment.md).

## Changes applied

- Successful SDK memory allocations now use the direct SDK path by default.
  `TOTK_TRACE_VAMM=1` restores the allocation observers for diagnostics. A
  comparable run previously wrote 21,689 allocation log lines; the default path
  writes none. This removes unnecessary work without changing allocation results.
- The benchmark has no FPS targets, display pacing, software ceiling or frame
  generation. It records all individual frame times and pools them for
  percentiles; slow frames are not discarded. See [methodology](uncapped-benchmarks.md).
- Offscreen tests previously left the startup overlay at 100% because no
  drawable was ever presented. The benchmark window now switches to test phases
  and still previews between timed windows. Raw target captures are retained;
  previews use the presentation crop so render-target padding is not shown.
- Opt-in guest event-wait tracing checks the Ghidra pacing hypothesis without
  bypassing any waits or changing guest instructions.

## Controlled comparisons

External evidence root:
`~/Documents/TearsOfKingdom/bench/`.
Both 20-second comparisons below used binary SHA-256
`6629dfcf9c388eef78551258dd98c41256ca095f9a932694378c26359efb9725`.
They used the same copied save, warm caches and unchanged gameplay defaults;
test-only environment overrides selected the rendering modes.

| Configuration | Standing FPS | Camera pan FPS | Standing p99 / max ms | Pan p99 / max ms |
| --- | ---: | ---: | ---: | ---: |
| 900p, allocation tracing on | 83.51 | 90.43 | 13.410 / 22.307 | 13.705 / 22.182 |
| 900p, allocation tracing off | 83.46 | 89.05 | 13.588 / 71.892 | 13.715 / 22.526 |
| 720p + spatial MetalFX, tracing off | 92.02 | 99.59 | 12.103 / 21.739 | 12.494 / 21.412 |

The tracing-on evidence is `20261019-135815/900p-unl`; tracing-off and
resolution comparisons are `20261019-140121/{900p-unl,720p-mfx-unl}`.
Standing and pan each have their own exact raw sample CSV and summary JSON.
The 900p tracing-off standing window includes a 71.9 ms hitch; it remains in
the reported results. Allocation logging removal has no demonstrated steady
FPS gain. Lower resolution with MetalFX improved measured standing throughput
10.3% and camera-pan throughput 11.8%, with a corresponding image-quality
tradeoff. This option already exists in settings; no new default is imposed.

Average GPU time was 11.574 ms standing at 900p versus 10.509 ms at 720p with
MetalFX, close to each total frame duration. Logs confirm actual render-stage
MetalFX dispatches from 1280x720 to 1920x1080; they are included in these timings.
CPU profiles were collected after the timed windows and are diagnostic only.

These 20-second windows are exploratory comparisons, not repeated balanced
acceptance runs. The camera input is delivered in timed steps, so the pan phase
includes any remaining stationary time. Weather, game time and background load
were not frozen. Results should not be extrapolated to all gameplay.

## Later cache experiments and GPU capture

The existing argument-epoch cache and immutable texture-reflection lists were
enabled in a development candidate, checked in the game, and compared in
same-session off/on/on/off standing windows. The combined trial used 10-second
windows; the texture-only trial used eight-second windows. Pooled p99 changed
from 13.721 to 16.178 ms for the combined trial, and 13.442 to 13.532 ms for
texture plans alone. Neither supports changing defaults. This does not prove
the combined path is inherently slower under all conditions.

The argument GPU probe passed 15,554 checks; deliberate binding and snapshot
invalidation errors were caught. The checked game run recorded millions of
reference comparisons without a mismatch. Additional argument reuse was small
relative to existing cache hits; raw hit counts are not savings attributable
to the new mode. All 45 source tests passed after the final rebuild.

The retained benchmark controls are `--cache-ab [both|arguments|texture]` and
`--gpu-passes N`. GPU profiling runs after speed windows, and reference checks
are disabled in A/B timing windows. Four diagnostic frames captured 99–121
render encoders, ten compute encoders and four blit encoders per frame. Render
work accounted for 9.47–11.30 ms; example full-screen shader-key groups averaged
about 0.4–0.55 ms per occurrence. Profiling changes submission/encoding behavior,
and a pass's first fragment shader does not identify all its draws, so these
are candidates for investigation rather than isolated shader execution costs.

Evidence: `20261019-142046/900p-unl` (checked run and GPU capture),
`20261019-142252/900p-unl` (combined caches), and
`20261019-142601/900p-unl` (texture only), under the evidence root above.

## Next work, ranked by current evidence

1. **Target specific GPU passes at unchanged resolution.** Use the captured
   rankings to identify the complete draw/shader workload, then remove proven
   redundant work or optimize the translation. Existing pass merging and store
   look-ahead already operate; do not count them as new optimizations.
2. **Diagnose non-GPU stalls.** The 71.9 and 85.4 ms frames had roughly 12 ms GPU
   time and no new pipeline/library compilation. Correlate guest scheduling and
   synchronization with frame IDs; allocation, upload or scheduler causes are
   not established. No stall fix has been implemented yet.
3. **Investigate finer cache invalidation and uniform copying only with new
   evidence.** The world logs show roughly 6,000 new argument blocks and 250
   renamed uniform copies / 4 MB per frame. These are counts to investigate,
   not proof that the work is redundant. The two simple cache modes were tested
   without a convincing win; uniform-copy reduction remains unimplemented.
4. **Broaden correctness coverage.** Combat, abilities, shrines, cutscenes,
   scene changes and long sessions still need coverage. Expanded compute warm-up
   needs a route that first uses one of the extra 181 pipelines. Video mapping
   batching is still open; removing allocation logs did not implement batching.

Quick screening is sufficient to reject promoting these candidates. Before
claiming a small positive gain or changing gameplay defaults, use longer,
balanced windows with the same binary, copied save, resolution and cache state,
retain all hitches, and keep diagnostics/captures outside measured windows.

## Hypotheses checked

Ghidra completed full analysis at 13:29:28 Europe/Berlin (2,337 seconds of work),
reporting 179,336 functions including externals. The saved database is
`~/Documents/TearsOfKingdom/TOTK_build/ghidra/TOTK.gpr`; its image base
is zero, so `main+` offsets are used directly without adding `0x100000`.
Read-only inspection exports and scripts are under `inspection-20261019/`
beside the database. Decompiled types and function boundaries remain analysis
results rather than recovered original source or independent correctness proof.

The Ghidra event-wait path at `main+0xa9229c` is real, but the diagnostic run
`20261019-134748/900p-60` recorded only five matching event waits, all at frame
zero, and none through roughly 7,200 later frames. That does not support
patching it to improve this scene. The saved Ghidra database has image base zero.
The heavy timeline recording in that run distorted timings and is not used
as an FPS baseline.

Expanded compute pipeline warm-up increased prepared compute pipelines from
39 to 220 but added roughly three seconds to startup without improving steady
FPS. It remains off by default; see [experiment](compute-pipeline-warmup.md).

## Validation

The native runtime builds successfully. All 45 source tests pass using the
external build environment's Python, including native raw-frame logging and
compute manifest selection. Graphics command lifetime probes and their negative
controls passed. Logs: `practical-source-tests-final.log`,
`practical-gfx-probe.json`, and `compute-dispatch-probe.log` in the evidence root.

The first complete benchmark-window integration run is `20261019-140750/900p-unl`:
it completed title, load, standing and camera pan, wrote private counter and raw
frame files, and captured the status window. Its shorter five-second phases
are integration checks rather than replacements for the performance comparison.

The packaged app from the first practical pass was rebuilt at
`~/Documents/TearsOfKingdom/Tears of the Kingdom.app` and passed strict
deep signature verification and its launcher check. Its runtime SHA-256 is
`0130bc7820571d28fb6031d58b12f21d9c74e8f4f45ab3146e73694d0489d3f5`.
The packaged-app integration run `20261019-141327/900p-unl` completed all four
phases and verified the cropped previews and phase labels in saved window
captures. Standing rendered at 84.24 FPS in that short five-second check,
including an 85.4 ms hitch. Evidence: `practical-app-build.log` and the run's
raw samples, `summary.json`, and `900p-unl-idle-window.png`. These integration
checks closed the test game normally and used only the copied test profile.

The later binding candidate did not replace that app. Its tested executable
SHA-256 was `cc080979c27760657441ec6fa7f0118c2760517cc1fc33c6477322d1de0237c0`;
both A/B runs explicitly started the experimental paths disabled. The final
development rebuild restored both off-by-default settings and has SHA-256
`9d6d109252a6b518e2dd4e5d742724421949e3f6b712b0a751701405383bb9d7`.
That final rebuild was compiled and source-tested; the game runs above used
the candidate with recorded overrides. The Git HEAD remains `5133055`; the
later runtime, benchmark, tests and documentation are local uncommitted changes,
not additional commits or pushes.
