#!/usr/bin/env bash
# Phase 9 benchmark matrix (PLAN item 32/33). All runs are warm-page-cache
# (cold cache would need root to drop caches; methodology noted in
# docs/benchmarks.md). Output is the machine-readable result stream —
# capture it, e.g.:  bench/run_bench.sh | tee bench/results-$(date +%F).log
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
B="$BUILD/arch_bench"
SHIM="$BUILD/libnetfs_delay.so"
DATA="${DATA:-$HOME/benchdata}"
C="--data=$DATA"

[ -x "$B" ] || { echo "build arch_bench first (cmake --build build --target arch_bench netfs_delay)" >&2; exit 1; }
mkdir -p "$DATA"

sec() { echo; echo "# ---------------------------------------------------------------------------"; echo "# $*"; echo; }

sec "0. workload generation"
for w in small mixed big; do "$B" --workload=$w --op=gen $C; done

sec "1. local NVMe (warm): tar --zstd -3 / tar store vs arch create -c zstd -l3 / extract / verify"
for w in small mixed big; do
    "$B" --workload=$w --op=all --no-gen $C --threads=4 --depth=32
done

sec "2. pipeline depth scaling (small workload, 20k files, no latency)"
for d in 1 2 4 8 16 32 64; do
    "$B" --workload=small --op=create --no-gen --no-tar $C --depth=$d
done

sec "3. simulated network FS (LD_PRELOAD: 500 us per open + first read)"
export NETFS_PREFIX="$DATA/small" NETFS_DELAY_US=500
LD_PRELOAD="$SHIM" "$B" --workload=small --op=create --no-gen $C --threads=4 --depth=1
LD_PRELOAD="$SHIM" "$B" --workload=small --op=create --no-gen $C --threads=4 --depth=32
# Phase 10: reader depth sweep (latency knob) — docs/extract-pipeline.md §3
LD_PRELOAD="$SHIM" "$B" --workload=small --op=extract --no-gen --no-tar $C --threads=4 --depth=1
LD_PRELOAD="$SHIM" "$B" --workload=small --op=extract --no-gen --no-tar $C --threads=4 --depth=4
LD_PRELOAD="$SHIM" "$B" --workload=small --op=extract --no-gen --no-tar $C --threads=4 --depth=32
LD_PRELOAD="$SHIM" "$B" --workload=small --op=extract --no-gen --no-tar $C --threads=4 --depth=128
LD_PRELOAD="$SHIM" "$B" --workload=small --op=verify --no-gen --no-tar $C --threads=4 --depth=32
unset LD_PRELOAD NETFS_PREFIX NETFS_DELAY_US

sec "4. memory profiling: writer threads sweep (big workload, 1 GiB, 4 MiB blocks)"
for t in 1 4 12; do
    "$B" --workload=big --op=create --no-gen --no-tar $C --threads=$t
done

sec "5. memory profiling: dedup on/off at data scale + per-entry container cost"
"$B" --workload=big --op=create --no-gen --no-tar $C
"$B" --workload=big --op=create --no-gen --no-tar $C --dedup
"$B" --op=dedupram --no-gen $C

sec "done"
