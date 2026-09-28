#!/usr/bin/env bash
# M3 greedy GC test:
# fill 80% of the device, overwrite the same logical range by 1.2x of the
# device capacity, and verify that zone GC/reset prevents ENOSPC.

set -euo pipefail

UNDERLYING=${UNDERLYING:-/dev/nullb0}
DM_NAME=${DM_NAME:-myzns-m3}
DM_DEV=/dev/mapper/$DM_NAME
MOD_NAME=dm-zns-base
TARGET_NAME=zns-m1

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SRC_DIR=$(cd "$SCRIPT_DIR/../src" && pwd)
KO_PATH="$SRC_DIR/$MOD_NAME.ko"

[ "$(id -u)" -eq 0 ] || { echo "Run with sudo." >&2; exit 1; }

cleanup() {
	dmsetup remove "$DM_NAME" 2>/dev/null || true
	rmmod "$MOD_NAME" 2>/dev/null || true
}
trap cleanup EXIT

[ -b "$UNDERLYING" ] || {
	echo "[FAIL] $UNDERLYING is missing. Run scripts/nullblk-up.sh first." >&2
	exit 1
}

for cmd in fio dmsetup blkzone blockdev; do
	command -v "$cmd" >/dev/null || {
		echo "[FAIL] Required command is missing: $cmd" >&2
		exit 1
	}
done

echo "[*] Building module"
make -C "$SRC_DIR" >/dev/null

dmsetup remove "$DM_NAME" 2>/dev/null || true
rmmod "$MOD_NAME" 2>/dev/null || true

echo "[*] Resetting all zones on $UNDERLYING"
blkzone reset "$UNDERLYING"

echo "[*] Loading $KO_PATH"
insmod "$KO_PATH"

device_sectors=$(blockdev --getsz "$UNDERLYING")
echo "0 $device_sectors $TARGET_NAME $UNDERLYING" | dmsetup create "$DM_NAME"

dm_base=$(basename "$(readlink -f "$DM_DEV")")
zoned=$(cat "/sys/block/$dm_base/queue/zoned")
if [ "$zoned" != "none" ]; then
	echo "[FAIL] DM device is not conventional" >&2
	exit 2
fi

device_bytes=$((device_sectors * 512))
range_bytes=$((device_bytes * 80 / 100 / 4096 * 4096))
overwrite_bytes=$((device_bytes * 120 / 100 / 4096 * 4096))
verify_bytes=$((64 * 1024 * 1024))
if [ "$verify_bytes" -gt "$range_bytes" ]; then
	verify_bytes=$range_bytes
fi

before_gc=$(dmesg | grep -c 'zns-m1: gc complete:' || true)
before_errors=$(dmesg | grep -Ec 'blk_update_request|I/O error' || true)

echo "[*] Filling 80% of the logical device with unique random writes"
fio --name=m3-fill --filename="$DM_DEV" --rw=randwrite \
	--bs=4k --size="$range_bytes" --io_size="$range_bytes" \
	--ioengine=libaio --iodepth=32 --direct=1 --randrepeat=1

echo "[*] Randomly overwriting the same range by 1.2x device capacity"
fio --name=m3-overwrite --filename="$DM_DEV" --rw=randwrite \
	--bs=4k --size="$range_bytes" --io_size="$overwrite_bytes" \
	--ioengine=libaio --iodepth=32 --direct=1 --norandommap=1 \
	--randrepeat=1 --group_reporting

echo "[*] Verifying data integrity after GC"
fio --name=m3-verify --filename="$DM_DEV" --rw=randwrite \
	--bs=4k --size="$verify_bytes" --ioengine=libaio --iodepth=32 \
	--direct=1 --verify=crc32c --verify_fatal=1 --do_verify=1 \
	--randrepeat=1 --group_reporting

after_gc=$(dmesg | grep -c 'zns-m1: gc complete:' || true)
after_errors=$(dmesg | grep -Ec 'blk_update_request|I/O error' || true)
gc_delta=$((after_gc - before_gc))
error_delta=$((after_errors - before_errors))

echo "    greedy GC cycles: $gc_delta"
echo "    block I/O error delta: $error_delta"

if [ "$gc_delta" -eq 0 ]; then
	echo "[FAIL] overwrite completed without an observed GC/reset cycle" >&2
	exit 3
fi

if [ "$error_delta" -ne 0 ]; then
	echo "[FAIL] block layer reported I/O errors" >&2
	exit 4
fi

echo
echo "=== M3 GREEDY GC PASSED ==="
