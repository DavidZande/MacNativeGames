#!/usr/bin/env python3
"""Local change: CPU profile of a running game, guest code included.

Runs macOS `sample` on the game process for N seconds, computes self time per function from the call tree, and names
guest addresses as main+0x... / sdk+0x... (with the nearest exported symbol) from the module bases the loader logs
("[totk] mapped <module> ... base=0x...") and the TOTK_SYMBOL_MAP file. With --input-file it also counts host calls
(hletrace) over the same window, and with --perf the timer reads and system calls per frame (perf.csv).

  profile_guest.py --log <totk.log> --symbols <map> [--input-file <input.txt>] [--perf perf.csv] [--seconds 20] [--top 15]
"""
import argparse
import bisect
import collections
import os
import re
import subprocess
import sys
import time

WAITS = re.compile(r'__psynch_cvwait|__psynch_mutexwait|__semwait_signal|__workq_kernreturn|semaphore_(timed)?wait(_signal)?_trap|mach_msg2?_trap|'
                   r'swtch_pri|kevent|__select|__read_nocancel|__wait4|__sigsuspend|start_wqthread|thread_start|_pthread_start|__ulock_wait|nanosleep|usleep')
LINE = re.compile(r'^(?P<prefix>[\s+!:|]*)(?P<count>\d+)\s+(?P<what>.*?)(?:\s+\[(?P<addr>0x[0-9a-f]+)[^\]]*\])?\s*$')


def modules_from(log_text):
    mods = []
    for m in re.finditer(r'\[totk\] mapped (\w+) size=(\d+) symbols=\d+ base=(0x[0-9a-f]+)', log_text):
        mods.append((int(m.group(3), 16), int(m.group(2)), m.group(1)))
    return sorted(mods)


def symbols_from(path):
    table = collections.defaultdict(list)   # module -> sorted [(offset, size, name)]
    if not path or not os.path.exists(path): return table
    cur = None
    with open(path, errors='replace') as f:
        for line in f:
            p = line.split(' ', 3)
            if p[0] == 'module': cur = p[1]; continue
            if cur and len(p) >= 3: table[cur].append((int(p[0], 16), int(p[1], 16), p[2].strip()))
    for v in table.values(): v.sort()
    return table


def name_guest(addr, mods, syms):
    for base, size, name in mods:
        if base <= addr < base + size:
            off = addr - base; tab = syms.get(name, [])
            i = bisect.bisect_right(tab, (off, 1 << 64, '')) - 1
            if i >= 0 and tab[i][1] > 0 and off < tab[i][0] + tab[i][1]:   # only a symbol that contains the address
                return f'{name}+{off:#x} ({tab[i][2]}+{off - tab[i][0]:#x})'
            return f'{name}+{off:#x}'
    return None


def self_times(sample_text):
    """Self samples per frame label from `sample`'s call graph (a node's count minus its children's)."""
    graph = sample_text.split('Call graph:', 1)[-1].split('Total number in stack', 1)[0]
    nodes = []   # (depth, count, what, addr)
    for line in graph.splitlines():
        m = LINE.match(line)
        if not m or not m.group('what'): continue
        nodes.append((len(m.group('prefix')), int(m.group('count')), m.group('what'), m.group('addr')))
    selfc = collections.Counter(); total = 0
    for i, (d, c, what, addr) in enumerate(nodes):
        children = 0; j = i + 1
        child_depth = nodes[j][0] if j < len(nodes) and nodes[j][0] > d else None
        while child_depth is not None and j < len(nodes) and nodes[j][0] > d:
            if nodes[j][0] == child_depth: children += nodes[j][1]
            j += 1
        s = c - children
        if s > 0: selfc[(what, addr)] += s; total += s
    return selfc, total


