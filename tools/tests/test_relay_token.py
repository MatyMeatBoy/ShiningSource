"""Native UDP identity and cleanup across host processes."""
import os
import socket
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import build


HARNESS = """
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shiningsource/runtime.h"
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
void ss_plat_pump(void) {}
int main(int argc, char **argv) {
    if (argc == 3) {
        assert(ss_plat_net_open(argv[1], atoi(argv[2])) == 0);
        ss_plat_net_farewell("bye", 3);
        ss_plat_net_send_farewell();
        _exit(0);
    }
    char token[11];
    const char *default_token = ss_plat_net_default_token();
    assert(default_token && *default_token);
    assert(strtoul(default_token, NULL, 10) != 0);
    snprintf(token, sizeof token, "%s", default_token);
    assert(strcmp(token, ss_plat_net_default_token()) == 0);
    puts(token);
    return 0;
}
"""


class RelayTokenTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(directory.cleanup)
        out = Path(directory.name)
        harness = out / "token.c"
        harness.write_text(HARNESS)
        cls.binary = out / "token.exe"
        root = build.ROOT
        libs = ["-lws2_32"] if os.name == "nt" else []
        subprocess.run([
            build.clang(), "-std=gnu11", "-I", str(root / "runtime/include"),
            str(harness), str(root / "runtime/plat/net_udp.c"), "-o", str(cls.binary), *libs,
        ], check=True)

    def test_default_tokens_are_per_process_and_stable(self):
        tokens = {subprocess.check_output([str(self.binary)], text=True).strip() for _ in range(4)}
        self.assertEqual(len(tokens), 4)

    def test_explicit_farewell_is_sent_before_immediate_exit(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as receiver:
            receiver.bind(("127.0.0.1", 0))
            receiver.settimeout(2)
            for host in ("127.0.0.1", "localhost"):
                with self.subTest(host=host):
                    subprocess.run([str(self.binary), host, str(receiver.getsockname()[1])],
                                   check=True, timeout=5)
                    self.assertEqual([receiver.recv(16), receiver.recv(16)], [b"bye", b"bye"])


if __name__ == "__main__":
    unittest.main()
