# Image decoding

LogitOS provides a shared image library that converts encoded bytes into straight RGBA8 pixels. Its public API and dispatcher live in `c/lib/image/img.h` and `c/lib/image/img.c`. JPEG, GIF, SVG and EXIF handling are written in C; PNG, BMP, ICO/CUR and WebP decoding are implemented under `rust/src/`. Browser and Preview link the decoders into their processes, while kernel consumers use a serialized decoding wrapper.

## Supported formats

`img_init()` registers PNG, GIF, JPEG, SVG, BMP, ICO and WebP handlers in that order. Each handler identifies input from its contents. Dispatch calls the first handler whose detector accepts the bytes.

| Format | Implemented features |
|---|---|
| PNG | Grayscale at 1/2/4/8/16 bits, indexed color at 1/2/4/8 bits, and RGB, grayscale with alpha and RGBA at 8/16 bits. Handles all five row filters, Adam7 interlacing and `tRNS` transparency. |
| APNG | Frame rectangles, delays, play counts, source and over blending, and keep, background and previous disposal. |
| GIF | Global and local palettes, LZW compression, interlacing, transparency, frame rectangles, delays, loop metadata and disposal modes. |
| JPEG | Baseline and progressive Huffman coding, 8-bit grayscale and YCbCr samples, component sampling factors, quantization tables and restart intervals. |
| BMP | Core and later DIB headers, 1/4/8/16/24/32-bit pixels, palettes, channel masks, RLE4, RLE8, and rows stored from the top or bottom. |
| ICO/CUR | Embedded PNG or icon DIB images. Entry selection uses the largest directory area, with bit depth as the tie breaker. |
| WebP | VP8L lossless images and VP8 lossy key frames, including a lossy image’s `ALPH` transparency plane. |
| SVG | Paths and basic shapes, solid fills and strokes, inherited paint, affine transforms, local references, clipping, group opacity and embedded stylesheets. |

PNG and APNG share reconstruction code in `rust/src/png.rs`. Icon decoding in `rust/src/ico.rs` delegates to the PNG decoder or the DIB implementation in `rust/src/bmp.rs`.

BMP alpha handling follows the container. A basic 32-bit BMP produces opaque output from its reserved fourth byte; a declared alpha mask supplies transparency. A 32-bit icon DIB uses the fourth byte as alpha. When all those alpha values are zero, the decoder makes the color pixels opaque and applies the trailing AND mask.

JPEG uses two coefficient storage strategies in `c/lib/image/jpeg.c`. Baseline decoding transforms each block as it arrives. Progressive decoding retains coefficient planes while later scans refine them, then reconstructs the sample planes. This confines the larger coefficient allocation to progressive input. Both paths use integer transforms and nearest neighbor chroma upsampling.

## API and ownership

`c/lib/image/img.h` defines the still and animation interfaces:

| Interface | Contract |
|---|---|
| `struct image` | Width, height and a pointer to `w * h * 4` bytes, ordered red, green, blue and alpha. Color channels use straight alpha. |
| `img_decode()` | Synchronously decodes borrowed input bytes. Returns zero on success and `-1` for unsupported input or decoding failure. |
| `img_free()` | Releases the pixel buffer and clears the image fields. |
| `struct img_frame` | A complete RGBA canvas and its display delay in milliseconds. |
| `struct img_anim` | Canvas dimensions, frame count, loop count and an array of frames. Zero loops means continuous playback; a positive value means that many plays. |
| `img_decode_anim()` | Decodes animation, or wraps a still image in one frame with delay zero and loop count one. |
| `img_anim_free()` | Releases the frame pixels and frame array, then clears dimensions and frame count. |

Keep the encoded buffer alive until decoding returns. After success, the caller owns the returned allocations and releases them with the matching image or animation function.

C and Rust decoders allocate through `kmalloc()` and release through `kfree()`. Rust’s owning buffers in `rust/src/imgbuf.rs` release scratch allocations when a function returns early and transfer completed output to C. Browser and Preview map these allocator names to their application allocator in `c/apps/gui/browser/app/runtime.c` and `c/apps/gui/preview/preview.c`.

The registry in `c/lib/image/img.c` holds eight entries. Each entry contains a detector, a still decoder and an optional animation callback. `img_register()` installs a still handler; `img_register_anim()` also supplies animation decoding.

