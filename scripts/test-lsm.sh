#!/usr/bin/env bash
# Validate events in a continuously captured, loss-checked kernel log.
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
FILE_SIZE_MB=${FILE_SIZE_MB:-32}
[ "$(id -u)" -eq 0 ] || { echo "Run with sudo." >&2; exit 1; }
source "$SCRIPT_DIR/test-kernel-log.sh"
trap kernel_log_abort EXIT
kernel_log_start
rc=0
FILE_SIZE_MB=$FILE_SIZE_MB bash "$SCRIPT_DIR/test-m2.sh" || rc=$?
kernel_log_finish
[ "$rc" -eq 0 ] || exit "$rc"
rotate_count=$(grep -c 'zns-m1: memtable rotated:' "$KERNEL_LOG" || true)
compact_count=$(grep -c 'zns-m1: compaction:' "$KERNEL_LOG" || true)
error_count=$(grep -Ec 'blk_update_request|I/O error|compaction failed:' "$KERNEL_LOG" || true)
echo "    memtable rotations: $rotate_count"
echo "    compactions:        $compact_count"
if [ "$rotate_count" -le 0 ]; then
    echo "[FAIL] no memtable rotation was observed" >&2; exit 10
fi
if [ "$compact_count" -le 0 ]; then
    echo "[FAIL] no LSM compaction was observed" >&2; exit 11
fi
if [ "$error_count" -ne 0 ]; then
    echo "[FAIL] kernel reported I/O or compaction errors" >&2; exit 12
fi
echo
echo "=== IN-MEMORY LSM CHECKS PASSED ==="
