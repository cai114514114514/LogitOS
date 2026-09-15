# Vector Studio

Run `/bin/vector-studio`, or compile the installed sources inside LogitOS:

```sh
/bin/tcc /usr/share/openlogit/vector/main.c /usr/share/openlogit/vector/artwork.c /usr/share/openlogit/vector/ui.c -I/usr/include/openlogit -lopenlogit -o /tmp/vector-studio
/tmp/vector-studio
```

The native 900x560 client has three pages. Keys 1/2/3 select vectors, atlas
sprites and retained layers. Space starts/pauses the shared SDK timeline; C
changes coverage clipping, D toggles the dashed stroke, S selects another atlas
region, and R resets state. Tab/Enter and slider arrows operate the controls.
On the layers page drag the card or use unfocused arrow keys to move it. Esc
closes the application. Reduced motion pauses playback; minimized windows do
not redraw. Idle UI has no continuous animation rendering.

The amount slider controls stroke width, nine-slice panel width or cached-card
opacity on the respective page. Vector geometry uses gradients, cubic paths,
round strokes and a saved affine transform. Atlas regions rotate independently;
clamped premultiplied bilinear filtering avoids adjacent-tile color bleeding
and transparent seams. The layers page reuses background and card surfaces.

## SDK contracts exercised here

`ol_canvas` is a caller-owned state stack with 16 save levels. Geometry follows
its affine transform; paints, rectangular clips and mask origins are in device
coordinates. Coverage masks are A8. Fill/stroke commands copy geometry and
retain versioned image resources. Invalid composite recording poisons the list,
so a partial nine-slice or unbalanced restore cannot publish a partial frame.
The context does not own the command list or stroke scratch storage.

Sprite source rectangles are integer texels; transforms map crop-local 24.8
coordinates into device coordinates using the existing 16.16 matrix convention.
`ol_cmd_image_region` gives exact integer destination coverage. Nine-slice
borders remain fixed in destination pixels and require a nonempty source center.
New clamped filtering is explicit; legacy image filtering keeps its old results.

`ol_scene2d` keeps up to 32 ordered placements. A surface stays alive while
attached and until the last recorded list releases it. Scene rendering detects
movement, hiding, opacity changes and source generations, clears the union of
old/new bounds, then recomposes every overlapping layer. Previous placements
advance only after successful submission. Cached effects must fit inside the
supplied layer bounds; the scene does not infer invisible shadow extents.

Window transport packs only damaged RGBA rows into caller-owned scratch, then
requests rectangular presentation. Scratch must not overlap source pixels.
Native font labels are clipped to this same region: drawing unchanged AA text
again outside cleared damage would accumulate alpha. Empty SDK damage is a
no-op, even though the underlying zero-rectangle syscall means full damage.

`main.c` owns resources, input and frame submission; `artwork.c` generates the
vector/atlas content; `ui.c` owns controls and labels. No 3D renderer, shader VM,
independent rasterizer or second animation engine is linked by this example.
The fixed layout is an SDK workbench, not a responsive document editor.
