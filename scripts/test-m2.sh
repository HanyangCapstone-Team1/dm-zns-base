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

[ "$(id -u)" -eq 0 ] || { echo "Run with sudo." >&2; exit 1; }

cleanup() {
	if [ -n "$MOUNT_DIR" ] && mountpoint -q "$MOUNT_DIR"; then
		umount "$MOUNT_DIR" 2>/dev/null || true
	fi
	dmsetup remove "$DM_NAME" 2>/dev/null || true
	rmmod "$MOD_NAME" 2>/dev/null || true
	[ -z "$MOUNT_DIR" ] || rmdir "$MOUNT_DIR" 2>/dev/null || true
}
trap cleanup EXIT

[ -b "$UNDERLYING" ] || {
	echo "[!] $UNDERLYING is missing. Run scripts/nullblk-up.sh first." >&2
	exit 1
}

for cmd in dmsetup blkzone blockdev mkfs.ext4 mount umount md5sum; do
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

before=$(dmesg | grep -Ec 'blk_update_request|I/O error' || true)

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
