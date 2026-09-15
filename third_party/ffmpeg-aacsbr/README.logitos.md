# FFmpeg AAC SBR narrow import

This directory imports only the float HE-AAC v1 Spectral Band Replication core
from the official FFmpeg n4.4 tree at commit
`dc91b913b6260e85e1304c74ff7bb3c22a8c9fb1`.

## Verbatim upstream files

- `libavcodec/aacsbr.c` -> `aacsbr.c`
- `libavcodec/aacsbr.h` -> `aacsbr.h`
- `libavcodec/aacsbr_template.c` -> `aacsbr_template.c`
- `libavcodec/aacsbrdata.h` -> `aacsbrdata.h`
- `libavcodec/sbr.h` -> `sbr.h`
- `libavcodec/sbrdsp.c` -> `sbrdsp.c`
- `libavcodec/sbrdsp.h` -> `sbrdsp.h`
- `libavcodec/sbrdsp_template.c` -> `sbrdsp_template.c`

The source URL for any item is
`https://git.ffmpeg.org/gitweb/ffmpeg.git/blob_plain/dc91b913b6260e85e1304c74ff7bb3c22a8c9fb1:/libavcodec/<name>`.

All are unmodified except `aacsbr_template.c`, whose decode exit has one local
exact-bound bit-reader error propagation block marked with a LogitOS comment.
That change prevents a truncated network payload from silently becoming pure
upsampling. The imported files retain FFmpeg's copyright and
LGPL-2.1-or-later notices.

SHA-256 of the imported/local source set:

```text
32c9ae1ae7f3e5fc13ae5c1fa16ec18eb8119dd3b45cdbe59dbab0ca0f27dcc7  aacsbr.c
a00118a697e7eab41ca302b8b6679e61f1a2eed3fb6d5224e2ecca57511a4dc5  aacsbr_template.c (locally patched)
b284a3ea4453aa9043e27039698062ef06a34175202620f7cd804faf9e824bdc  aacsbrdata.h
bda690cbe019b8512b5675a688a8bd443aa501a77121e5d46125139580bf82ca  sbr.h
1772c76b44645c2ae5f7eb21a9c51af42a72623ba89ab09d9d87b61db765630c  sbrdsp.c
42840190401a918943572b0b2e288e5f4dfea12c7afc6927f35a6aa27a0f5a6d  sbrdsp.h
acc2ca4b1bbdc15a212222730b49db1171ec181d04ecc6689d2f3d86ee45047b  sbrdsp_template.c
```

## Project-authored adaptation surface

`aac.h`, `aac_defines.h`, `aacps.h`, `config.h`, `fft.h`, `get_bits.h`,
`internal.h`, and `libavutil/*.h` are small LogitOS compatibility headers, not
copies of FFmpeg headers. They provide bounded bit reads, direct canonical VLC
lookup, scalar float DSP, the private AAC/SBR structs used by this snapshot,
and adapters to `c/lib/audio/amath` and `afft`.

Parametric Stereo is intentionally stubbed at this private boundary and public
AOT 29 input remains unsupported. The consumer is
`c/lib/audio/aac_sbr.c`; the browser advertises only AOT 5 (`mp4a.40.5`).
