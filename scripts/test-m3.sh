#!/usr/bin/env bash
# M3 GC test:
# fill 80% of the device, overwrite the same logical range by 1.2x of the
# device capacity, and verify that zone GC/reset prevents ENOSPC.

set -euo pipefail

UNDERLYING=${UNDERLYING:-/dev/nullb0}
DM_NAME=${DM_NAME:-myzns-m3}
DM_DEV=/dev/mapper/$DM_NAME
MOD_NAME=dm-zns-base
TARGET_NAME=zns-m1
GC_POLICY=${GC_POLICY:-cost-benefit}
ACTIVE_ZONE_POOL_SIZE=${ACTIVE_ZONE_POOL_SIZE:-2}

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SRC_DIR=$(cd "$SCRIPT_DIR/../src" && pwd)
KO_PATH="$SRC_DIR/$MOD_NAME.ko"

[ "$(id -u)" -eq 0 ] || { echo "Run with sudo." >&2; exit 1; }

case "$GC_POLICY" in
	greedy|cost-benefit) ;;
	*) echo "[FAIL] GC_POLICY must be greedy or cost-benefit" >&2; exit 1 ;;
esac

case "$ACTIVE_ZONE_POOL_SIZE" in
	''|*[!0-9]*|0) echo "[FAIL] ACTIVE_ZONE_POOL_SIZE must be a positive integer" >&2; exit 1 ;;
esac

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

echo "[*] Loading $KO_PATH (GC policy: $GC_POLICY, active pool: $ACTIVE_ZONE_POOL_SIZE)"
insmod "$KO_PATH" gc_policy="$GC_POLICY" \
	active_zone_pool_size="$ACTIVE_ZONE_POOL_SIZE"

device_sectors=$(blockdev --getsz "$UNDERLYING")
echo "0 $device_sectors $TARGET_NAME $UNDERLYING" | dmsetup create "$DM_NAME"

pool_log=$(dmesg | grep 'zns-m1: active pool initialized:' | tail -n 1 || true)
if [ -z "$pool_log" ]; then
	echo "[FAIL] active zone pool initialization was not observed" >&2
	exit 2
fi
echo "[*] $pool_log"
before_pool_replacements=$(dmesg | grep -c 'zns-m1: active pool replace:' || true)

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

before_gc=$(dmesg | grep -Ec 'zns-m1: (compaction )?gc complete:' || true)
before_compaction_gc=$(dmesg | grep -c 'zns-m1: compaction gc complete:' || true)
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

after_gc=$(dmesg | grep -Ec 'zns-m1: (compaction )?gc complete:' || true)
after_compaction_gc=$(dmesg | grep -c 'zns-m1: compaction gc complete:' || true)
after_errors=$(dmesg | grep -Ec 'blk_update_request|I/O error' || true)
after_pool_replacements=$(dmesg | grep -c 'zns-m1: active pool replace:' || true)
gc_delta=$((after_gc - before_gc))
compaction_gc_delta=$((after_compaction_gc - before_compaction_gc))
error_delta=$((after_errors - before_errors))
pool_replacement_delta=$((after_pool_replacements - before_pool_replacements))

echo "    $GC_POLICY GC cycles: $gc_delta"
echo "    LSM compaction-coupled GC cycles: $compaction_gc_delta"
echo "    active pool replacements: $pool_replacement_delta"
echo "    block I/O error delta: $error_delta"

if [ "$pool_replacement_delta" -eq 0 ]; then
	echo "[FAIL] no active zone was replaced from the free pool" >&2
	exit 3
fi

if [ "$gc_delta" -eq 0 ]; then
	echo "[FAIL] overwrite completed without an observed GC/reset cycle" >&2
	exit 4
fi

if [ "$compaction_gc_delta" -eq 0 ]; then
	echo "[FAIL] LSM compaction did not trigger physical zone GC" >&2
	exit 5
fi

if [ "$error_delta" -ne 0 ]; then
	echo "[FAIL] block layer reported I/O errors" >&2
	exit 6
fi

echo
echo "=== M3 ${GC_POLICY^^} GC PASSED ==="
