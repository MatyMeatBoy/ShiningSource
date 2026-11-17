#!/usr/bin/env python3
"""Bounded crash-report ingestion; deploy behind a verified HTTPS proxy.

No retrieval route, executable uploads, ROM processing or automatic patches.
The public client contains no secret; hashes provide integrity/idempotency,
not authentication. Storage/quota and request timeouts bound abuse.
"""
import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import re
import socket
import threading
import time

MAX_BYTES = 32768
RETENTION_SECONDS = 30 * 86400


class ReportServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, folder):
        self.folder = Path(folder)
        self.folder.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.max_reports = 2000
        self.storage_lock = threading.Lock()
        self.slots = threading.BoundedSemaphore(8)
        self.last_cleanup = 0
        self.prune()
        super().__init__(address, Handler)

    def prune(self):
        now = time.time()
        for suffix, age in (('txt', RETENTION_SECONDS), ('pending', 3600)):
            for report in self.folder.glob('*.' + suffix):
                try:
                    if now - report.stat().st_mtime > age:
                        report.unlink()
                except FileNotFoundError:
                    pass
        self.last_cleanup = now

    def service_actions(self):
        if time.time() - self.last_cleanup >= 3600:
            with self.storage_lock:
                self.prune()

    def handle_error(self, request, address):
        # socketserver's default traceback includes the remote address.
        print('report request interrupted', flush=True)

    def process_request(self, request, address):
        if not self.slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, address)
        except BaseException:
            self.slots.release()
            raise

    def process_request_thread(self, request, address):
        try:
            super().process_request_thread(request, address)
        finally:
            self.slots.release()


class Handler(BaseHTTPRequestHandler):
    server_version = 'ShiningSourceReports/1'

    def setup(self):
        super().setup()
        self.connection.settimeout(5)
        self.deadline = threading.Timer(10, self.expire)
        self.deadline.daemon = True
        self.deadline.start()

    def expire(self):
        try:
            self.connection.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass

    def finish(self):
        self.deadline.cancel()
        super().finish()

    def log_message(self, *args):
        pass  # Do not journal client IPs, paths or report bodies.

    def reply(self, code):
        self.send_response(code)
        self.send_header('Content-Length', '0')
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()

    def do_GET(self):
        self.reply(200 if self.path == '/health' else 404)

    def do_POST(self):
        if self.path != '/v1/crash-reports':
            return self.reply(404)
        if self.headers.get('Transfer-Encoding'):
            return self.reply(400)
        try:
            length = int(self.headers.get('Content-Length', ''))
        except ValueError:
            return self.reply(411)
        if length < 1 or length > MAX_BYTES:
            return self.reply(413)
        identity = self.headers.get('X-Report-ID', '')
        if not re.fullmatch('[a-f0-9]{64}', identity):
            return self.reply(400)
        if self.headers.get_content_type() != 'text/plain':
            return self.reply(415)
        try:
            data = self.rfile.read(length)
            if len(data) != length or hashlib.sha256(data).hexdigest() != identity:
                return self.reply(400)
            text = data.decode('utf-8')
            if not text.startswith('ShiningSource report\n') or '\0' in text:
                return self.reply(400)
        except (UnicodeError, OSError):
            return self.reply(400)
        with self.server.storage_lock:
            folder = self.server.folder
            destination = folder / (identity + '.txt')
            if destination.is_file():
                return self.reply(200)
            try:
                self.server.prune()
                if len(list(folder.glob('*.txt'))) >= self.server.max_reports:
                    return self.reply(503)
                # Only acknowledge after a complete durable write and atomic publication.
                temporary = folder / (identity + '.pending')
                try:
                    with temporary.open('wb') as output:
                        output.write(data)
                        output.flush()
                        os.fsync(output.fileno())
                    os.replace(temporary, destination)
                    if os.name != 'nt':
                        descriptor = os.open(folder, os.O_RDONLY | os.O_DIRECTORY)
                        try:
                            os.fsync(descriptor)
                        finally:
                            os.close(descriptor)
                finally:
                    temporary.unlink(missing_ok=True)
            except OSError:
                return self.reply(503)
        self.reply(201)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bind', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=8080)
    parser.add_argument('--storage', type=Path, default=Path('/data/reports'))
    args = parser.parse_args()
    ReportServer((args.bind, args.port), args.storage).serve_forever()
