#!/usr/bin/env python3
"""
Count positions in .binpack and .vf / .vf.zst (Chess/Chess960/DFRC) files.

Usage:
    python3 binpack-check.py [file_or_dir ...]
    python3 binpack-check.py /mnt/d/Bulk-Data
"""

import os
import sys
import struct
import subprocess
from pathlib import Path
from collections import defaultdict

ENTRY_SIZE = 32
VF_HEADER_SIZE = 35
VF_SENTINEL_SCORE = -32768

try:
    import zstandard
    HAS_ZSTD = True
except ImportError:
    HAS_ZSTD = False


def normalize_path(path):
    """Translate Windows drive path like D:\\Bulk-Data to /mnt/d/Bulk-Data on Linux."""
    if os.name != 'nt' and len(path) >= 2 and path[0].isalpha() and path[1] == ':':
        drive = path[0].lower()
        rest = path[2:].replace('\\', '/').lstrip('/')
        return f"/mnt/{drive}/{rest}" if rest else f"/mnt/{drive}"
    return path


def detect_format(path):
    """Return 'binpack', 'vf', or None based on extension."""
    name = os.path.basename(path)
    if name.endswith('.binpack.zst') or name.endswith('.binpack'):
        return 'binpack'
    if name.endswith('.vf.zst') or name.endswith('.vf'):
        return 'vf'
    return None


def is_zst(path):
    return path.endswith('.zst')


class SubprocessStream:
    def __init__(self, path):
        self.proc = subprocess.Popen(['zstd', '-dc', path], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)

    def read(self, n=-1):
        return self.proc.stdout.read(n)

    def close(self):
        if self.proc.stdout:
            self.proc.stdout.close()
        self.proc.wait()


def open_stream(path):
    if is_zst(path):
        if HAS_ZSTD:
            f = open(path, 'rb')
            return zstandard.ZstdDecompressor().stream_reader(f)
        return SubprocessStream(path)
    return open(path, 'rb')


def describe_binpack_entry(entry):
    """Decode first binpack entry for a sanity-check (binpack layout only)."""
    try:
        score = struct.unpack('<h', entry[2:4])[0]
        move = struct.unpack('<H', entry[4:6])[0]
        result = struct.unpack('<H', entry[6:8])[0]
        occ = struct.unpack('<Q', entry[8:16])[0]
        return {'score': score, 'move': move, 'result': result,
                'occupied_squares': bin(occ).count('1')}
    except Exception:
        return None


def count_vf_stream(stream):
    """Count scored positions in a compact .vf / .vf.zst stream."""
    scored_positions = 0
    games = 0
    leftover = b''
    block = 1 << 20  # 1 MB

    while True:
        chunk = stream.read(block)
        if not chunk:
            break
        data = leftover + chunk
        offset = 0
        n = len(data)
        while offset + VF_HEADER_SIZE <= n:
            count = struct.unpack_from('<H', data, offset + 33)[0]
            game_len = VF_HEADER_SIZE + count * 4
            if offset + game_len > n:
                break
            rec_start = offset + VF_HEADER_SIZE
            for i in range(count):
                score = struct.unpack_from('<h', data, rec_start + i * 4 + 2)[0]
                if score != VF_SENTINEL_SCORE:
                    scored_positions += 1
            offset += game_len
            games += 1
        leftover = data[offset:]

    truncated = len(leftover) > 0
    return scored_positions, {'info': {'games': games}, 'truncated': truncated}


def count_file(path, verbose=False):
    """Return the number of positions in the file."""
    fmt = detect_format(path)
    stream = open_stream(path)
    try:
        if fmt == 'vf':
            return count_vf_stream(stream)

        n_entries = 0
        first_info = None
        leftover = b''

        if verbose and fmt == 'binpack':
            first = stream.read(ENTRY_SIZE)
            if len(first) < ENTRY_SIZE:
                return 0, {'info': None, 'truncated': len(first) > 0}
            first_info = describe_binpack_entry(first)
            n_entries = 1

        block = 1 << 20  # 1 MB
        while True:
            chunk = stream.read(block)
            if not chunk:
                break
            data = leftover + chunk
            n_full = len(data) // ENTRY_SIZE
            n_entries += n_full
            leftover = data[n_full * ENTRY_SIZE:]

        truncated = len(leftover) > 0
        return n_entries, {'info': first_info, 'truncated': truncated}
    finally:
        stream.close()


def gather_files(args):
    """Expand directories into binpack + vf files (both .zst and raw)."""
    files = []
    for a in args:
        p = Path(normalize_path(a))
        if p.is_dir():
            for f in sorted(p.iterdir()):
                if detect_format(str(f)) is not None:
                    files.append(str(f))
        elif p.is_file():
            if detect_format(str(p)) is not None:
                files.append(str(p))
            else:
                print(f"warning: unknown format (skipping): {a}", file=sys.stderr)
        else:
            print(f"warning: not a file or directory: {a}", file=sys.stderr)
    return files


def fmt_size(n):
    if n >= 1e9:
        return f"{n/1e9:.2f} GB"
    if n >= 1e6:
        return f"{n/1e6:.2f} MB"
    if n >= 1e3:
        return f"{n/1e3:.2f} KB"
    return f"{n} B"


def main():
    args = sys.argv[1:] if len(sys.argv) >= 2 else ['/mnt/d/Bulk-Data']

    files = gather_files(args)
    if not files:
        print("no .binpack or .vf files found", file=sys.stderr)
        sys.exit(1)

    totals = defaultdict(int)
    counts = defaultdict(int)
    truncated_files = []

    print(f"{'format':<8s} {'file':<55s} {'positions':>18s} {'size':>12s}")
    print('-' * 97)

    for path in files:
        fmt = detect_format(path) or '?'
        try:
            size_bytes = os.path.getsize(path)
            n, meta = count_file(path, verbose=(fmt == 'binpack'))

            print(f"{fmt:<8s} {os.path.basename(path):<55s} {n:>18,} {fmt_size(size_bytes):>12s}")

            if fmt == 'binpack' and meta and meta['info']:
                info = meta['info']
                print(f"         first entry: score={info['score']} "
                      f"move=0x{info['move']:04x} result={info['result']} "
                      f"occupied_squares={info['occupied_squares']}")

            if meta and meta['truncated']:
                truncated_files.append(path)

            totals[fmt] += n
            counts[fmt] += 1
        except Exception as e:
            print(f"{fmt:<8s} {os.path.basename(path):<55s} ERROR: {e}")

    print('-' * 97)

    grand_total = 0
    for fmt in sorted(totals.keys()):
        print(f"{fmt:<8s} {'subtotal ('+str(counts[fmt])+' files)':<55s} "
              f"{totals[fmt]:>18,}  ({totals[fmt]/1e9:.3f} B)")
        grand_total += totals[fmt]

    print('-' * 97)
    print(f"{'':<8s} {'GRAND TOTAL':<55s} {grand_total:>18,}  ({grand_total/1e9:.3f} B)")

    if truncated_files:
        print(f"\nNote: {len(truncated_files)} file(s) had a trailing partial entry.")
        print("  Normal for binpack chunk headers / VF file headers. Ignore.")


if __name__ == '__main__':
    main()