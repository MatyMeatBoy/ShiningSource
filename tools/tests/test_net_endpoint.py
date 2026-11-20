"""Direct relay addresses use one IPv4/hostname parser in the launcher and game."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import build


class NetEndpoint(unittest.TestCase):
    def test_default_port_whitespace_and_validation(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / "endpoint.c"
            binary = Path(temp) / "endpoint.exe"
            source.write_text(r'''
#include <assert.h>
#include <string.h>
#include "shiningsource/net_endpoint.h"

int main(void) {
    char host[SS_NET_HOST_CAPACITY];
    int port;
    assert(ss_net_parse_endpoint("127.0.0.1", host, sizeof host, &port));
    assert(strcmp(host, "127.0.0.1") == 0 && port == SS_NET_DEFAULT_PORT);
    assert(ss_net_parse_endpoint(" \tlocalhost:47901\r\n", host, sizeof host, &port));
    assert(strcmp(host, "localhost") == 0 && port == 47901);
    assert(ss_net_parse_endpoint("relay-3.example.org:65535", host, sizeof host, &port));
    assert(strcmp(host, "relay-3.example.org") == 0 && port == 65535);
    assert(ss_net_parse_endpoint("127.0.0.1:1", host, sizeof host, &port));
    assert(port == 1);
    const char *invalid[] = {
        NULL, "", " ", ":47900", "localhost:", "localhost:0", "localhost:65536",
        "localhost:99999999999999999999999", "localhost:47900foo", "localhost:+47900",
        "localhost: 47900", "local host:47900", "localhost :47900", "localhost:4:7900",
        "localhost/int", "[::1]:47900", "::1", "udp://127.0.0.1",
    };
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i)
        assert(!ss_net_parse_endpoint(invalid[i], host, sizeof host, &port));
    assert(!ss_net_parse_endpoint("localhost", host, 9, &port));
    assert(ss_net_parse_endpoint("localhost", host, 10, &port));
}
''', encoding="utf-8")
            subprocess.run([build.clang(), "-std=c11", "-Werror", "-I",
                            str(build.ROOT / "runtime/include"), str(source), "-o",
                            str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