## Animation composition

Each returned frame contains the complete display canvas. The decoder applies offsets, transparency, blending and disposal before retaining the frame. Players can display every frame through the same bitmap operation, and tests can compare the composited pixels directly.

In `c/lib/image/gif.c`, transparent palette indices preserve the canvas underneath. Disposal mode two clears the frame rectangle to transparent pixels. Mode three restores a snapshot taken before drawing that frame. The still entry point returns the first composited frame at the logical screen dimensions.

APNG uses the same output model in `rust/src/png.rs`. Source blending replaces the frame rectangle, including its alpha values; over blending combines the frame with the canvas. Previous disposal restores a saved canvas, with previous disposal on the first frame treated as background disposal.

For PNG input, `img_decode()` reads the default image from `IDAT` chunks. The animation path follows frame controls and their compressed data, allowing the default image and animation to have different roles.

`tests/unit/img_anim_test.c` checks canvas dimensions, frame counts, delays, loop counts and the bytes of each composited frame. These assertions expose disposal errors that a frame count would miss.

## EXIF orientation

`c/lib/image/exif.c` locates orientation metadata in JPEG APP1 segments, PNG `eXIf` chunks, WebP `EXIF` chunks and bare TIFF data. It reads either TIFF byte order and examines the first image file directory for orientation tag `0x0112`. Missing or unusable metadata yields orientation one.

The dispatcher applies orientation after successful decoding. `exif_apply()` implements the eight rotation and reflection mappings using a replacement pixel buffer. Orientations five through eight exchange width and height. Allocation failure propagates as a decoding error.

For animation, the dispatcher transforms every returned frame. Still formats reached through `img_decode_anim()` pass through `img_decode()`, so orientation is applied once. Centralizing this step gives the public entry points the same orientation behavior across codecs.

`tests/unit/img_exif_test.c` checks recovered tag values, output dimensions, pixel mappings for lossless carriers, and agreement between the still and animation interfaces.

## SVG geometry and scenes

`c/lib/image/svg.c` searches the first 1024 input bytes for an SVG root. Its prefix check accepts whitespace, a UTF-8 byte order mark, processing instructions, comments and declarations. Root dimensions accept bare numbers or pixel units. Missing dimensions take the corresponding `viewBox` dimension when available, then fall back to 300 by 150 pixels.

Path geometry includes absolute and relative move, line, horizontal, vertical, cubic, quadratic, smooth curve, arc and close commands. Basic shapes include rectangles, circles, ellipses, lines, polygons and polylines. Parsing and geometry use fixed point arithmetic; OpenLogit supplies shared path rendering for antialiased fills and strokes.

`c/lib/image/svg_scene.inc` builds a tree before rendering. This permits a local `use` reference to resolve an element declared later and gives clipping and opacity a subtree to operate on. References accept `href` and `xlink:href`; an active-node marker stops reference cycles.

Transforms include matrix, translation, scaling, rotation and skew operations. Nested SVG and referenced symbol viewports apply `viewBox` mapping and `preserveAspectRatio`, with viewport clipping by default. Local clip paths support user coordinates and object bounding box coordinates.

Group opacity renders children into a temporary canvas and composites that canvas once. This preserves the opacity of overlapping shapes within the group. Clip paths render masks that can also be combined with viewport masks.

## SVG stylesheets and browser integration

Standalone SVG uses the stylesheet compiler in `c/lib/image/svg_stylesheet.inc`. It collects embedded style elements before drawing, allowing a later stylesheet to affect earlier shapes.

Supported selectors include type names, the universal selector, IDs, classes, compounds such as `path.icon#mark`, and comma lists. The cascade combines presentation attributes, stylesheet rules and inline declarations using importance, specificity and source order. Declaration values are validated before they can replace an earlier value.

The supported paint properties are `color`, `fill`, `stroke`, `fill-opacity`, `stroke-opacity`, `opacity`, `fill-rule`, `clip-rule`, `stroke-width`, `display` and `visibility`. The scene applies inheritance and the `inherit`, `initial` and `unset` keywords. `currentColor` resolves against the element’s color.

