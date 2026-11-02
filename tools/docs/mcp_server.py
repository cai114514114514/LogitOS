#!/usr/bin/env python3
"""MCP server over stdio for the LogitOS developer documentation.

Agents connect to it to search the docs, read a page or one section of it, find the pages that describe a
source file, list a section, and see which pages may be out of date. It reads docs/ from this checkout, so the
answers match the code the agent is editing. Standard library only; configured for Claude Code in .mcp.json.

Protocol: JSON-RPC 2.0, one message per line on stdin/stdout (MCP stdio transport). Diagnostics go to stderr,
because anything else on stdout would corrupt the message stream.
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import docs_index  # noqa: E402

SERVER_INFO = {'name': 'logitos-docs', 'version': '1.0.0'}
SUPPORTED_VERSIONS = ['2025-06-18', '2025-03-26', '2024-11-05']
SEARCH_LIMIT_DEFAULT = 8
SEARCH_LIMIT_MAX = 25
SNIPPET_CHARACTERS = 240

TOOLS = [
    {
        'name': 'search_docs',
        'description': 'Search the LogitOS developer documentation by words (function names, file names, concepts). '
                       'Returns ranked pages with their summary and the best matching passage.',
        'inputSchema': {'type': 'object', 'properties': {
            'query': {'type': 'string', 'description': 'words to look for'},
            'limit': {'type': 'integer', 'description': f'how many pages (default {SEARCH_LIMIT_DEFAULT}, '
                                                        f'at most {SEARCH_LIMIT_MAX})'}},
            'required': ['query']},
    },
    {
        'name': 'read_doc',
        'description': 'Read one documentation page as Markdown, or only one of its ## sections.',
        'inputSchema': {'type': 'object', 'properties': {
            'path': {'type': 'string', 'description': 'page path, for example docs/kernel/scheduling.md'},
            'section': {'type': 'string', 'description': 'optional ## heading to return instead of the whole page'}},
            'required': ['path']},
    },
    {
        'name': 'docs_for_source',
        'description': 'Which pages describe a source file or directory (path relative to the repository root, '
                       'for example c/kernel/sched/sched.c or c/drivers/usb/). Read the primary pages before '
                       'changing the file.',
        'inputSchema': {'type': 'object', 'properties': {
            'path': {'type': 'string', 'description': 'source file or directory'}},
            'required': ['path']},
    },
    {
        'name': 'list_docs',
        'description': 'List documentation pages with one-line summaries, optionally for one section.',
        'inputSchema': {'type': 'object', 'properties': {
            'section': {'type': 'string', 'description': 'one of: ' + ', '.join(docs_index.SECTIONS)}}},
    },
    {
        'name': 'stale_docs',
        'description': 'Pages that may be out of date: a source file they name changed after the commit they were '
                       'last verified against.',
        'inputSchema': {'type': 'object', 'properties': {
            'ref': {'type': 'string', 'description': 'compare against this git ref (default HEAD)'}}},
    },
]


def log(message):
    """Diagnostics on stderr (stdout carries protocol messages only)."""
    print(f'logitos-docs: {message}', file=sys.stderr, flush=True)


def words(text):
    """Lower-case search terms; keeps identifiers such as sched_nice_set and file names intact."""
    return [w for w in re.findall(r'[A-Za-z0-9_./-]+', text.lower()) if len(w) > 1]


def search(pages, query, limit):
    """Rank pages: title hits weigh most, then headings, summary and source paths, then body frequency."""
    terms = words(query)
    if not terms:
        return []
    scored = []
    for page in pages:
        body = page['markdown'].lower()
        score = 0
        for term in terms:
            score += 8 * page['title'].lower().count(term)
            score += 4 * sum(h.lower().count(term) for h in page['headings'])
            score += 3 * page['summary'].lower().count(term)
            score += 3 * sum(1 for source in page['source_mentions'] if term in source.lower())
            score += min(body.count(term), 20)
        if score:
            scored.append((score, page))
    scored.sort(key=lambda item: (-item[0], item[1]['path']))
    results = []
    for score, page in scored[:limit]:
        results.append({'path': page['path'], 'title': page['title'], 'summary': page['summary'],
                        'score': score, 'passage': best_passage(page['markdown'], terms)})
    return results


def best_passage(markdown, terms):
    """The paragraph containing the most query terms, trimmed to a snippet."""
    best, best_hits = '', 0
    for block in re.split(r'\n\s*\n', markdown):
        hits = sum(block.lower().count(term) for term in terms)
        if hits > best_hits:
            best, best_hits = block, hits
    text = docs_index.plain(best)
    return text if len(text) <= SNIPPET_CHARACTERS else text[:SNIPPET_CHARACTERS].rsplit(' ', 1)[0] + ' …'


def section_of(markdown, heading):
    """The text of one ## section (heading matched case-insensitively), or None."""
    parts = re.split(r'^(##\s+.+)$', markdown, flags=re.M)
    for index in range(1, len(parts) - 1, 2):
        if docs_index.plain(parts[index][2:]).lower() == heading.strip().lower():
            return parts[index] + parts[index + 1]
    return None


