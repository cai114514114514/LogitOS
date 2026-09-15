#!/usr/bin/env python3
"""Encode the Vorbis differential corpus through libsndfile/libvorbis.

FFmpeg's native experimental Vorbis encoder in 8.1 emits stereo streams whose
second channel its own decoder reconstructs differently from libvorbis.  Such
self-agreement cannot be an independent decoder oracle.  This helper is the
fallback producer when FFmpeg lacks libvorbis: libsndfile writes the stream,
then vorbis_gen.sh still asks FFmpeg to produce the reference PCM.
"""

import sys

import numpy as np
import soundfile as sf


def main() -> int:
    if len(sys.argv) != 5:
        raise SystemExit("usage: vorbis_encode.py <in.wav> <out.ogg> <quality> <channels>")
    src, dst = sys.argv[1], sys.argv[2]
    quality = max(0.0, min(1.0, float(sys.argv[3])))
    channels = int(sys.argv[4])
    pcm, rate = sf.read(src, dtype="float32", always_2d=True)
    if channels == 0:
        channels = pcm.shape[1]
    if channels == 1 and pcm.shape[1] != 1:
        pcm = pcm.mean(axis=1, keepdims=True)
    elif channels == 2 and pcm.shape[1] == 1:
        pcm = np.repeat(pcm, 2, axis=1)
    elif pcm.shape[1] != channels:
        raise SystemExit(f"cannot convert {pcm.shape[1]} channels to {channels}")
    sf.write(dst, pcm, rate, format="OGG", subtype="VORBIS",
             compression_level=quality)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