Browser inline SVG reaches the decoder through `c/apps/gui/browser/paint/svg_dom_paint.inc`, included by `c/apps/gui/browser/layout/block.c`. During layout, the browser serializes the live DOM subtree with paint values selected by its CSS machinery and the requested raster dimensions. This serialization lets DOM changes reach rendering and makes bitmap caching sensitive to the resulting SVG input. The outer browser painter applies root opacity; inner groups receive their opacity in the serialized input.

`img_css_color()` in `c/lib/image/svg.c` exposes the color literal parser. It returns one and writes RGBA when parsing succeeds, or zero while preserving the caller’s output bytes. Canvas uses it in `c/apps/gui/browser/js/graphics/canvas.c`.

## Resource limits and error handling

Bounds are enforced at several stages:

| Resource | Bound and implementation |
|---|---|
| PNG, BMP, icon DIB and VP8L dimensions | At most 8192 pixels per dimension. Shared RGBA sizing also checks 64 × 1024 × 1024 pixels in `rust/src/imgbuf.rs`. |
| VP8 key frame dimensions | At most 16383 pixels per dimension and 64 × 1024 × 1024 pixels, checked in `rust/src/vp8.rs`. |
| JPEG and GIF dimensions | At most 8192 pixels per dimension, checked in `c/lib/image/jpeg.c` and `c/lib/image/gif.c`. |
| Progressive JPEG coefficients | 64 MiB across component coefficient buffers. |
| GIF animation | Frame capacity starts at 1024 and is reduced using a 192 MiB retained-canvas budget. |
| APNG animation | Frame capacity uses the declared count, a maximum of 512 frames and a 192 MiB retained-canvas budget. |
| SVG source and output | Source up to 4 MiB; output dimensions clamped to 1–2048 pixels, with an area check of 4 × 1024 × 1024 pixels. |
| SVG scene | 2048 nodes, depth bound 48, 8192 rendering visits and 512 compiled stylesheet rules. |
| SVG temporary rendering storage | 64 MiB of live temporary layers. |
| SVG rendering work | An 8 MiB geometry-attribute traversal budget and a default budget of 256 × 1024 × 1024 canvas-pixel passes. |

Animation memory includes retained canvases, working canvases, compressed data and decoder scratch. EXIF transformation temporarily holds the original and replacement pixels. SVG work accounting charges surface clears and composites as well as shape rendering, including empty groups with opacity.

JPEG releases component planes, progressive coefficients and output storage through its cleanup paths. Rust owning buffers release scratch and unpublished frames on early returns. SVG releases scene nodes, stylesheet rules, masks, layers and output storage when allocation or resource accounting fails.

## Consumers and execution context

Preview calls `img_decode_anim()` in `c/apps/gui/preview/preview.c`. It displays the returned canvases using frame delays and loop count. Browser layout calls `img_decode()` for image resources and serialized inline SVG. Canvas can reuse a layout bitmap or decode image bytes from a data URL.

MJPEG decoding in `c/lib/video/mjpeg.c` passes each JPEG frame through `img_decode()`. Its wrapper supplies default Huffman tables when a frame omits them, allowing it to reuse the image library’s JPEG reconstruction.

Wallpaper and application-icon loaders in `c/kernel/gui/wm_wallpaper.c` and `c/kernel/gui/app_icons.c` call `kernel_img_decode()`. Defined in `c/kernel/exec/syscall.c`, this wrapper holds a decoding mutex and the graphics lock around `img_decode()`.

`SYS_IMG_DECODE` reads a file through the VFS, decodes it, checks the caller’s destination capacity, copies RGBA pixels to that buffer and returns dimensions through `struct logit_imgreq`. The request structure is defined in `include/abi/logit_abi.h`.

`Makefile` links the SVG implementation into Browser and Preview alongside the C decoders and Rust image library.

## Limitations

