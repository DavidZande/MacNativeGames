#!/usr/bin/env python3
"""List the NVN shaders in TOTK's AGL shader archives (.sharcb, magic BAHS), which the BNSH-based seed pass skips.

These archives are where most of the shaders that looked "runtime-built" come from: they hold complete GLSLC output
(a 0x880-byte control section plus NVN code) for every compiled variation, but in AGL's own container, not BNSH.

Layout (reverse-engineered from the 1.0.0 files; no published spec):
  header  u32 @0x1c                  code region base
  stage record, 4-byte aligned:      u32 code_off (from code base), u32 0, u32 control_size (0x880),
                                     s32 control_rel (from the record's own offset)
  control                            GLSLC control section, magic 0x98761234; code length @+0x704, stage @+0x714
                                     (0 vertex, 1 fragment, 5 compute). Identical controls are shared between records.
  code                               base + code_off, NVN header magic 0x12345678 (graphics) / 0x12345679 (compute)
The runtime key (nvn_mtl.m prog_get) is sha256(control[0:0x880] + code[0:length]), the same as for BNSH shaders.

Usage: enumerate_agl_sharcb.py <verified romfs dir (index.json + romfs.bin)> [--json out.json]
Prints a per-archive count; --json writes one row per unique key (no game bytes, only hashes and offsets).
The stages() generator yields (key, stage, control, code) tuples shaped like prepare_shader_seed.bnsh_shaders().
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

ARCHIVES = ('Lib/agl/agl_resource.Nin_NX_NVN.release.sarc.zs',
            'Lib/gsys/gsys_resource.Nin_NX_NVN.release.sarc.zs',
            'Lib/Terrain/tera_resource.Nin_NX_NVN.release.sarc.zs',
            'Shader/ApplicationPackage.Nin_NX_NVN.release.sarc.zs')
STAGES = {0: 'vs', 1: 'ps', 5: 'cs'}


def sarc(data):
    header, size, start = struct.unpack_from('<HxxII', data, 4)
    count = struct.unpack_from('<H', data, header+6)[0]
    names = header+12+count*16+8
    for i in range(count):
        _, attr, lo, hi = struct.unpack_from('<4I', data, header+12+i*16)
        off = names+(attr & 0xffffff)*4
        yield data[off:data.index(0, off, start)].decode(), data[start+lo:start+hi]


def stages(d):
    """Yield (key, stage, control, code, info) for every distinct control/code pair in one .sharcb."""
    if d[:4] != b'BAHS':
        raise ValueError('not an AGL shader archive')
    base = struct.unpack_from('<I', d, 0x1c)[0]
    seen = set()
    for r in range(0x20, len(d)-16, 4):
        off, zero, csize, rel = struct.unpack_from('<IIIi', d, r)
        if zero or csize != 0x880:
            continue
        c, code = r+rel, base+off
        if not (0 <= c <= len(d)-0x880 and 0 <= code <= len(d)-4):
            continue
        if struct.unpack_from('<I', d, c)[0] != 0x98761234 or struct.unpack_from('<I', d, code)[0] not in (0x12345678, 0x12345679):
            continue
        c1, length = struct.unpack_from('<II', d, c+0x700)
        stage = STAGES.get(struct.unpack_from('<I', d, c+0x714)[0])
        if not stage or not 0 < length <= len(d)-code or c1 > length or (c, code) in seen:
            continue
        seen.add((c, code))
        control, body = d[c:c+0x880], d[code:code+length]
        yield hashlib.sha256(control+body).hexdigest(), stage, control, body, dict(record=r, control=c, code=code, length=length)


def archives(romfs):
    import compression.zstd as zstd
    index = json.loads((romfs/'index.json').read_text())
    if index.get('status') != 'pass':
        raise ValueError('RomFS has not passed integrity verification')
    entries = {r['path']: r for r in index['files']}
    with (romfs/'romfs.bin').open('rb') as f:
        def read(name):
            f.seek(entries[name]['offset'])
            return f.read(entries[name]['size'])
        dicts = {}
        for _, raw in sarc(zstd.decompress(read('Pack/ZsDic.pack.zs'))):
            z = zstd.ZstdDict(raw)
            dicts[z.dict_id] = z
        for name in ARCHIVES:
            raw = read(name)
            info = zstd.get_frame_info(raw)
            for member, data in sarc(zstd.decompress(raw, zstd_dict=dicts.get(info.dictionary_id))):
                if data[:4] == b'BAHS':
                    yield f'{name}/{member}', data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('romfs', type=Path)
    ap.add_argument('--json', type=Path)
    args = ap.parse_args()
    rows = {}
    for path, data in archives(args.romfs):
        n = 0
        for key, stage, _, _, info in stages(data):
            n += 1
            rows.setdefault(key, dict(key=key, stage=stage, archive=path, **info))
        print(f'{path}: {n} stage binaries')
    print(f'{len(rows)} unique shaders')
    if args.json:
        args.json.write_text(json.dumps(sorted(rows.values(), key=lambda r: r['key']), indent=1))


if __name__ == '__main__':
    main()
