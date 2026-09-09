#!/bin/sh
set -eu

cd "$(dirname "$0")"

TRACKED_DIR="/home/nybo/services/nanocvs-c"
TEST_NAME="nanocvs-smoke-$$.tmp"
TEST_FILE="$TRACKED_DIR/$TEST_NAME"
REL_TEST_FILE="nanocvs-c/$TEST_NAME"
RESTORE_OUT="/tmp/nanocvs-smoke-restore.txt"
LOG1="/tmp/nanocvs-smoke-log1.txt"
LOG2="/tmp/nanocvs-smoke-log2.txt"
EXPECT_OUT="/tmp/nanocvs-smoke-expected.txt"

cleanup() {
  rm -f "$TEST_FILE" "$RESTORE_OUT" "$LOG1" "$LOG2" "$EXPECT_OUT"
}
trap cleanup EXIT INT TERM

printf '[build]\n'
make >/dev/null

printf '[init]\n'
./nanocvs-c init >/dev/null

printf '[create test file]\n'
rm -f "$TEST_FILE"
printf 'alpha\n' > "$TEST_FILE"
./nanocvs-c scan >/dev/null
./nanocvs-c log --path "$REL_TEST_FILE" > "$LOG1"
grep -q 'create' "$LOG1"
CREATE_ID=$(sed -n 's/^\[\([0-9][0-9]*\)\].*/\1/p' "$LOG1" | head -n1)
[ -n "$CREATE_ID" ]

printf '[modify test file]\n'
printf 'alpha\nbeta\n' > "$TEST_FILE"
./nanocvs-c scan >/dev/null
./nanocvs-c log --path "$REL_TEST_FILE" > "$LOG2"
grep -q 'modify' "$LOG2"
MODIFY_ID=$(sed -n 's/^\[\([0-9][0-9]*\)\].*/\1/p' "$LOG2" | head -n1)
[ -n "$MODIFY_ID" ]

printf '[diff]\n'
./nanocvs-c diff --stat "$REL_TEST_FILE" "$MODIFY_ID" | grep -q '^diff '

printf '[cat]\n'
./nanocvs-c cat "$REL_TEST_FILE" "$MODIFY_ID" | grep -q '^alpha$'
./nanocvs-c cat "$REL_TEST_FILE" "$MODIFY_ID" | grep -q '^beta$'

printf '[restore]\n'
./nanocvs-c restore "$REL_TEST_FILE" "$CREATE_ID" --to "$RESTORE_OUT" >/dev/null
cmp -s "$RESTORE_OUT" "$TEST_FILE" && {
  echo 'restore verification failed: restored file should differ from current modified file' >&2
  exit 1
}
printf 'alpha\n' > "$EXPECT_OUT"
cmp -s "$RESTORE_OUT" "$EXPECT_OUT"

printf '[delete]\n'
rm -f "$TEST_FILE"
./nanocvs-c scan >/dev/null
./nanocvs-c log --path "$REL_TEST_FILE" | grep -q 'delete'

printf 'smoke ok\n'
