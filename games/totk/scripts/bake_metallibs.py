#!/usr/bin/env python3
"""Precompile the runtime shader cache to .metallib files with Xcode's Metal compiler (TOTK_RT_METALLIB).

The game loads <key>.metallib next to each translated <key>.metal instead of compiling the source at launch, which
otherwise costs ~1 s per shader whenever macOS's shared Metal cache has been cleared (74 s before the first frame and
minutes of background compiling during play). The game bakes new shaders itself after its warm-up on two
background-priority threads; this script does a whole cache at once with every core (e.g. after a translation-chain
update, when every source is new). Same rules as the game (nvn_rt_metallib.h): the runtime's default language version
and safe math; the output carries the source's modification time and is used only while the times are equal; a
zero-size stamped file marks a source the compiler rejects.

Needs the Metal Toolchain once: xcodebuild -downloadComponent MetalToolchain

  bake_metallibs.py                 # this tree's cache: ~/Library/Application Support/MacNativeGames/TOTK/shader-cache/rt-<DMSH_CHAIN>
  bake_metallibs.py DIR [-j N] [--std metal4.1]
"""
import argparse, os, re, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


def default_dir():
    # the cache of this source tree's translation chain (dmsh_chain.h), as the game and bench.py pick it
    h = Path(__file__).resolve().parent.parent / "third_party/d2r_gfx/gfx/nvn/dmsh_chain.h"
    chain = re.search(r'#define DMSH_CHAIN "([^"]+)"', h.read_text()).group(1)
    d = Path.home() / f"Library/Application Support/MacNativeGames/TOTK/shader-cache/rt-{chain}"
    if not d.is_dir(): sys.exit(f"no shader cache at {d}")
    return d


def fresh(src, lib):
    try: return src.stat().st_mtime_ns == lib.stat().st_mtime_ns
    except FileNotFoundError: return False


def bake(cc, std, src):
    lib = src.with_suffix(".metallib"); tmp = lib.with_name(lib.name + f".tmp{os.getpid()}")
    t0 = src.stat().st_mtime_ns
    r = subprocess.run([cc, f"-std={std}", "-fmetal-math-mode=safe", "-w", "-o", str(tmp), str(src)], capture_output=True)
    if src.stat().st_mtime_ns != t0: tmp.unlink(missing_ok=True); return None   # rewritten meanwhile: the game re-bakes it
    if r.returncode: tmp.write_bytes(b"")
    os.utime(tmp, ns=(time.time_ns(), t0)); os.replace(tmp, lib)
    return r.returncode == 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dir", nargs="?", type=Path)
    ap.add_argument("-j", type=int, default=os.cpu_count())
    ap.add_argument("--std", default="metal4.1", help="the runtime's default MTLCompileOptions.languageVersion (macOS 27: 4.1)")
    a = ap.parse_args(); d = a.dir or default_dir()
    cc = subprocess.run(["xcrun", "-f", "metal"], capture_output=True, text=True).stdout.strip()
    if not cc or subprocess.run([cc, "--version"], capture_output=True).returncode:
        sys.exit("no Metal compiler: xcodebuild -downloadComponent MetalToolchain")
    todo = [s for s in d.glob("*.metal") if not fresh(s, s.with_suffix(".metallib"))]
    print(f"{d}: {len(todo)} to bake, {a.j} jobs", flush=True)
    t0 = time.time(); ok = bad = 0
    with ThreadPoolExecutor(a.j) as ex:
        for i, r in enumerate(ex.map(lambda s: bake(cc, a.std, s), todo), 1):
            ok += r is True; bad += r is False
            if i % 1000 == 0: print(f"  {i} / {len(todo)} ({time.time() - t0:.0f} s)", flush=True)
    print(f"done: {ok} baked, {bad} rejected by the compiler, {time.time() - t0:.0f} s")


if __name__ == "__main__":
    main()
