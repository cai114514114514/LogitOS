Serve this directory through the ordinary local fixture HTTP server and open
`index.html` in the real guest. Use the actual screenshot/font positions to
choose the blank between `json` and `next`, rather than borrowing host font
coordinates. Click that blank and require the target URL plus the painted text
`INLINE SPACE NAVIGATION REACHED`; return and repeat for the plain nested span.
On the wrapped line, verify its unused right end does not navigate. The red
non-link overlay must block navigation, and only the visible part of the
clipped line can be hit. Save screenshots before/after and serial navigation
records. Glyph clicks are the control for mouse delivery.

This fixture does not claim pointer-events support (there is no complete
computed producer yet), transformed-hit geometry, or full browser readiness.
