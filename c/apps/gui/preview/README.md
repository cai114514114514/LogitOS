# Preview

`fsroot/icons/preview.png` is the original generated RGBA app icon. The disk
packages it at `/icons/preview.png`; Dock, Finder and the library header share
the generic app-icon loader. Generation details are in `fsroot/icons/preview.md`.

The native media viewer uses the existing OpenLogit drawing transport. Its
library has a category sidebar, header-sniffed format/size labels, keyboard
selection, paging and wheel navigation. The viewer separates toolbar, stage
and footer, with a visible return action and Fit / 100% image controls.
`0` selects Fit and `1` selects actual size. Large actual-size images are
center-cropped to the stage; panning is not implemented.

`preview.c` owns file loading, decoding and media clocks. `view.inc` owns the
theme, window geometry, toolbar, image placement and wrapped messages.
`picker.inc` owns the local collection. `audio_view.inc` paints playback time
and the actual PCM level meters. Decoder refusals remain explicit. Progress
bars are display-only because the decoder interface does not provide seek.

Opening a file from the library nests a viewer inside the same application:
Escape, Backspace or the Library button returns to the list. Opening a file
directly retains the existing Escape-to-close behavior. Resize and theme
events repaint held stills, completion screens and error messages.

The library still scans `/media` and one level of subdirectories, with the
existing 48-entry ceiling. Containers form a combined category: a container
header alone does not establish whether it contains audio or video. Codec
support and the audio/video clock policy are unchanged.

```sh
make BUILD=build-preview build-preview/logit.iso build-preview/disk.img
make BUILD=build-preview test-preview
make BUILD=build-preview test-preview-nav
make BUILD=build-preview test-mk-wired
```

The pixel gate compares guest pixels with decoded fixtures and checks the
category and zoom controls. It obtains visible row geometry from the guest,
so a scrolled list cannot silently redirect a click to another file. The
missing-delay control is a prerequisite of the positive gate and requires a
measured early animation loop; a startup or input failure is not a successful
negative control.
