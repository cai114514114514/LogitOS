#!/usr/bin/env python3
"""check_provenance.py -- every PEM in tools/roots/ must be ACCOUNTED FOR.

"Accounted for" means the SHA-256 fingerprint of the certificate's DER bytes
(not the PEM text, not the filename, not the subject string -- the exact bytes
of the certificate object) appears in one of two recorded ledgers, both
committed beside this script:

  1. certdata-fingerprints.txt -- every certificate object Mozilla's NSS
     certdata.txt carried on the date recorded in README.md, regardless of
     its CKA_TRUST_SERVER_AUTH value. A root that is in this file but marked
     CKT_NSS_MUST_VERIFY_TRUST is real and traceable, just not currently
     Mozilla-trusted for TLS server auth -- see README.md's "roots we hold
     that Mozilla no longer trusts" list.

  2. legacy-exceptions.txt -- roots that were in tools/roots/ before this
     audit and are NOT in today's certdata.txt at all (Mozilla has fully
     removed them from the shipped file, not merely downgraded their trust
     bit). They are recorded here BY NAME, with their own fingerprint, as a
     closed, deliberate list -- not a wildcard. Adding to this file is a
     decision, made the same way the original 42 were: verified against a
     dated snapshot and written down, not typed from memory. See README.md.

A fingerprint in NEITHER ledger means the PEM's provenance was never
recorded anywhere in this directory's history. That is what this check
exists to catch -- not "is this a well-formed certificate" (openssl already
answers that), but "did anyone account for how it got here".

This script is deliberately NOT wired into the Makefile (tools/roots/** is
this audit's own area; the Makefile is contended -- see the top-level
instructions this audit was run under). Run it by hand or from a shell
script:

    python3 tools/roots/check_provenance.py

Exit 0 and a one-line summary if every PEM is accounted for. Exit 1 and one
line per UNACCOUNTED PEM, naming the file, if not.
"""
import base64
import glob
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def load_ledger(path):
    """fingerprint -> (extra columns...) from a TSV: fp \\t col2 \\t col3..."""
    out = {}
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            fp = parts[0].strip().lower()
            out[fp] = parts[1:]
    return out


def pem_to_der(path):
    body = []
    keep = False
    with open(path, encoding="ascii", errors="strict") as f:
        for line in f:
            if "BEGIN CERTIFICATE" in line:
                keep = True
                continue
            if "END CERTIFICATE" in line:
                break
            if keep:
                body.append(line.strip())
    return base64.b64decode("".join(body))


def main():
    certdata_ledger = load_ledger(os.path.join(HERE, "certdata-fingerprints.txt"))
    legacy_ledger = load_ledger(os.path.join(HERE, "legacy-exceptions.txt"))

    if not certdata_ledger and not legacy_ledger:
        print("check_provenance: BOTH ledgers are empty or missing -- refusing to "
              "run a check that would pass vacuously. Expected "
              f"{os.path.join(HERE, 'certdata-fingerprints.txt')} and "
              f"{os.path.join(HERE, 'legacy-exceptions.txt')}.", file=sys.stderr)
        return 2

    pems = sorted(glob.glob(os.path.join(HERE, "*.pem")))
    if not pems:
        print("check_provenance: no *.pem files found in tools/roots/ -- "
              "nothing to check (this is a warning, not a pass)", file=sys.stderr)
        return 0

    unaccounted = []
    from_certdata = 0
    from_legacy = 0
    for path in pems:
        name = os.path.basename(path)
        try:
            der = pem_to_der(path)
        except Exception as ex:
            unaccounted.append((name, f"could not parse as a PEM certificate: {ex}"))
            continue
        fp = hashlib.sha256(der).hexdigest()
        if fp in certdata_ledger:
            from_certdata += 1
        elif fp in legacy_ledger:
            from_legacy += 1
        else:
            unaccounted.append((name, f"sha256={fp} matches neither ledger"))

    if unaccounted:
        print(f"check_provenance: {len(unaccounted)} PEM(s) in tools/roots/ are "
              f"UNACCOUNTED FOR (fingerprint not in certdata-fingerprints.txt or "
              f"legacy-exceptions.txt):", file=sys.stderr)
        for name, reason in unaccounted:
            print(f"  UNACCOUNTED  {name}: {reason}", file=sys.stderr)
        print("Add it to the record deliberately (README.md explains how) before "
              "it ships, or remove it.", file=sys.stderr)
        return 1

    print(f"check_provenance: {len(pems)} PEM(s) all accounted for "
          f"({from_certdata} in certdata-fingerprints.txt, "
          f"{from_legacy} in legacy-exceptions.txt)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
