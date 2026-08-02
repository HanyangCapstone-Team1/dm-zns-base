#!/usr/bin/env bash
# M2 ext4 round-trip test:
# mkfs -> mount -> write/hash -> unmount/remount -> hash comparison.
#
# Env: UNDERLYING (default /dev/nullb0), DM_NAME (default myzns-m2),
#      FILE_SIZE_MB (default 10).

set -uo pipefail

UNDERLYING=${UNDERLYING:-/dev/nullb0}
DM_NAME=${DM_NAME:-myzns-m2}
FILE_SIZE_MB=${FILE_SIZE_MB:-10}
DM_DEV=/dev/mapper/$DM_NAME
MOD_NAME=dm-zns-base
TARGET_NAME=zns-m1

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SRC_DIR=$(cd "$SCRIPT_DIR/../src" && pwd)
KO_PATH="$SRC_DIR/$MOD_NAME.ko"
MOUNT_DIR=
RAW_DIR=

[ "$(id -u)" -eq 0 ] || { echo "Run with sudo." >&2; exit 1; }

cleanup() {
	if [ -n "$MOUNT_DIR" ] && mountpoint -q "$MOUNT_DIR"; then
		umount "$MOUNT_DIR" 2>/dev/null || true
	fi
	dmsetup remove "$DM_NAME" 2>/dev/null || true
	rmmod "$MOD_NAME" 2>/dev/null || true
	[ -z "$MOUNT_DIR" ] || rmdir "$MOUNT_DIR" 2>/dev/null || true
	if [ -n "$RAW_DIR" ]; then
		rm -f "$RAW_DIR/before.bin" "$RAW_DIR/after.bin" "$RAW_DIR/zero.bin"
		rmdir "$RAW_DIR" 2>/dev/null || true
	fi
}
trap cleanup EXIT

[ -b "$UNDERLYING" ] || {
	echo "[!] $UNDERLYING is missing. Run scripts/nullblk-up.sh first." >&2
	exit 1
}

for cmd in dmsetup blkzone blockdev blkdiscard mkfs.ext4 mount umount md5sum; do
	command -v "$cmd" >/dev/null || {
		echo "[!] Required command is missing: $cmd" >&2
		exit 1
	}
done

if [ ! -f "$KO_PATH" ]; then
	echo "[*] Building module"
	make -C "$SRC_DIR" >/dev/null || { echo "[!] Build failed" >&2; exit 1; }
fi

dmsetup remove "$DM_NAME" 2>/dev/null || true
rmmod "$MOD_NAME" 2>/dev/null || true

echo "[*] Resetting all zones on $UNDERLYING"
blkzone reset "$UNDERLYING" || { echo "[!] Zone reset failed" >&2; exit 1; }

echo "[*] Loading $KO_PATH"
insmod "$KO_PATH" || { echo "[!] insmod failed" >&2; exit 1; }

sectors=$(blockdev --getsz "$UNDERLYING")
echo "0 $sectors $TARGET_NAME $UNDERLYING" | dmsetup create "$DM_NAME" || {
	echo "[!] dmsetup create failed" >&2
	exit 1
}

dm_base=$(basename "$(readlink -f "$DM_DEV")")
zoned=$(cat "/sys/block/$dm_base/queue/zoned")
echo "[*] $DM_DEV queue/zoned=$zoned"
if [ "$zoned" != "none" ]; then
	echo "[FAIL] DM device is not conventional" >&2
	exit 2
fi

logical_block_size=$(cat "/sys/block/$dm_base/queue/logical_block_size")
echo "[*] $DM_DEV logical_block_size=$logical_block_size"
if [ "$logical_block_size" -ne 4096 ]; then
	echo "[FAIL] DM logical block size is not 4096" >&2
	exit 2
fi

before=$(dmesg | grep -Ec 'blk_update_request|I/O error' || true)

echo "[*] Checking logical discard tombstone"
RAW_DIR=$(mktemp -d /tmp/dm-zns-discard.XXXXXX)
dd if=/dev/urandom of="$RAW_DIR/before.bin" bs=4K count=1 status=none
dd if="$RAW_DIR/before.bin" of="$DM_DEV" bs=4K count=1 seek=128 \
	oflag=direct conv=notrunc status=none || {
	echo "[FAIL] discard setup write failed" >&2
	exit 3
}
blkdiscard --offset $((128 * 4096)) --length 4096 "$DM_DEV" || {
	echo "[FAIL] logical discard failed" >&2
	exit 3
}
dd if="$DM_DEV" of="$RAW_DIR/after.bin" bs=4K count=1 skip=128 \
	iflag=direct status=none || {
	echo "[FAIL] read after discard failed" >&2
	exit 3
}
dd if=/dev/zero of="$RAW_DIR/zero.bin" bs=4K count=1 status=none
cmp "$RAW_DIR/zero.bin" "$RAW_DIR/after.bin" || {
	echo "[FAIL] discarded logical block did not read as zero" >&2
	exit 3
}
echo "[OK] discarded block reads as zero"

echo "[*] Creating ext4 (discard disabled)"
mkfs.ext4 -F -E nodiscard "$DM_DEV" >/dev/null || {
	echo "[FAIL] mkfs.ext4 failed" >&2
	exit 3
}

MOUNT_DIR=$(mktemp -d /tmp/dm-zns-m2.XXXXXX)
mount "$DM_DEV" "$MOUNT_DIR" || { echo "[FAIL] first mount failed" >&2; exit 4; }

echo "[*] Writing ${FILE_SIZE_MB} MiB test file"
dd if=/dev/urandom of="$MOUNT_DIR/data.bin" bs=1M count="$FILE_SIZE_MB" \
	status=none || { echo "[FAIL] file write failed" >&2; exit 5; }
hash_a=$(md5sum "$MOUNT_DIR/data.bin" | awk '{print $1}')
echo "    hash A: $hash_a"

sync
umount "$MOUNT_DIR" || { echo "[FAIL] unmount failed" >&2; exit 6; }
mount "$DM_DEV" "$MOUNT_DIR" || { echo "[FAIL] remount failed" >&2; exit 7; }

hash_b=$(md5sum "$MOUNT_DIR/data.bin" | awk '{print $1}')
echo "    hash B: $hash_b"
if [ "$hash_a" != "$hash_b" ]; then
	echo "[FAIL] hash mismatch after remount" >&2
	exit 8
fi

after=$(dmesg | grep -Ec 'blk_update_request|I/O error' || true)
delta=$((after - before))
echo "    block I/O error delta: $delta"
if [ "$delta" -ne 0 ]; then
	echo "[FAIL] block layer reported I/O errors" >&2
	exit 9
fi

echo
echo "=== M2 EXT4 ROUND-TRIP PASSED ==="
