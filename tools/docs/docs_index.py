"""Read the developer documentation under docs/ the way agents need it.

Shared by tools/docs/stale.py and tools/docs/mcp_server.py. The website build (website/scripts/build-docs.mjs)
derives the same facts in JavaScript; the rules here mirror it so both agree on which page describes which file:

- a page is a Markdown file in one of SECTIONS;
- its summary is the first sentence of its first prose paragraph;
- its sources are the repository paths it names in backticks, ranked by how often it names them;
- docs/VERIFIED.json records the commit each page was last checked against.

Standard library only, so any agent can run it with the system python3.
"""
import fnmatch
import json
import os
import re
import subprocess

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DOCS = os.path.join(REPO, 'docs')
VERIFIED = os.path.join(DOCS, 'VERIFIED.json')

# Section directories in reading order; the website uses the same list.
SECTIONS = ['getting-started', 'architecture', 'kernel', 'drivers', 'filesystems', 'networking', 'crypto',
            'userland', 'browser', 'aetherscript', 'libraries', 'reference', 'contributing']

# A backticked token counts as a source path when it starts at a top-level source directory or names one of the
# root files pages cite. Globs such as c/drivers/usb/*.c are kept: they still tell an agent where to look.
PATH_RE = re.compile(r'^(?:c|include|tools|tests|third_party|scripts)/[\w./*{},-]+$'
                     r'|^(?:Makefile|linker\.ld|README\.md|TRANSPARENCY\.md)$')
TICK_RE = re.compile(r'`([^`\n]+)`')


def git(*args):
    """Run git in the repository and return stdout, or '' when git fails (a tarball has no history)."""
    result = subprocess.run(['git', '-C', REPO, *args], capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else ''


def plain(text):
    """Strip Markdown links, emphasis and code marks so a sentence reads as text."""
    text = re.sub(r'!\[[^\]]*\]\([^)]*\)', '', text)
    text = re.sub(r'\[([^\]]+)\]\([^)]*\)', r'\1', text)
    text = re.sub(r'[`*_]', '', text)
    return re.sub(r'\s+', ' ', text).strip()


def summary(markdown):
    """First sentence of the first prose paragraph after the title, at most about 190 characters."""
    body = re.sub(r'^#\s+.+$', '', markdown, count=1, flags=re.M)
    for block in re.split(r'\n\s*\n', body):
        block = block.strip()
        if block and not re.match(r'^(#|\||```|>|- |\d+\. )', block):
            text = plain(block)
            match = re.match(r'^.+?[.!?](?=\s|$)', text)
            sentence = match.group(0) if match else text
            if len(sentence) > 190:
                sentence = re.sub(r'\s+\S*$', '', sentence[:187]) + '…'
            return sentence
    return ''


def load_pages():
    """Every documentation page with its title, section, summary, headings and ranked source paths."""
    pages = []
    for section in SECTIONS:
        folder = os.path.join(DOCS, section)
        if not os.path.isdir(folder):
            continue
        for name in sorted(os.listdir(folder)):
            if not name.endswith('.md'):
                continue
            relative = f'docs/{section}/{name}'
            markdown = open(os.path.join(folder, name), encoding='utf-8').read()
            title_match = re.search(r'^#\s+(.+)$', markdown, re.M)
            ticks = [token.strip() for token in TICK_RE.findall(markdown)]
            mentions = {}
            for token in ticks:
                if PATH_RE.match(token):
                    mentions[token] = mentions.get(token, 0) + 1
            pages.append({
                'path': relative,
                'section': section,
                'title': title_match.group(1).strip() if title_match else name[:-3],
                'summary': summary(markdown),
                'headings': [plain(h) for h in re.findall(r'^##\s+(.+)$', markdown, re.M)],
                'source_mentions': mentions,
                'markdown': markdown,
            })
    return pages


def pages_for_source(pages, source):
    """Pages naming a source path, most mentions first; 'primary' are those within half of the top count."""
    refs = [(page['source_mentions'][source], page['path']) for page in pages if source in page['source_mentions']]
    refs.sort(key=lambda item: (-item[0], item[1]))
    if not refs:
        return {'primary': [], 'pages': []}
    top = refs[0][0]
    primary = [path for count, path in refs if count * 2 >= top and count >= min(top, 2)]
    return {'primary': primary, 'pages': [{'page': path, 'mentions': count} for count, path in refs]}


def verified_commits():
    """Page -> commit it was last checked against (docs/VERIFIED.json); missing pages map to None."""
    if not os.path.exists(VERIFIED):
        return {}
    return json.load(open(VERIFIED, encoding='utf-8')).get('pages', {})


def stale_report(pages, ref='HEAD'):
    """For each page: the sources it names that changed between its verified commit and ref."""
    verified = verified_commits()
    report = []
    changed_cache = {}
    for page in pages:
        commit = verified.get(page['path'])
        if not commit:
            report.append({'page': page['path'], 'verified': None, 'changed_sources': None})
            continue
        if commit not in changed_cache:
            changed_cache[commit] = set(git('diff', '--name-only', f'{commit}..{ref}').split())
        changed = changed_cache[commit]
        hits = sorted(source for source in page['source_mentions']
                      if source in changed or ('*' in source and any(_glob_match(source, f) for f in changed)))
        report.append({'page': page['path'], 'verified': commit, 'changed_sources': hits})
    return report


def _glob_match(pattern, path):
    """Shell-style match for the globs pages cite (c/drivers/usb/*.c)."""
    return fnmatch.fnmatch(path, pattern)