def report(sample_text, mods, syms, top):
    """Top functions by self time among busy samples (threads waiting in the kernel are left out)."""
    selfc, total = self_times(sample_text)
    named = collections.Counter(); guest = busy = 0
    for (what, addr), n in selfc.items():
        if WAITS.search(what): continue
        busy += n
        g = name_guest(int(addr, 16), mods, syms) if addr and ('???' in what or 'unknown' in what) else None
        if g:   # by symbol, else by 4 KiB page of the module
            guest += n; m = re.match(r'(\w+)\+(0x[0-9a-f]+)(?: \((.+)\+0x[0-9a-f]+\))?', g)
            key = f'guest {m.group(1)}: {m.group(3)}' if m.group(3) else f'guest {m.group(1)}+{int(m.group(2), 16) & ~0xfff:#x}'
        else:
            w = what.split('  [0x')[0]
            w = re.sub(r'\s+\+\s+[\d,.]+\s*$', '', w); key = re.sub(r'\s+\(in ([^)]*)\)', r' [\1]', w).strip()
        named[key] += n
    print(f'{total} samples, {busy} busy ({100 * busy / max(total, 1):.1f}%); guest code {100 * guest / max(busy, 1):.1f}% of busy')
    per_mod = collections.Counter()
    for k, n in named.items():
        if k.startswith('guest '): per_mod[k.split()[1].rstrip(':+').split('+')[0]] += n
    print('guest code by module: ' + ', '.join(f'{m} {100 * n / max(busy, 1):.1f}%' for m, n in per_mod.most_common()))
    print(f'top {top} by self time (share of busy samples):')
    for k, n in named.most_common(top): print(f'  {100 * n / max(busy, 1):5.1f}%  {k}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--pid', type=int); ap.add_argument('--log', required=True); ap.add_argument('--symbols')
    ap.add_argument('--input-file'); ap.add_argument('--perf'); ap.add_argument('--seconds', type=int, default=20)
    ap.add_argument('--top', type=int, default=15); ap.add_argument('--out')
    ap.add_argument('--from-sample', help='analyze an existing sample file instead of sampling')
    a = ap.parse_args()
    if a.from_sample: return report(open(a.from_sample, errors='replace').read(), *(lambda t: (modules_from(t), symbols_from(a.symbols)))(open(a.log, errors='replace').read()), a.top)
    pid = a.pid or int(subprocess.run(['pgrep', '-f', r'totk-native(-[0-9]+)? rewritten'], capture_output=True, text=True).stdout.split()[0])
    log_text = open(a.log, errors='replace').read()
    mods, syms = modules_from(log_text), symbols_from(a.symbols)
    perf_rows0 = sum(1 for _ in open(a.perf)) if a.perf else 0
    log_mark = len(log_text)
    if a.input_file: open(a.input_file, 'a').write('hletrace 1\n')
    out = a.out or f'/tmp/totk-sample-{pid}.txt'
    subprocess.run(['sample', str(pid), str(a.seconds), '-mayDie', '-file', out], capture_output=True)
    if a.input_file: open(a.input_file, 'a').write('hletrace 0\n'); time.sleep(1.5)
    report(open(out, errors='replace').read(), mods, syms, a.top)
    if a.perf:
        rows = [l.split(',') for l in open(a.perf).read().splitlines()[perf_rows0:] if l and l[0].isdigit()]
        hdr = [l for l in open(a.perf).read().splitlines() if l.startswith('time,')][-1].split(',')
        col = lambda name: [float(r[hdr.index(name)]) for r in rows if len(r) == len(hdr)]
        frames = sum(col('fps')) or 1
        print(f'per frame over {len(rows)} s ({frames:.0f} frames): timer reads {sum(col("timer_reads")) / frames:.0f}, system calls {sum(col("svc_calls")) / frames:.0f}')
    if a.input_file:
        hle = re.findall(r'\[hle\]\s+(\d+)\s+(.+)', open(a.log, errors='replace').read()[log_mark:])
        if hle:
            frames = frames if a.perf else 1
            pick = [(int(n), w) for n, w in hle if re.search(r'mem(cpy|set|move)', w)]
            print('host calls per frame (memcpy / memset / memmove):', ', '.join(f'{w.strip()} {n / frames:.0f}' for n, w in pick) or 'none')
            print('top host calls per frame:', ', '.join(f'{w.strip()} {int(n) / frames:.0f}' for n, w in hle[:8]))


if __name__ == '__main__':
    sys.exit(main())
