#!/usr/bin/env python3
"""Which documentation pages may be out of date?

A page is suspect when a source file it names has changed since the commit recorded for it in
docs/VERIFIED.json. Run this after changing code, then re-read and fix the pages it lists; after a page has been
checked against the current tree, record that with --mark so it drops off the list.

  python3 tools/docs/stale.py                 # suspect pages against HEAD
  python3 tools/docs/stale.py --ref main      # against another ref
  python3 tools/docs/stale.py --json          # machine-readable
  python3 tools/docs/stale.py --mark docs/kernel/scheduling.md [...]   # record HEAD as verified for these pages
  python3 tools/docs/stale.py --mark-all      # record HEAD for every page (after a full refresh)

Exit status: 0 when nothing is suspect, 1 when some page is, 2 on a usage error.
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import docs_index  # noqa: E402


def mark(pages, commit):
    """Record commit as the verified commit for the given page paths in docs/VERIFIED.json."""
    data = {'about': 'Commit each page was last checked against; maintained by tools/docs/stale.py --mark.', 'pages': {}}
    if os.path.exists(docs_index.VERIFIED):
        data = json.load(open(docs_index.VERIFIED, encoding='utf-8'))
    for page in pages:
        data['pages'][page] = commit
    data['pages'] = dict(sorted(data['pages'].items()))
    with open(docs_index.VERIFIED, 'w', encoding='utf-8') as out:
        json.dump(data, out, indent=1)
        out.write('\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--ref', default='HEAD', help='compare against this ref (default HEAD)')
    parser.add_argument('--json', action='store_true', help='print JSON')
    parser.add_argument('--mark', nargs='+', metavar='PAGE', help='record HEAD as verified for these pages')
    parser.add_argument('--mark-all', action='store_true', help='record HEAD as verified for every page')
    args = parser.parse_args()
    pages = docs_index.load_pages()
    known = {page['path'] for page in pages}
    head = docs_index.git('rev-parse', '--short', 'HEAD')
    if args.mark or args.mark_all:
        targets = sorted(known) if args.mark_all else args.mark
        unknown = [page for page in targets if page not in known]
        if unknown:
            print('not a documentation page: ' + ', '.join(unknown), file=sys.stderr)
            return 2
        mark(targets, head)
        print(f'marked {len(targets)} page(s) as verified at {head}')
        return 0
    report = docs_index.stale_report(pages, args.ref)
    suspect = [row for row in report if row['changed_sources']]
    unverified = [row['page'] for row in report if row['verified'] is None]
    if args.json:
        print(json.dumps({'ref': args.ref, 'suspect': suspect, 'unverified': unverified}, indent=1))
    else:
        for row in suspect:
            print(f"{row['page']}  (verified {row['verified']}): " + ', '.join(row['changed_sources'][:8])
                  + (' …' if len(row['changed_sources']) > 8 else ''))
        if unverified:
            print(f'{len(unverified)} page(s) have no verified commit: ' + ', '.join(unverified[:10])
                  + (' …' if len(unverified) > 10 else ''))
        print(f'{len(suspect)} of {len(report)} pages may be out of date against {args.ref}.')
    return 1 if suspect else 0


if __name__ == '__main__':
    sys.exit(main())
