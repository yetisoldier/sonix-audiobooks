#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WORK="$ROOT/build/upstream-regression"
mkdir -p "$WORK"
cd "$ROOT/sonix-player"
FLAGS=(-O1 -g -DHOST_BUILD=1 -D_FILE_OFFSET_BITS=64 -DLV_CONF_INCLUDE_SIMPLE=1
       -I. -Ilvgl -ffunction-sections -fdata-sections -fno-omit-frame-pointer
       -fsanitize=address,undefined)
LIBS=($(pkg-config --cflags --libs opusfile wavpack))
gcc "${FLAGS[@]}" "$ROOT/tools/metadata-probe.c" \
    src/system/library/metadata.c src/system/library/cue.c src/system/core/utils.c \
    src/system/decode/{decode,mp4,wavpackdec,apedec,sndfile,stb_vorbis}.c \
    -Wl,--gc-sections "${LIBS[@]}" -lm -ldl -lpthread -o "$WORK/metadata-probe"
gcc "${FLAGS[@]}" "$ROOT/tools/kernel-log-test.c" src/system/core/logging.c \
    -Wl,--gc-sections -Wl,--wrap=open64,--wrap=lseek64,--wrap=read,--wrap=__read_chk \
    -lpthread -o "$WORK/kernel-log-test"
"$WORK/kernel-log-test"
python3 "$ROOT/tools/metadata-regression.py" "$WORK/metadata-probe"
