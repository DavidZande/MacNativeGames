# Remaining text flicker: glyph instance storage, 2026-10-19

The Construct name label was still losing individual letters after the initial UI texture snapshot experiment.
The copied save starts directly in front of the same Steward Construct, making the exact report reproducible.
The earlier cloud-depth and shared-storage alias fixes in `61555e0` remain intact.

## Cause and correction

The font draw uses vertex shader `35cbc686…` and fragment shader `8a20741c…`, a static 112-byte quad, six indices,
and 17 glyph instances. Its read-only vertex storage binding contains **17 × 64 = 1,088 bytes** of per-character
position, colour and glyph coordinates. Uniform bytes were captured at submit, but this storage binding still
pointed at live guest memory. The late frame fence let the game overwrite that data before the previous frame used it.

Small storage bindings (up to 64 KiB) now join the existing submit-time buffer capture. The argument builder uses
those saved bytes for read-only storage access. Writable bindings stay on the live GPU timeline. A GPU write in the
current or a prior submission invalidates / bypasses the corresponding saved version, as for uniforms.

Distinct storage ranges are kept separate from neighbours when coalescing snapshots: invalidating a writable range
must not discard an unrelated uniform or another glyph buffer next to it. Empty captures are also released correctly.

`TOTK_STORAGE_SNAPSHOT=0` restores the previous storage-binding path diagnostically. The default retains
`TOTK_ASYNC_REPLAY` and `TOTK_FENCE_LAG`. No new GPU wait or write barrier is added.

The earlier broad UI texture snapshot experiment was removed. It did not fix this label, and the storage correction
passes with it disabled. The final source captures glyph instance data instead of copying about 74 MiB of textures
per world submission. The old `TOTK_UI_SNAPSHOT` option is no longer present.

## Direct reproduction

External evidence root: `~/Documents/TearsOfKingdom/bench/`.

- `20261019-201950/900p-unl`: previous texture-only build. **10 of 40 consecutive label frames** have missing letters.
  `label-contact.png` shows the failure, matching David's screenshot.
- `20261019-202928/900p-unl`: storage correction, broad texture capture explicitly disabled. The full name is intact
  in **all 40 consecutive frames**. The pass dump records the actual 1,088-byte binding using captured GPU storage
  with `live_equal=0`: the guest bytes had already changed by encoding time. Sixteen inventory frames also retain
  their text. The final source removes the disabled texture path; its glyph-buffer implementation is unchanged.
- Short uncapped 900p run: **94.08 FPS standing / 88.79 panning**, p99 **12.66 / 14.51 ms**, maximum **21.39 / 26.04 ms**,
  with no frames ≥50 ms in either timed phase. Screenshot and pass-dump work is between the timed windows.

These are offscreen throughput measurements on a copied save. Cloud state and sample durations differ from earlier
runs, so the figures demonstrate retained fast-mode throughput, not a controlled performance gain.

## Regression checks

`make probe-gpu-ui-snapshot` draws 17 records of 64 bytes through the production vertex and fragment argument
builder. Its 8,712 pixel / invariant checks cover CPU rewrites after capture, writable bindings, current-submission
GPU copies, prior GPU writes completed after capture, neighbouring uniform preservation, and argument epoch caching
on / off. Setting `TOTK_STORAGE_SNAPSHOT=0` makes it fail with "instance data changed after submit".

The final binary also passes the existing UBO snapshot, argument epoch and CPU graphics probes. Dispatch-chain and
texture-slot GPU probes passed with the same storage correction before removal of the disabled texture experiment.
All 45 project Python tests pass with `TOTK_build/venv/bin/python`. Evidence: `glyph-release*.{json,log}` and
`glyph-release-source-tests.log`.

Final runtime SHA-256: `5546778015ac0d00fc09655d971bf945842958bc7f94d9559fc8f1a5d06d8201`.

Longer play coverage of dialogue, map updates and other scenes is still needed. Other shared resources remain subject
to the existing late-fence experiment; this does not certify full-game correctness or address the reported shadow textures.
