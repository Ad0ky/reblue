#!/usr/bin/env bash
# Profile-guided optimisation, step 2: the instrumented NRO's counts
# (sdmc:/switch/reblue/pgo/reblue_pgo_<N>.stream, one per session) into one
# .gcda file per object in pgo/ (gcov-tool merge-stream adds the sessions up).
# Runs in the switch-dev image, where the build ran: the streams name the files
# by the build's paths (/work/pgo/...). tools/switch/build-switch.ps1 -PgoMerge
# runs this and then tools/switch/pgo_summary.py (host image).
#
#   docker run --rm -v "$PWD:/work" -v "<streams>:/streams:ro" -w /work <switch image> \
#       bash tools/switch/pgo-merge.sh /streams
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
streams="${1:?usage: pgo-merge.sh <folder with reblue_pgo_*.stream>}"
: "${DEVKITPRO:=/opt/devkitpro}"
tool="$DEVKITPRO/devkitA64/bin/aarch64-none-elf-gcov-tool"
[ -x "$tool" ] || { echo "error: $tool not found (run in the switch-dev image)" >&2; exit 1; }

shopt -s nullglob
files=("$streams"/reblue_pgo_*.stream)
[ ${#files[@]} -gt 0 ] || { echo "error: no reblue_pgo_*.stream in $streams" >&2; exit 1; }

mkdir -p "$root/pgo"
old=("$root"/pgo/*.gcda)
if [ ${#old[@]} -gt 0 ]; then
    # merge-stream adds to what is there: merging the same streams twice would
    # count them twice. Keep the old profile aside instead.
    keep="$root/pgo.old.$(date +%Y%m%d-%H%M%S)"
    mv "$root/pgo" "$keep"
    mkdir -p "$root/pgo"
    echo "the previous profile moved to $(basename "$keep")"
fi

for f in "${files[@]}"; do
    printf 'merging %s (%s bytes)\n' "$(basename "$f")" "$(stat -c %s "$f")"
    "$tool" merge-stream "$f"
done

count=$(find "$root/pgo" -name '*.gcda' | wc -l)
[ "$count" -gt 0 ] || { echo "error: no .gcda written in pgo/: the streams name another folder (build and merge on the same PC)" >&2; exit 1; }
echo "${#files[@]} session(s) merged into $count .gcda file(s) in pgo/"
