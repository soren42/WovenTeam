#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT_DIR"

TEST_DIR="build/smoke-phase0"
LOG_PATH="$TEST_DIR/phase0-room.jsonl"
rm -rf "$TEST_DIR"
mkdir -p "$TEST_DIR"

WT_ROOM_LOG_PATH="$LOG_PATH" ./build/wt-say ceo all "Claude, ChatGPT, Gemini: each confirm that you can read and respond through the shared room." >/dev/null
WT_ROOM_LOG_PATH="$LOG_PATH" ./build/wt-agent --agent claude --once >/dev/null
WT_ROOM_LOG_PATH="$LOG_PATH" ./build/wt-agent --agent chatgpt --once >/dev/null
WT_ROOM_LOG_PATH="$LOG_PATH" ./build/wt-agent --agent gemini --once >/dev/null

line_count=$(wc -l < "$LOG_PATH" | tr -d ' ')
test "$line_count" -eq 4

grep -q '"senderName":"ceo"' "$LOG_PATH"
grep -q '"senderName":"claude"' "$LOG_PATH"
grep -q '"senderName":"chatgpt"' "$LOG_PATH"
grep -q '"senderName":"gemini"' "$LOG_PATH"
grep -q 'messageId 1' "$LOG_PATH"

WT_ROOM_LOG_PATH="$LOG_PATH" ./build/wt-tail --limit 20 | grep -q '\[4\].*gemini -> ceo'

echo "phase0 smoke ok"
