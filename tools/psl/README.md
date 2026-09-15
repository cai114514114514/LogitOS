# Cookie Public Suffix List

The jar and schemeful SameSite classifier share the complete ICANN and PRIVATE
sections of the Mozilla Public Suffix List, including wildcard and exception
rules. The former short ccTLD heuristic could both over-reject and under-reject;
for example, its old claim that `com.de` is registrable is also stale against
this snapshot, which includes that domain in the PRIVATE section.

Vendored source: https://publicsuffix.org/list/public_suffix_list.dat

- Version: `2026-09-08_12-18-37_UTC`
- Commit: `3955e3ec29b94c3cca7bd4509c5f14a7c0959e26`
- Source SHA-256: `4b673689999dbaca60b93fa3e1da5752505ef9717b1c4dc44acbfdafd35679ea`
- Rules: 10,325, including PRIVATE rules
- Source and generated table license: MPL-2.0, as recorded in both files.
- Upstream test fixture: `publicsuffix/list` at the same commit,
  `tests/test_psl.txt`; CC0/public domain notice retained in the file.

The browser build does not fetch data at startup or build time. To update, fetch
only the official source URL above (no more than once per day), review its
version/change, replace the vendored source and matching upstream fixture, run
`python3 tools/psl/generate.py`, then run
`make BUILD=build-cookie-core-fix test-cookie-psl test-cookie-hardening`.
Update this provenance alongside the data. `generate.py --check` rejects stale
generated data; the gate compares the compiled matcher with every raw rule and
with the upstream expected registrable domains.

The generated table uses 32-bit offsets and a NUL-separated string blob to avoid
10,325 pointer relocations. Source U-labels are normalized and encoded as
A-labels without IDNA2003 mappings. Runtime callers supply canonical URL A-label
hosts; the cookie boundary rejects raw Unicode rather than inventing a partial
IDNA implementation. This does not claim that the browser's separate URL parser
implements complete IDNA.