- JPEG rejects extended sequential, arithmetic, lossless, 12-bit and four-component input. Its baseline path requires a scan containing all components. See `c/lib/image/jpeg.c`.
- WebP provides still decoding. Animated WebP frame chunks have no animation decoding path. A failed lossy alpha-plane decode leaves the color image opaque. See `rust/src/webp.rs`.
- SVG omits text rendering, scripts, external resources, gradients and stroke dash patterns. Unsupported selectors invalidate their whole stylesheet rule; at-rules are skipped. Browser serialization omits style elements and inline declaration text because its CSS machinery has already selected paint values.
- The kernel build excludes `c/lib/image/svg.c`. Its weak `svg_register()` definition registers no handler, so kernel image decoding rejects SVG.
- EXIF handling extracts orientation. Bare TIFF metadata recognition supplies no TIFF pixel decoder. PNG skips chunk CRC verification; zlib decompression checks Adler-32.
- Successful decoding can yield partial content: BMP preserves available rows and clears the remainder, GIF pads short index data, and GIF or APNG can return an accumulated frame prefix. Unusable APNG animation can fall back to the default PNG image.
- GIF and APNG retain capacity for at least one frame, even when that canvas exceeds the 192 MiB retained-frame budget. Scratch allocations add to that budget.
- JPEG, GIF and SVG use shared scratch state within a process. Direct library callers must serialize decoding; kernel callers use `kernel_img_decode()`.

## Source map

| File | What it contains |
|---|---|
| `c/lib/image/img.h` | Public image, animation, registration and EXIF interfaces. |
| `c/lib/image/img.c` | Registry, dispatch, orientation integration and release functions. |
| `c/lib/image/jpeg.c` | Baseline and progressive JPEG reconstruction and cleanup. |
| `c/lib/image/gif.c` | GIF parsing, LZW, composition, timing and disposal. |
| `c/lib/image/exif.c` | Container metadata scanning and orientation transforms. |
| `c/lib/image/svg.c` | SVG detection, numeric and color parsing, geometry and output sizing. |
| `c/lib/image/svg_scene.inc` | Scene tree, references, transforms, clipping, opacity and resource accounting. |
| `c/lib/image/svg_stylesheet.inc` | Embedded stylesheet parsing and paint cascade. |
| `rust/src/imgbuf.rs` | C-compatible types, owning buffers and shared size checks. |
| `rust/src/png.rs` | PNG reconstruction and APNG composition. |
| `rust/src/bmp.rs` | BMP and icon DIB decoding. |
| `rust/src/ico.rs` | ICO/CUR entry selection and embedded-image dispatch. |
| `rust/src/webp.rs` | WebP container, VP8L and alpha-plane decoding. |
| `rust/src/vp8.rs` | VP8 header parsing and dimension checks. |
| `rust/src/vp8_frame.rs` | VP8 key frame reconstruction driver. |
| `rust/src/inflate.rs` | DEFLATE and zlib decompression with Adler-32 verification. |
| `c/apps/gui/preview/preview.c` | Allocator adapters and image playback. |
| `c/apps/gui/browser/app/runtime.c` | Browser allocator adapters. |
| `c/apps/gui/browser/layout/block.c` | Browser image decoding and caching. |
| `c/apps/gui/browser/paint/svg_dom_paint.inc` | Live SVG serialization with browser-selected paint values. |
| `c/apps/gui/browser/js/graphics/canvas.c` | Canvas image input and color-parser use. |
| `c/lib/video/mjpeg.c` | MJPEG framing and JPEG-backed decoding. |
| `c/kernel/exec/syscall.c` | Serialized kernel decoding and image syscall handling. |
| `c/kernel/gui/wm_wallpaper.c` | Wallpaper image loading. |
| `c/kernel/gui/app_icons.c` | Application-icon decoding and caching. |
| `include/abi/logit_abi.h` | Image syscall number and request structure. |
| `Makefile` | Kernel and application image-library membership. |
| `tests/unit/img_anim_test.c` | Animation canvas, delay and loop assertions. |
| `tests/unit/img_exif_test.c` | Orientation and still/animation agreement checks. |
| `tests/unit/svg_scene_test.c` | Scene rendering and allocation-failure assertions. |
| `tests/unit/svg_scene_budget_test.c` | SVG input and rendering-budget checks. |
| `tests/unit/svg_stylesheet_test.c` | Selector, cascade, inheritance and pixel comparisons. |

## See also

- [Libraries](overview.md)
- [Layout: block, flex, grid and text](../browser/layout.md)
- [Painting and hit testing](../browser/paint.md)
- [Desktop applications](../userland/desktop-apps.md)
- [System calls](../architecture/system-calls.md)