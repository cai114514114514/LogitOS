#!/bin/bash
# Build + run the gif.c leak/ASan harness against real and malformed GIFs.
set -u
cd /mnt/d/ststem
mkdir -p build/giftest

# real GIFs via ffmpeg: normal, interlaced, transparent-ish palette, animated(multi-frame)
ffmpeg -v error -y -f lavfi -i testsrc2=size=64x48:duration=1:rate=5 -vf format=rgb24 build/giftest/normal.gif
ffmpeg -v error -y -f lavfi -i mandelbrot=size=80x60:rate=3 -t 1 -vf format=rgb24 build/giftest/mandel.gif
ffmpeg -v error -y -f lavfi -i testsrc2=size=37x23:duration=1:rate=2 -vf format=rgb24 build/giftest/odd.gif

# malformed: garbage with GIF magic, and a header-only stub
printf 'GIF89aGARBAGEGARBAGEGARBAGE' > build/giftest/garbage.gif
head -c 13 build/giftest/normal.gif > build/giftest/stub.gif

gcc -std=c99 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Dstatic= -Dgif_register=gif_register_unused -Ic/lib/image \
    tests/unit/gif_leak_test.c c/lib/image/gif.c -o build/giftest/gif_leak_test
rc=$?
if [ $rc -ne 0 ]; then echo "BUILD FAILED"; exit 1; fi

./build/giftest/gif_leak_test build/giftest/*.gif
echo "HARNESS_RC=$?"
