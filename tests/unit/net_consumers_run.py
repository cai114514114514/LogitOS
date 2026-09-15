#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run the real net CLI over host files and loopback HTTP, with independent hashes."""
import argparse
import hashlib
import http.server
import os
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[2]
B3 = {int(n): h[:64] for n, h in re.findall(r'\{\s*(\d+),\s*"([0-9a-f]+)"',
    (ROOT / 'tests/unit/blake3_vectors.inc').read_text())}
DATA = bytes(i % 251 for i in range(300123))
checks = 0
calls = []


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *_):
        pass

    def do_GET(self):
        calls.append((self.path, dict(self.headers)))
        path = self.path.split('?')[0]
        body = DATA
        code = 200
        if path == '/missing': code = 404
        if path == '/partial': code = 206
        if path == '/empty': body = b''
        if path == '/b3': body = DATA[:1025]
        if path == '/large-save': body = (DATA * 32)[:9 * 1024 * 1024]
        if path == '/truncated': body = b'cut'
        if path in ('/redirect', '/loop', '/query', '/bad-scheme'):
            if path == '/query' and '?' in self.path:
                pass
            else:
                self.send_response(302)
                loc = {'/redirect': '/data', '/loop': '/loop', '/query': '?v=2',
                       '/bad-scheme': 'ftp://example.invalid/file'}[path]
                self.send_header('Location', loc)
                self.send_header('Content-Length', '0')
                self.end_headers()
                return
        self.send_response(code)
        if path == '/attachment': self.send_header('Content-Disposition', "attachment; filename*=UTF-8''%E4%B8%8B%E8%BD%BD.bin")
        if path == '/compressed': self.send_header('Content-Encoding', 'gzip')
        if path == '/large':
            self.send_header('Content-Length', str(65 * 1024 * 1024)); body = b''
        elif path == '/chunked': self.send_header('Transfer-Encoding', 'chunked')
        elif path != '/eof': self.send_header('Content-Length', str(len(body) + (20 if path == '/truncated' else 0)))
        self.send_header('Connection', 'close')
        self.end_headers()
        try:
            if path == '/chunked':
                for i in range(0, len(body), 997):
                    block = body[i:i+997]
                    self.wfile.write(f'{len(block):x}\r\n'.encode() + block + b'\r\n')
                self.wfile.write(b'0\r\nX-Test: complete\r\n\r\n')
            else: self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass
        self.close_connection = True


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--binary', type=Path, required=True)
    p.add_argument('--negative', action='store_true')
    a = p.parse_args()
    binary = a.binary.resolve()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f'http://127.0.0.1:{server.server_port}'

    def run(args, status=0, contains='', env=None, stdin=None):
        global checks
        result = subprocess.run([str(binary), *args], input=stdin, capture_output=True,
            env={**os.environ, **(env or {})}, timeout=10)
        text = (result.stdout + result.stderr).decode(errors='replace')
        checks += 1
        assert result.returncode == status and contains in text, (args, result.returncode, text)
        return result.stdout.decode()

    try:
        with tempfile.TemporaryDirectory(prefix='net-consumers-') as tmp:
            file = Path(tmp) / 'input.bin'; file.write_bytes(DATA)
            dest = Path(tmp) / 'output.bin'; dest.write_bytes(b'keep-existing')
            sha = hashlib.sha256(DATA).hexdigest()
            if a.negative:
                run(['verify', 'sha256', sha, str(file)], 2, 'checksum mismatch')
                run(['get', base + '/data', 'sha256', sha], 2, 'checksum mismatch')
                run(['save', base + '/data', str(dest), 'sha256', sha], 2, 'checksum mismatch')
                assert dest.read_bytes() == b'keep-existing'
                print('CONTROL FIRED: local file, HTTP body and verified save rejected the corrupted digest')
                return
            for alg in ('sha256', 'sha512', 'blake2b'):
                expected = getattr(hashlib, alg)(DATA).hexdigest()
                run(['checksum', alg, str(file)], contains=expected)
                run(['verify', alg, expected.upper(), str(file)], contains='verified')
                run(['verify', alg, '0' * len(expected), str(file)], 2, 'checksum mismatch')
                run(['checksum', alg, '-'], stdin=DATA, contains=expected)
                run(['get', base + '/data', alg, expected], contains='checksum verified')
            # Every official unkeyed BLAKE3 vector crosses this actual file
            # consumer, including the subtree boundaries. No extra pip package.
            for n, expected in B3.items():
                file.write_bytes(DATA[:n])
                run(['checksum', 'blake3', str(file)], contains=expected)
            run(['get', base + '/b3', 'blake3', B3[1025]], contains='checksum verified')
            file.write_bytes(DATA)
            run(['checksum', 'sha256', str(file)], 1, 'read failed', env={'NET_TEST_READ_FAIL': '1'})
            run(['checksum', 'sha256', str(file) + '-absent'], 1, 'read failed')
            run(['verify', 'sha256', 'a', str(file)], 1, 'invalid algorithm')
            run(['verify', 'sha256', 'z' * 64, str(file)], 1, 'invalid algorithm')
            run(['get', base + '/data'])
            for path in ('/data', '/chunked', '/eof', '/redirect', '/query', '?v=2'):
                run(['get', base + path, 'sha256', sha], contains='http bytes 300123')
            run(['get', base + '/data#not-sent', 'sha256', sha], contains='checksum verified')
            run(['get', base + '/empty', 'sha256', hashlib.sha256(b'').hexdigest()], contains='http bytes 0')
            for path, message in [('/truncated', 'closed mid-message'), ('/missing', 'HTTP status 404'),
                                  ('/partial', 'HTTP status 206'), ('/large', 'exceeds limit'),
                                  ('/compressed', 'encoding'), ('/loop', 'too many redirects'),
                                  ('/bad-scheme', 'refused redirect')]:
                run(['save', base + path, str(dest), 'sha256', sha], 1, message)
                assert dest.read_bytes() == b'keep-existing', 'failed transfer changed destination'
            run(['save', base + '/data', str(dest), 'sha256', '0'*64], 2, 'checksum mismatch')
            assert dest.read_bytes() == b'keep-existing'
            run(['save', base + '/data', str(dest), 'sha256', sha], 1, 'write failed', env={'NET_TEST_WRITE_FAIL': '1'})
            assert dest.read_bytes() == b'keep-existing'
            run(['save', base + '/chunked', str(dest), 'sha256', sha], contains='saved ')
            assert dest.read_bytes() == DATA, 'saved bytes differ from server bytes'
            run(['verify', 'sha256', sha, str(dest)], contains='verified')
            env={'NET_TEST_DOWNLOAD_ROOT':tmp}
            run(['download',base+'/attachment','sha256',sha],contains='saved /download/下载.bin',env=env)
            run(['download',base+'/attachment'],contains='saved /download/下载 (2).bin',env=env)
            assert (Path(tmp)/'download/下载.bin').read_bytes()==DATA
            assert (Path(tmp)/'download/下载 (2).bin').read_bytes()==DATA
            run(['download',base+'/empty'],contains='http bytes 0',env=env)
            assert (Path(tmp)/'download/empty').read_bytes()==b''
            before=set((Path(tmp)/'download').iterdir())
            run(['download',base+'/data','sha256','0'*64],2,'checksum mismatch',env=env)
            run(['download',base+'/truncated'],1,'closed mid-message',env=env)
            assert set((Path(tmp)/'download').iterdir())==before
            big=(DATA*32)[:9*1024*1024];bigsha=hashlib.sha256(big).hexdigest()
            run(['save',base+'/large-save',str(dest),'sha256',bigsha],contains='http bytes 9437184')
            assert dest.read_bytes()==big
            for url in ('http://user@example.invalid/x', 'http://' + 'a'*128 + '/x',
                        base + '/' + 'x'*512, 'http://localhost:80junk/x', 'ftp://localhost/x'):
                before = len(calls)
                run(['get', url], 1, 'invalid or unsupported URL')
                assert len(calls) == before
            assert all(h.get('Accept-Encoding') == 'identity' for _, h in calls)
            assert any(path == '/?v=2' for path, _ in calls)
            assert not any('#' in path for path, _ in calls)
            assert len(B3) == 35
            print(f'net-consumers: {checks} command checks passed; saved bytes exact; failed transfers preserve destination')
    finally:
        server.shutdown(); server.server_close()


if __name__ == '__main__': main()
