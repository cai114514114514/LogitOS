# Scene Studio

Run `/bin/scene-studio` inside LogitOS. To rebuild the installed example:

```sh
/bin/tcc /usr/share/openlogit/studio/main.c /usr/share/openlogit/studio/scene.c /usr/share/openlogit/studio/ui.c -I/usr/include/openlogit -lopenlogit -o /tmp/scene-studio
/tmp/scene-studio
```

The source is split by responsibility: `main.c` owns the window/event loop,
`scene.c` owns scene resources and rendering, `ui.c` owns inspector state and
layout, and `shaders.h` contains editable LSL vertex/fragment source.

The three inspector pages contain different controls. Scene exposes picking,
mesh counts, projected shadows and light position. Motion exposes pose mixing
and explains the two-node hierarchy. Material exposes the repeated bilinear
texture, toon bands and specular gain. The timeline remains accessible on all
pages. Drag it to seek; Space pauses/resumes. Right-drag the viewport to orbit,
or use the camera buttons. Click a mesh or its object-list entry to select it.
Arrow keys adjust a focused slider by 5%; otherwise they orbit the camera.
Tab/Enter focus and activate buttons. S/T/C toggle shadow/texture/toon, Q changes
quality, R resets camera/material/pose state, H opens help, Esc closes help or
pauses playback. Closing the window exits the app.

The reusable APIs are `openlogit_ui.h` and `openlogit_scene.h`:

- UI: stable control IDs, pointer capture, keyboard focus, scalar feedback,
  spring hover/press/toggle/slider geometry and progress drawing. Define slots,
  call `ol_ui_begin` once with a monotonic frame time, route events, sample all
  visible controls, then render. The caller owns labels, layout and values.
- Scene: parameterized sphere, torus, open tube and plane; orbit matrices and
  world rays; triangle picking; planar light projection; pose interpolation;
  four-influence mesh skinning with inverse-transpose normals. Generated meshes
  own their allocations: destroy a previous mesh before reusing its descriptor.
  Bone matrices supplied to skinning must already include inverse bind transforms
  when imported rigs need them. This sample's bind transforms are identity.

No private rasterizer or second animation integrator lives in the example.
Scene motion, modal entrance/exit, the tab indicator, notification and control
feedback use OpenLogit timelines/springs. Each visible iteration uses one caller
time. Unchanged 3D results are reused during UI-only animation. Completed motion
stops rendering; minimized windows do not render; reduced motion pauses scene
playback and places transient UI at its endpoint. Quality changes release old
command-list resource references before replacing the target.

The software viewport defaults to 320×224 with a 160×112 mode, displayed in a
520×364 region. The UI is drawn at the window's 960×600 logical layout. Lighting
is Gouraud diffuse plus Blinn-style specular at vertices, with optional toon
bands. Fragment shading modulates the texture and applies distance fog. Shadows
are opaque planar projections onto the ground, not shadow maps. GPU rendering,
PBR, per-pixel normal maps, scene import/export and general-purpose model editing
are outside this sample's current scope. Render timings exclude UI composition;
they must not be presented as display FPS or a whole-system performance result.
