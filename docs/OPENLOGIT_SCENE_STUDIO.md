# OpenLogit Scene Studio batch

This batch adds a real native graphics-tool consumer and two reusable SDK
modules. It extends the existing software pipeline; it does not claim a complete
DirectX/OpenGL replacement or all remaining graphics-plan acceptance.

Implementation is split into `c/lib/gfx/ui/`, `c/lib/gfx3d/scene/` and
`examples/openlogit/studio/`. Public additions preserve prior API descriptors.
UI code can link into the freestanding 2D core; scene generation, skinning and
LSL remain optional user-space archive members. The SDK installer recursively
preserves example subdirectories and installs `/bin/scene-studio`.

The consumer includes three inspector pages, object selection, sliders, toggles,
playback and seeking, camera controls, quality switching, help and reset notices.
Its scene includes parameterized geometry, two-bone skinning, blended poses,
vertex lighting/specular/toon bands, bilinear textures, fog and planar shadows.
Animations share SDK time sampling. UI-only motion reuses the cached 3D result;
idle and minimized states stop rendering. System reduced motion pauses playback.

Validation entry points:

```sh
make BUILD=/absolute/isolated/build test-openlogit-scene
make BUILD=/absolute/isolated/build test-openlogit-scene-os
```

The host gate uses independent geometry, ray/plane, skin/normal, pose and shader
numeric checks, plus pointer/focus and fixed-time UI behavior. Shader execution
disabled in the negative build must fail the same texture/fog oracle. ASan and
UBSan run on the positive path. Existing 3D depth/shared-edge, animation, LSL,
API 1.1 and consumer-route gates remain separate regression evidence.

The guest gate compiles three installed source files with guest TCC, then uses
normal mouse/keyboard input. Viewport screenshots must change under controls
and restore exactly when toggles are restored. Pause, modal cleanup, quality,
minimize/restore and system reduced motion have independent assertions. It saves
screenshots, serial output, checks, image/archive hashes and render timings.
Timings describe scene rendering only, not displayed FPS or a 2D p95 comparison.

Remaining scope includes GPU acceleration, complete GLSL-equivalent language
coverage, shadow maps, PBR, asset import/export, broad system UI redesign and the
original whole-system p95 performance budget. Existing Island gameplay remains
a separate consumer; this batch's tool is not a replacement game or editor.
