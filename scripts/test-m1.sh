#!/usr/bin/env bash
# M1 smoke test: expose a conventional DM device on top of zoned null_blk,
# then verify that 4 KiB random writes do not hit ZNS sequential-write errors.
#
# Env: UNDERLYING (default /dev/nullb0), DM_NAME (default myzns-m1),
#      SIZE (default 100M).

set -uo pipefail

UNDERLYING=${UNDERLYING:-/dev/nullb0}
DM_NAME=${DM_NAME:-myzns-m1}
DM_DEV=/dev/mapper/$DM_NAME
MOD_NAME=dm-zns-base
TARGET_NAME=zns-m1
SIZE=${SIZE:-100M}
TMP_DIR=

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SRC_DIR=$(cd "$SCRIPT_DIR/../src" && pwd)
KO_PATH="$SRC_DIR/$MOD_NAME.ko"

[ "$(id -u)" -eq 0 ] || { echo "Run with sudo." >&2; exit 1; }

cleanup() {
	dmsetup remove "$DM_NAME" 2>/dev/null || true
	rmmod $MOD_NAME 2>/dev/null || true
	[ -z "$TMP_DIR" ] || rm -rf "$TMP_DIR"
}
trap cleanup EXIT

[ -b "$UNDERLYING" ] || {
	echo "[!] $UNDERLYING is missing. Run scripts/nullblk-up.sh first." >&2
	exit 1
}

command -v fio >/dev/null || {
	echo "[!] fio is missing. Install fio first." >&2
	exit 1
}

if [ ! -f "$KO_PATH" ]; then
	echo "[*] Building module"
	make -C "$SRC_DIR" >/dev/null || { echo "[!] Build failed" >&2; exit 1; }
fi

dmsetup remove "$DM_NAME" 2>/dev/null || true
rmmod $MOD_NAME 2>/dev/null || true

echo "[*] Resetting all zones on $UNDERLYING"
blkzone reset "$UNDERLYING" || { echo "[!] blkzone reset failed" >&2; exit 1; }

echo "[*] insmod $KO_PATH"
insmod "$KO_PATH" || { echo "[!] insmod failed" >&2; exit 1; }

sectors=$(blockdev --getsz "$UNDERLYING")
echo "[*] dmsetup create $DM_NAME ($TARGET_NAME on $UNDERLYING, $sectors sectors)"
echo "0 $sectors $TARGET_NAME $UNDERLYING" | dmsetup create "$DM_NAME" || {
	echo "[!] dmsetup create failed" >&2; exit 1;
}

dm_base=$(basename "$(readlink -f "$DM_DEV")")

echo
echo "=== [1/4] DM queue should be conventional ==="
zoned=$(cat "/sys/block/$dm_base/queue/zoned")
echo "queue/zoned=$zoned"
if [ "$zoned" != "none" ]; then
	echo "[FAIL] M1 target must not expose a zoned queue upward" >&2
	exit 2
fi
echo "[OK]"

echo
echo "=== [2/4] fio random write + verify ($SIZE, 4 KiB) ==="
before=$(dmesg | grep -c 'blk_update_request')
if ! fio --name=m1-randwrite --filename="$DM_DEV" --rw=randwrite \
	--bs=4k --size="$SIZE" --ioengine=libaio --iodepth=32 --direct=1 \
	--verify=crc32c --verify_fatal=1 --do_verify=1; then
	echo "[FAIL] fio failed" >&2
	exit 3
fi

echo
echo "=== [3/4] overwrite should read back the latest data ==="
TMP_DIR=$(mktemp -d)
dd if=/dev/urandom of="$TMP_DIR/a.bin" bs=4096 count=1 status=none
dd if=/dev/urandom of="$TMP_DIR/b.bin" bs=4096 count=1 status=none
dd if="$TMP_DIR/a.bin" of="$DM_DEV" bs=4096 count=1 seek=2048 \
	oflag=direct conv=notrunc status=none || { echo "[FAIL] first overwrite write failed" >&2; exit 4; }
dd if="$TMP_DIR/b.bin" of="$DM_DEV" bs=4096 count=1 seek=2048 \
	oflag=direct conv=notrunc status=none || { echo "[FAIL] second overwrite write failed" >&2; exit 4; }
dd if="$DM_DEV" of="$TMP_DIR/out.bin" bs=4096 count=1 skip=2048 \
	iflag=direct status=none || { echo "[FAIL] overwrite read failed" >&2; exit 4; }
cmp "$TMP_DIR/b.bin" "$TMP_DIR/out.bin" || {
	echo "[FAIL] overwrite read did not return the latest data" >&2
	exit 4
}
echo "[OK]"

after=$(dmesg | grep -c 'blk_update_request')
delta=$((after - before))
echo "blk_update_request delta: $delta"
if [ "$delta" -ne 0 ]; then
	echo "[FAIL] underlying device reported write errors" >&2
	exit 3
fi
echo "[OK]"

echo
echo "=== [4/4] underlying write pointer advanced sequentially ==="
blkzone report "$UNDERLYING" | head -n 4

echo
echo "=== M1 CHECKS PASSED ==="
