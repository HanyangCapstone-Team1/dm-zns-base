#!/usr/bin/env bash
# LSM mapping test: run the M2 round-trip with enough data to rotate
# memtables and compact four L0 runs, then verify both events in dmesg.

set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
FILE_SIZE_MB=${FILE_SIZE_MB:-32}

[ "$(id -u)" -eq 0 ] || { echo "Run with sudo." >&2; exit 1; }

before_rotate=$(dmesg | grep -c 'zns-m1: memtable rotated' || true)
before_compact=$(dmesg | grep -c 'zns-m1: compaction:' || true)

FILE_SIZE_MB=$FILE_SIZE_MB bash "$SCRIPT_DIR/test-m2.sh" || exit $?

after_rotate=$(dmesg | grep -c 'zns-m1: memtable rotated' || true)
after_compact=$(dmesg | grep -c 'zns-m1: compaction:' || true)
rotate_delta=$((after_rotate - before_rotate))
compact_delta=$((after_compact - before_compact))

echo "    memtable rotations: $rotate_delta"
echo "    compactions:        $compact_delta"

if [ "$rotate_delta" -eq 0 ]; then
	echo "[FAIL] no memtable rotation was observed" >&2
	exit 10
fi

if [ "$compact_delta" -eq 0 ]; then
	echo "[FAIL] no LSM compaction was observed" >&2
	exit 11
fi

echo
echo "=== IN-MEMORY LSM CHECKS PASSED ==="