def call_tool(name, arguments):
    """Run one tool and return its text result; raises ValueError for bad arguments."""
    pages = docs_index.load_pages()
    by_path = {page['path']: page for page in pages}
    if name == 'search_docs':
        limit = max(1, min(int(arguments.get('limit') or SEARCH_LIMIT_DEFAULT), SEARCH_LIMIT_MAX))
        return json.dumps(search(pages, str(arguments.get('query', '')), limit), indent=1)
    if name == 'read_doc':
        path = str(arguments.get('path', '')).lstrip('./')
        if not path.startswith('docs/'):
            path = 'docs/' + path
        page = by_path.get(path)
        if not page:
            raise ValueError(f'no page {path}; use list_docs or search_docs')
        heading = arguments.get('section')
        if heading:
            text = section_of(page['markdown'], heading)
            if text is None:
                raise ValueError(f'{path} has no section "{heading}"; sections: ' + '; '.join(page['headings']))
            return text
        return page['markdown']
    if name == 'docs_for_source':
        source = str(arguments.get('path', '')).lstrip('./')
        if source.endswith('/'):
            matches = sorted({s for page in pages for s in page['source_mentions'] if s.startswith(source)})
            ranked = {}
            for match in matches:
                for ref in docs_index.pages_for_source(pages, match)['pages']:
                    ranked[ref['page']] = ranked.get(ref['page'], 0) + ref['mentions']
            ordered = sorted(ranked.items(), key=lambda item: (-item[1], item[0]))
            return json.dumps({'directory': source, 'files_named_in_docs': len(matches),
                               'pages': [{'page': p, 'mentions': n} for p, n in ordered[:15]]}, indent=1)
        return json.dumps({'source': source, **docs_index.pages_for_source(pages, source)}, indent=1)
    if name == 'list_docs':
        section = arguments.get('section')
        if section and section not in docs_index.SECTIONS:
            raise ValueError('unknown section; one of: ' + ', '.join(docs_index.SECTIONS))
        rows = [f"{page['path']}: {page['title']} - {page['summary']}" for page in pages
                if not section or page['section'] == section]
        return '\n'.join(rows)
    if name == 'stale_docs':
        report = docs_index.stale_report(pages, str(arguments.get('ref') or 'HEAD'))
        suspect = [row for row in report if row['changed_sources']]
        return json.dumps({'suspect': suspect,
                           'unverified': [row['page'] for row in report if row['verified'] is None]}, indent=1)
    raise ValueError(f'unknown tool {name}')


def handle(message):
    """Answer one JSON-RPC request; returns None for notifications."""
    method = message.get('method')
    request_id = message.get('id')
    if request_id is None:
        return None
    if method == 'initialize':
        requested = message.get('params', {}).get('protocolVersion')
        version = requested if requested in SUPPORTED_VERSIONS else SUPPORTED_VERSIONS[0]
        result = {'protocolVersion': version, 'capabilities': {'tools': {}}, 'serverInfo': SERVER_INFO,
                  'instructions': 'LogitOS developer documentation. Before changing a source file, call '
                                  'docs_for_source and read its primary pages; after changing code, call stale_docs.'}
    elif method == 'ping':
        result = {}
    elif method == 'tools/list':
        result = {'tools': TOOLS}
    elif method == 'tools/call':
        params = message.get('params', {})
        try:
            text = call_tool(params.get('name'), params.get('arguments') or {})
            result = {'content': [{'type': 'text', 'text': text}]}
        except (ValueError, TypeError) as error:
            result = {'content': [{'type': 'text', 'text': str(error)}], 'isError': True}
    else:
        return {'jsonrpc': '2.0', 'id': request_id, 'error': {'code': -32601, 'message': f'unknown method {method}'}}
    return {'jsonrpc': '2.0', 'id': request_id, 'result': result}


def main():
    log(f'serving {docs_index.DOCS}')
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            message = json.loads(line)
        except json.JSONDecodeError:
            reply = {'jsonrpc': '2.0', 'id': None, 'error': {'code': -32700, 'message': 'parse error'}}
        else:
            reply = handle(message)
        if reply is not None:
            sys.stdout.write(json.dumps(reply) + '\n')
            sys.stdout.flush()


if __name__ == '__main__':
    main()
