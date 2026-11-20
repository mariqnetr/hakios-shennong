#!/bin/bash
# Make a HackiOS tree with the shennong (Xiaomi 14 Pro, SM8650) port: upstream at the base commit, with the
# files of port/ copied over it (port/ has the same layout as upstream; it holds the files the port adds or changes).
#   ./apply.sh [dir]        (default ./hackios)
set -e
BASE=d82c64a3a31cd27a0a0afbf6e213e467e44b1b4c
UP=https://github.com/oosexgang/hackios-xnu-kernel.git
HERE=$(cd "$(dirname "$0")" && pwd)
DIR=${1:-hackios}
[ -e "$DIR" ] && { echo "$DIR exists"; exit 1; }
git clone "$UP" "$DIR"
git -C "$DIR" checkout -q "$BASE"
cp -a "$HERE/port/." "$DIR/"
echo "done: $DIR is upstream $BASE with the shennong port's files over it"
