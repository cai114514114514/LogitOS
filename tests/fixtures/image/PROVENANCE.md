# Provenance: tests/fixtures/image

## Classification: C — project-generated

`anim.apng`, `anim.gif`, `icon.ico`, `rot.jpg`, `still.bmp`, `still.webp` are
all produced by `tests/unit/img_fixture_gen.py`, whose own header states:

> "Generate the committed fixtures that ride on the LogitFS disk image, so
> the same bytes are decoded on the host and inside the guest (make
> test-imgcheck). One file per format that this line added, small enough to
> commit ..."

Pixel content is a procedurally generated pattern (`pattern(w, h, seed)`,
a deterministic function of `x`/`y`/`seed`, no external image data), 40×28
pixels, built with the Python `PIL`/`Pillow` library. No third-party image
content. `rot.jpg`'s EXIF orientation tag is set by the same script for the
same synthetic pattern, not sourced from a photograph.

## Consuming gates

`Makefile`'s `$(IMG_FIXTURES)` variable and `$(DISK)` (packs these onto the
disk at fixed paths for `test-imgcheck`); `tests/unit/img_dump.c` and other
host image-decoder tests read them directly. Not touched by this pass —
class C, outside the class-A removal mandate.

## History

Generated fixtures of this shape first appear alongside the image-decoder
work; `tests/unit/img_fixture_gen.py` is the generator of record for the
current files (regenerable at any time — delete and re-run the script, no
capture step involved).
