---
name: logitos-docs
description: Find the LogitOS documentation for the code you are about to change, and keep it correct afterwards. Use before editing any source file you have not read docs for, and after any change that alters behaviour, names, files or make targets.
---

# Use and maintain the documentation

**Before changing code**
- Ask the docs MCP server (`.mcp.json`, server `logitos-docs`): `docs_for_source` with the file or
  directory, then read its primary pages with `read_doc` (a single `section` is enough when you know
  what you need). `search_docs` finds pages by function or concept.
- Without MCP: `docs/README.md` is the index; the website's `/docs-index.json` has the same
  source-to-page map in `by_source`, and every page is plain Markdown under `docs/`.

**After changing code**
1. Run `python3 tools/docs/stale.py` (or `make docs-stale`). It lists pages naming a file that changed
   since the commit recorded for the page in `docs/VERIFIED.json`.
2. Fix those pages against the current code: correct what became false, add what is new, remove what
   is gone. Name only files, functions and targets that exist; state limitations once, under
   `## Limitations`.
3. Record the check: `python3 tools/docs/stale.py --mark docs/<section>/<page>.md`.

Writing rules: `docs/contributing/documentation-style.md`.
