# Private spacing-parser control against release-v2 source

This diagnostic browser isolates the new typed gap/logical-padding parser from
the other browser changes. It is not a delivery candidate or a full rollback.
`build-ai-sites-0913/source-v1` already contains the current button metrics
overlay. A COW copy to `source-spacing-parser-before` adds only
`#define CSS_SPACING_MATH_LEGACY 1` at the top of its private `css_extra.c`.
The existing negative-control branches retain the former px-only gap and
logical-padding parsing and skip typed capture/application.

All 6,685 source files were hashed before and after copying. Only
`c/apps/browser/css_extra.c` differs. In particular, `css.h` keeps the same
layout and fields, and `layout.c` keeps both the corrected containing-width
padding resolver and current button automatic metrics. Neither
`LAYOUT_PADDING_OWN_WIDTH_LEGACY` nor `LAYOUT_BUTTON_AUTO_METRICS_LEGACY` is enabled.

The corresponding COW `native-spacing-parser-before` build succeeds. All 591
actual linker inputs remain in the same order; only
`browserobj/c/apps/browser/css_extra.o` differs in content. The kernel and ISO
are identical to release-v2, and both baseline source and browser remain
unchanged. Artifact hashes:

- Baseline browser: `b29a60312a9ffb442992f58c185a1977377c7439e8e7a6953bac2e138c963860`
- Control browser: `edd61e0450ee4c0a3501da7b6e8ffdb05a9e9c37334ab2acdd4707472a5f52aa`
- Layout retained: `d70c2ba9ee62b4de028a4225099c2ae8d359c1d0721348143cac784d494a7c8d`
- cstyle header retained: `8ac39c4bd21a1ebccbb06b419783d5cb143e28e0e14ba3c9b119ef654631dded`

Three ordinary local qualification cases pass 10/10 in both builds with their
respective expectations: literal gap/inline padding/block padding remain
12/12/6 px; calculated versions change from 12/12/6 to 0/0/0 px; the physical
padding button stays 86×34. The fixture contains no site markup, script,
resources or requests. No broad suite, VM or real site was run by this subtask.

Evidence is retained under `build-ai-sites-0913/native-spacing-parser-before/`:
`control-inputs.json` (complete source hashes), `control-build-evidence.json`
(actual ordered linker inputs and artifact hashes), `build.log`, and
`qualification.json` with its paired logs. The root task owns packaging and
the real-site comparison; compiling this control does not identify the cause
of the observed long mounting delay.
