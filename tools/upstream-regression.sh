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
# Upstream 1.3.0's metadata.c reads Ogg comments through oggtags.c, honours the
# track-artist setting through config.c, and inflates zlib-compressed ID3 data
# through miniz's tinfl (the same implementation object png_scaled.c builds in
# the main tree; here it is a stub TU so the probe links standalone).
cat >"$WORK/miniz_tinfl.c" <<'EOF'
#include "miniz.h"
#include "miniz_tinfl.h"
#include "tinfl_impl.inc"
EOF
gcc "${FLAGS[@]}" -Isrc/system/image/miniz -c "$WORK/miniz_tinfl.c" -o "$WORK/miniz_tinfl.o"
gcc "${FLAGS[@]}" "$ROOT/tools/metadata-probe.c" \
    src/system/library/metadata.c src/system/library/oggtags.c src/system/library/cue.c \
    src/system/core/utils.c src/system/core/config.c \
    src/system/decode/{decode,mp4,wavpackdec,apedec,sndfile,stb_vorbis}.c \
    "$WORK/miniz_tinfl.o" \
    -Wl,--gc-sections "${LIBS[@]}" -lm -ldl -lpthread -o "$WORK/metadata-probe"
gcc "${FLAGS[@]}" "$ROOT/tools/kernel-log-test.c" src/system/core/logging.c \
    -Wl,--gc-sections -Wl,--wrap=open64,--wrap=lseek64,--wrap=read,--wrap=__read_chk \
    -lpthread -o "$WORK/kernel-log-test"
"$WORK/kernel-log-test"
python3 "$ROOT/tools/metadata-regression.py" "$WORK/metadata-probe"
