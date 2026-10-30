#!/usr/bin/env bash
# Shared by tests run as root. Retain logs and fail if collection is incomplete.
kernel_log_start() {
 command -v python3 >/dev/null || { echo '[FAIL] python3 is required for kernel log capture' >&2; return 1; }
 exec 9>/run/lock/dm-zns-tests.lock
 flock -n 9 || { echo '[FAIL] another instrumented test is running' >&2; return 1; }
 mkdir -p "$SCRIPT_DIR/../results/kernel-logs"
 TEST_LOG_DIR=${TEST_LOG_DIR:-$(mktemp -d "$SCRIPT_DIR/../results/kernel-logs/run.XXXXXX")}
 mkdir -p "$TEST_LOG_DIR"
 KERNEL_LOG="$TEST_LOG_DIR/kernel.log"
 KERNEL_READY="$TEST_LOG_DIR/ready.$$"
 KERNEL_END="dm-zns-test-end-$$-$(date +%s%N)"
 python3 "$SCRIPT_DIR/capture-kernel.py" "$KERNEL_LOG" "$KERNEL_READY" "$KERNEL_END" 2> "$TEST_LOG_DIR/collector-error.log" &
 KERNEL_PID=$!
 for _ in $(seq 1 100); do
  if [ -f "$KERNEL_READY" ]; then echo "[*] Kernel log: $KERNEL_LOG"; return 0; fi
  kill -0 "$KERNEL_PID" 2>/dev/null || break
  sleep 0.05
 done
 echo '[FAIL] kernel log collector did not start' >&2
 kernel_log_abort
 return 1
}
kernel_log_finish() {
 printf '<6>%s\n' "$KERNEL_END" > /dev/kmsg || return 1
 for _ in $(seq 1 100); do
  kill -0 "$KERNEL_PID" 2>/dev/null || break
  sleep 0.1
 done
 if kill -0 "$KERNEL_PID" 2>/dev/null; then
  echo '[FAIL] kernel log collector did not drain' >&2
  kernel_log_abort
  return 1
 fi
 local rc=0
 wait "$KERNEL_PID" || rc=$?
 KERNEL_PID=
 if [ "$rc" -ne 0 ] || [ -s "$TEST_LOG_DIR/collector-error.log" ]; then
  echo '[FAIL] kernel log capture failed; cannot validate test' >&2
  cat "$TEST_LOG_DIR/collector-error.log" >&2
  return 1
 fi
}
kernel_log_abort() {
 if [ -n "${KERNEL_PID:-}" ]; then
  kill "$KERNEL_PID" 2>/dev/null || true
  wait "$KERNEL_PID" 2>/dev/null || true
  KERNEL_PID=
 fi
}
