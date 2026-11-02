#!/usr/bin/env python3
"""Profile-guided optimisation, step 2b: the object summary of the merged .gcda
files (after tools/switch/pgo-merge.sh).

__gcov_info_to_gcda, which the instrumented NRO writes its counts with
(src/platform/switch/pgo_stream.cpp), leaves every object's summary at zero:
runs = 0 and sum_max = 0. A program that exits through libgcov would have
written runs = 1 and sum_max = its largest counter, and -fprofile-use reads its
hot and never-run thresholds from those two numbers. This sets runs to the
number of sessions merged and sum_max to the largest arc counter over all the
files (capped to 32 bits, the field's size).

  python3 tools/switch/pgo_summary.py <profile folder> <sessions>

The .gcda layout (GCC 12 and later, little-endian target): a 16-byte header
(magic 'gcda', version, stamp, checksum), then records of a 32-bit tag, a
32-bit signed length in bytes and the data. A negative length is a block of
counters that are all zero, with no data. The summary's tag is 0xa1000000
(runs, sum_max), the arc counters' 0x01a10000 (64-bit values).
"""
import os
import struct
import sys

MAGIC = 0x67636461
TAG_SUMMARY = 0xA1000000
TAG_ARCS = 0x01A10000


def walk(data):
    """(tag, data offset, data length) of every record."""
    magic = struct.unpack_from('<I', data, 0)[0] if len(data) >= 4 else 0
    if magic != MAGIC:
        raise ValueError('not a little-endian .gcda file')
    pos = 16
    while pos + 8 <= len(data):
        tag, length = struct.unpack_from('<Ii', data, pos)
        size = max(length, 0)
        yield tag, pos + 8, size
        pos += 8 + size


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    folder, runs = sys.argv[1], int(sys.argv[2])
    paths = [os.path.join(d, f) for d, _, names in os.walk(folder)
             for f in names if f.endswith('.gcda')]
    if not paths:
        sys.exit('no .gcda files in ' + folder)
    largest = 0
    for path in paths:
        data = open(path, 'rb').read()
        for tag, start, size in walk(data):
            if tag == TAG_ARCS and size:
                values = struct.unpack_from('<%dQ' % (size // 8), data, start)
                largest = max(largest, max(values))
    sum_max = min(largest, 0xFFFFFFFF)
    patched = 0
    for path in paths:
        data = bytearray(open(path, 'rb').read())
        for tag, start, size in walk(data):
            if tag == TAG_SUMMARY and size >= 8:
                struct.pack_into('<II', data, start, runs, sum_max)
                patched += 1
                break
        with open(path, 'wb') as f:
            f.write(data)
    print('%d .gcda files: runs = %d, sum_max = %d (largest counter %d), '
          '%d summaries set' % (len(paths), runs, sum_max, largest, patched))


if __name__ == '__main__':
    main()
