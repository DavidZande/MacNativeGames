# Binding fast paths: quick implementation and test, 2026-10-19

The existing optional argument-epoch cache and precomputed texture reflection
lists were activated in a development candidate, checked against the original
paths, and measured uncapped. Neither experiment supports changing the defaults
in the tested world scene. Both remain available as opt-in experiments. The
packaged playable app was not replaced by this candidate.

For the complete performance status, packaged-app identity and next priorities,
see the [consolidated findings](practical-performance-20261019.md).

## Implementation retained

The benchmark now has `--cache-ab [both|arguments|texture]` and `--gpu-passes N`.
The first adds four same-session standing windows with off/on/on/off ordering.
The second captures per-encoder GPU timestamps after performance windows.
Actual runtime flags and ordering are recorded in the input file and summary.
See [usage and timing limitations](uncapped-benchmarks.md).

The candidate initially enabled both paths by default; those default changes
were reverted after testing. The argument cache preserves pass, arena, resource,
binding and snapshot invalidation. Texture plans cache only immutable reflection
slot lists; current handles and texture content are still resolved normally.
No fences, readbacks, uniform snapshots, draw calls or image quality were removed.

## Correctness checks

The native production argument-epoch GPU probe passed 15,554 checks. Negative
control 18 deliberately omitted binding invalidation and failed with a GPU pixel
mismatch; control 19 omitted snapshot invalidation and failed with a snapshot GPU
mismatch. The controls therefore detected the stale data they were designed to
introduce, rather than merely failing to launch.

An isolated game run enabled both paths plus reference checks through title,
load, standing and camera pan. It recorded 5,011,724 checked argument-cache hits
at the last periodic argument report and 27,733,106 checked texture-plan visits
at the last texture report, with no mismatch. Additional argument-cache reuse
was only 4,204 at that report, versus millions of existing same-shader hits;
the high total hit count should not be mistaken for new work eliminated.
Texture plans selected 49,795,400 entries out of 186,312,217 reflection entries
at the final report, without demonstrating a corresponding FPS improvement.

External evidence root: `~/Documents/TearsOfKingdom/bench/`.
Probe results are in `cache-fastpaths-probes.json`; the checked game and GPU
capture are in `20261019-142046/900p-unl`.

## Quick performance results

The two comparisons used one development executable and a copied save. Each
condition pools the actual per-frame samples from its two windows; no outliers
were removed and no averaged percentiles are used here. Both tests stayed at
the same 900p configuration with interpolation and all pacing disabled.

| Experiment | Window duration | Disabled FPS | Enabled FPS | Disabled p99 ms | Enabled p99 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Argument cache + texture plans | 10 s each | 82.86 | 81.36 | 13.721 | 16.178 |
| Texture plans only | 8 s each | 83.59 | 83.73 | 13.442 | 13.532 |

Evidence directories: `20261019-142252/900p-unl` and
`20261019-142601/900p-unl`, respectively. `cache-pooled.json` stores the combined
results; individual window samples remain in `cache-*-frames.csv`.

The combined experiment had inconsistent window FPS and worse frame-time tails.
The texture-only difference was about +0.16%, with CPU usage effectively
unchanged at 285.4% versus 285.5% across the selected coarse counter rows.
These are short exploratory windows with naturally changing weather/time and
background load, not proof that the combined implementation is inherently slower
or that the texture path produces a real gain. They are sufficient to reject
promoting either experiment on this evidence.

## GPU capture

Four diagnostic frames captured 99–121 render encoders, ten compute encoders
and four blit encoders per frame. Render work accounted for 9.47–11.30 ms in
these captures; the cost was spread across many passes. Example fragment-key
groups at 800x450 and 1600x900 averaged roughly 0.55 ms and 0.48 ms per occurrence.
These identify shader/pass candidates for a separate targeted optimization.
The profiler changes command submission and encoding, so these rankings are
not normal-path FPS measurements. The first shader key in a pass also does not
identify every draw in that pass.

All 45 source tests passed, including the native frame logger. Build and test
logs are `cache-fastpaths-build.log`, `cache-fastpaths-final-build.log` and
`cache-fastpaths-source-tests.log`. Executable hashes and launch environments
are recorded in each run's `summary.json`.
