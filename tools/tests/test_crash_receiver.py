import hashlib
import importlib.util
import contextlib
import io
import os
from pathlib import Path
import socket
import tempfile
import threading
import time
import unittest
from unittest import mock
from urllib import request, error

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('crash_reports', ROOT / 'relay/crash_reports.py')
receiver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(receiver)


class CrashReceiverTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.server = receiver.ReportServer(('127.0.0.1', 0), Path(self.folder.name))
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = 'http://127.0.0.1:%s/v1/crash-reports' % self.server.server_port

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.folder.cleanup()

    def post(self, data, identity=None, url=None):
        req = request.Request(url or self.url, data=data, headers={
            'Content-Type': 'text/plain; charset=utf-8',
            'X-Report-ID': identity or hashlib.sha256(data).hexdigest()})
        try:
            with request.urlopen(req, timeout=3) as response:
                return response.status, response.read()
        except error.HTTPError as response:
            return response.code, response.read()

    def test_durable_and_idempotent(self):
        data = b'ShiningSource report\ncrash: test\n'
        self.assertEqual(self.post(data)[0], 201)
        self.assertEqual(self.post(data)[0], 200)
        files = list(Path(self.folder.name).glob('*.txt'))
        self.assertEqual(len(files), 1)
        self.assertEqual(files[0].read_bytes(), data)

    def test_bad_hash_and_path_do_not_write(self):
        self.assertEqual(self.post(b'ShiningSource report\n', 'a' * 64)[0], 400)
        self.assertEqual(self.post(b'ShiningSource report\n', '../anything')[0], 400)
        self.assertEqual(self.post(b'ShiningSource report\n', url=self.url + '/other')[0], 404)
        self.assertEqual(list(Path(self.folder.name).iterdir()), [])

    def test_rejects_nonreport_binary_and_large(self):
        self.assertEqual(self.post(b'not a diagnostic')[0], 400)
        self.assertEqual(self.post(b'ShiningSource report\n\xff')[0], 400)
        self.assertEqual(self.post(b'x' * (receiver.MAX_BYTES + 1))[0], 413)

    def test_quota_still_accepts_duplicates(self):
        self.server.max_reports = 1
        first = b'ShiningSource report\nfirst'
        self.assertEqual(self.post(first)[0], 201)
        self.assertEqual(self.post(b'ShiningSource report\nsecond')[0], 503)
        self.assertEqual(self.post(first)[0], 200)

    def test_startup_prunes_expired_reports_and_abandoned_writes(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            now = time.time()
            files = {
                'old.txt': receiver.RETENTION_SECONDS + 10,
                'current.txt': 60,
                'old.pending': 3610,
                'active.pending': 60,
            }
            for name, age in files.items():
                path = folder / name
                path.write_bytes(b'test')
                os.utime(path, (now - age, now - age))
            server = receiver.ReportServer(('127.0.0.1', 0), folder)
            try:
                self.assertEqual({p.name for p in folder.iterdir()},
                                 {'current.txt', 'active.pending'})
            finally:
                server.server_close()

    def test_idle_service_prunes_without_an_upload(self):
        path = Path(self.folder.name) / 'expired.txt'
        path.write_bytes(b'old report')
        old = time.time() - receiver.RETENTION_SECONDS - 10
        os.utime(path, (old, old))
        # Trigger the real serve_forever maintenance path without a request.
        self.server.last_cleanup = time.time() - 3601
        deadline = time.monotonic() + 2
        while path.exists() and time.monotonic() < deadline:
            time.sleep(.02)
        self.assertFalse(path.exists(), 'retention must not depend on new uploads')

    def test_absolute_deadline_closes_trickling_headers_and_body(self):
        real_timer = threading.Timer
        def quick_deadline(seconds, callback):
            self.assertEqual(seconds, 10)
            return real_timer(.35, callback)
        headers = (b'POST /v1/crash-reports HTTP/1.0\r\n'
                   b'Content-Type: text/plain\r\nContent-Length: 1000\r\n'
                   b'X-Report-ID: ' + b'a' * 64 + b'\r\n\r\n')
        for initial in (b'POST /v1/crash-reports HTTP/1.0\r\nX-Trickle: ', headers):
            with self.subTest(phase='body' if initial == headers else 'headers'):
                interrupted_output = io.StringIO()
                with mock.patch.object(receiver.threading, 'Timer', side_effect=quick_deadline), \
                     contextlib.redirect_stderr(interrupted_output), \
                     contextlib.redirect_stdout(interrupted_output):
                    with socket.create_connection(self.server.server_address, timeout=1) as client:
                        client.settimeout(.04)
                        client.sendall(initial)
                        deadline = time.monotonic() + 1.5
                        closed = False
                        while time.monotonic() < deadline:
                            try:
                                client.sendall(b'x')
                                if not client.recv(1):
                                    closed = True
                                    break
                            except socket.timeout:
                                continue
                            except OSError:
                                closed = True
                                break
                        self.assertTrue(closed, 'continuous trickling must not renew the deadline')
                    recovered = 0
                    try:
                        for _ in range(8):
                            self.assertTrue(self.server.slots.acquire(timeout=1), 'expired worker must release its slot')
                            recovered += 1
                    finally:
                        for _ in range(recovered):
                            self.server.slots.release()
                self.assertNotIn('127.0.0.1', interrupted_output.getvalue())
                self.assertNotIn('Traceback', interrupted_output.getvalue())
        # Workers must return their slots after expiry; health remains available.
        with request.urlopen(self.url.replace('/v1/crash-reports', '/health'), timeout=2) as response:
            self.assertEqual(response.status, 200)


if __name__ == '__main__':
    unittest.main()
