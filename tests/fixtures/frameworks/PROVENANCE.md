# Provenance: tests/fixtures/frameworks

## Classification: B + C (mixed), not A — this directory was NOT moved aside for the class-A skip proof

Unlike `tests/fixtures/cssweb`/`webapi`/`jsperf`/`cssperf`/`browser`, nothing
here is a verbatim capture of a live site's own content. Every fixture is a
minimal application **built here**, by this project, using each framework's
own public toolchain (`create-vite`, `create-next-app`, `@angular/cli`,
webpack's documented minimal setup) — see
`tests/fixtures/frameworks/build_apps.sh`. The output mixes:

- **Class C**: the application source under `_src/<name>/` (a component with
  state, a click handler, a lazily-imported route) — written by this project,
  a few files per framework, listed in the README's "WHAT IS IN EACH
  FIXTURE" section.
- **Class B**: the framework RUNTIME code the toolchain emits alongside that
  source into `<name>/index.html` and `<name>/s*.js` — React, Vue, Svelte,
  Angular, Next.js, Vite's client runtime, webpack's runtime, and (bundled
  into `next/s006.js`) core-js. All eight are MIT-licensed by their
  respective publishers (verified below, not recalled).

Because every one of these permits exactly this kind of redistribution
(“distribute ... copies of the Software”, MIT's own grant), none of this
directory needed the class-A removal/skip treatment, and `tests/frameworks.mk`
(`test-frameworks`, `probe-frameworks`) was left untouched by this pass.

## What travels with the bundles today, and what did not

The built bundles are production-minified (Vite/webpack/Next's default
production build), and **minification strips licence-comment banners by
default** — confirmed by inspection: none of `react/s*.js`, `vue/s*.js`,
`svelte/*.js`, `angular/*.js`, `webpack/s*.js` contain the strings `MIT`,
`Copyright` or `license` (`grep -c` across all bundle files found matches in
exactly two: `next/s006.js` and `next/s007.js`, both from the one library
that bundles its own runtime notice — core-js — which self-identifies at
runtime with `copyright:"© 2014-2024 Denis Pushkarev (zloirock.ru)"` baked
into its exported metadata object, not a comment).

**Fixed in this pass**: the actual upstream licence text for every framework
whose runtime is bundled into this corpus, fetched from each project's own
repository (not recalled) and reproduced below so the permission each one
grants is on record beside the bundle it covers, the same way `LICENSES/
MIT.txt` documents this project's own outer licence.

### React (`react/`) — `next/` also bundles the React runtime

Source: `https://raw.githubusercontent.com/facebook/react/main/LICENSE`

```
MIT License

Copyright (c) Meta Platforms, Inc. and affiliates.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### Vue (`vue/`)

Source: `https://raw.githubusercontent.com/vuejs/core/main/LICENSE`

```
The MIT License (MIT)

Copyright (c) 2018-present, Yuxi (Evan) You and Vue contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

### Svelte (`svelte/`)

Source: `https://raw.githubusercontent.com/sveltejs/svelte/main/LICENSE.md`

```
Copyright (c) 2016-2025 Svelte Contributors

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

### Angular (`angular/`)

Source: `https://raw.githubusercontent.com/angular/angular/main/LICENSE`

```
The MIT License

Copyright (c) 2010-2026 Google LLC. https://angular.dev/license

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

### webpack (`webpack/`)

Source: `https://raw.githubusercontent.com/webpack/webpack/main/LICENSE`

```
Copyright JS Foundation and other contributors

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
'Software'), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED 'AS IS', WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

### Vite (`vite/`, and the dev/build tooling behind `react/`, `vue/`, `svelte/`)

Source: `https://raw.githubusercontent.com/vitejs/vite/main/LICENSE`

```
MIT License

Copyright (c) 2019-present, VoidZero Inc. and Vite contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### Next.js (`next/`)

Source: `https://raw.githubusercontent.com/vercel/next.js/canary/license.md`

```
The MIT License (MIT)

Copyright (c) 2025 Vercel, Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### core-js (bundled inside `next/s006.js`)

Source: `https://raw.githubusercontent.com/zloirock/core-js/master/LICENSE`.
`next/s006.js` itself already carries a runtime copyright string —
`copyright:"© 2014-2024 Denis Pushkarev (zloirock.ru)"`,
`license:"https://github.com/zloirock/core-js/blob/v3.38.1/LICENSE"` — this
is the licence that URL points to:

```
Copyright (c) 2013–2025 Denis Pushkarev (zloirock.ru)
Copyright (c) 2025–2026 CoreJS Company (core-js.io)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## Consuming gates (unchanged — not class A, no skip work done)

`test-frameworks` (`tests/frameworks.mk`) already has its own graceful
degradation, independent of this audit: `tests/unit/framework_rank.py`'s
`run_probe()` prints `"framework_rank: no fixtures under %s" % corpus` when
`FW_DIRS` finds no `index.html`. Not verified further here since the
directory is not class A and was out of this task's removal-focused mandate.

## Later additions, not yet covered above

Five entries postdate the classification pass above and are not third-party
material -- no new licence claim attaches to any of them:

- **`pack.py`** -- this project's own script, described in its own docstring
  as turning "a framework's BUILT OUTPUT into a probe fixture"; same
  authorship and licence (this project's outer MIT, per `LICENSING.md`) as
  `build_apps.sh`, already covered above.
- **`_platform/`** -- a project-authored fixture (`_platform/SOURCE`:
  `https://platform.fixture.logitos/`, a synthetic in-repo URL, not a live
  site) with a manifest and one script (`s001.js`) exercising
  `document.currentScript` and related platform APIs -- Class C, same
  treatment as `_src/<name>/` above.
- **`_paint/<name>/`** -- one `SOURCE`/`index.html`/`manifest.txt` triplet per
  framework (react/vue/svelte/angular/next/vite/webpack), each `SOURCE`
  a synthetic `https://<name>.fixture.logitos/` URL and each `index.html`
  project-authored (it loads `/_paint.js`, not a vendored bundle) -- these
  hold no framework runtime code of their own (no `s*.js` bundle), so unlike
  the top-level `<name>/` directories above there is no Class B content here
  to license.
- **`BASELINE`** and **`CHROMEDIFF`** -- plain-text measurement output (a
  per-framework API-availability count and a Chrome differential report,
  both project-generated), not vendored data.
- **`.gitattributes`** -- repository hygiene (line-ending handling for the
  bundle files above), not vendored data, same role as the one in
  `tests/fixtures/fonts/`.

## History

First added: commit `cd7060bd7` ("fixtures: seven framework builds, and the
bundler was not the variable"), 2026-08-08.
