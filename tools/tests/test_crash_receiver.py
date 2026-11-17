import hashlib
import importlib.util
from pathlib import Path
import tempfile
import threading
import unittest
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


if __name__ == '__main__':
    unittest.main()
