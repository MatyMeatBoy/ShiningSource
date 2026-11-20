"""Exercise discovery and admission against the actual UDP relay."""
import os
import socket
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_remote_sfx import build


def packet(kind, identity=0, token=1, cursor=0, scope=0, flags=0, code=b""):
    header = bytearray(60)
    struct.pack_into("<4sBBBB6I", header, 0, b"SSN1", kind, 255, 2, flags,
                     identity, token, cursor, 0, 0, scope)
    return header + (code.ljust(32, b"\0") if code else b"")


class PublicLobbyTests(unittest.TestCase):
    def test_rooms_appear_fill_and_close_without_affecting_legacy_rooms(self):
        with tempfile.TemporaryDirectory() as folder:
            exe = Path(folder) / ("relay.exe" if os.name == "nt" else "relay")
            libraries = ["-lws2_32"] if os.name == "nt" else []
            subprocess.run([build.clang(), "-O2", str(build.ROOT / "relay/ss_relay.c"),
                            "-o", str(exe), *libraries], check=True)
            with subprocess.Popen([str(exe), "0"], stderr=subprocess.PIPE,
                                  env={**os.environ, "SS_RELAY_MIN_BUILD": "904",
                                       "SS_RELAY_LOOPBACK": "1"}) as relay:
                try:
                    headline = relay.stderr.readline().split()
                    self.assertEqual(headline[:-1], [b"ss_relay", b"on", b"UDP", b"port"])
                    port = int(headline[-1])
                    self.assertGreater(port, 0)
                    self.exercise_rooms(port)
                finally:
                    relay.terminate()

    def exercise_rooms(self, port):
        from contextlib import ExitStack

        with ExitStack() as stack:
            players = []
            for _ in range(6):
                peer = stack.enter_context(socket.socket(socket.AF_INET, socket.SOCK_DGRAM))
                peer.bind(("127.0.0.1", 0))
                peer.connect(("127.0.0.1", port))
                peer.settimeout(0.3)
                players.append(peer)
            host, *guests, observer = players
            scope, room, code = 110, 221, b"ABCD2345"

            def page(scope=scope, cursor=0):
                observer.send(packet(6, scope, 99, cursor))
                reply = observer.recv(1200)
                self.assertEqual(len(reply), 60)
                self.assertEqual(reply[4], 7)
                self.assertEqual(struct.unpack_from("<II", reply, 8), (scope, 99))
                self.assertLessEqual(reply[7], 4)
                return ([(reply[20 + i * 9:28 + i * 9], reply[28 + i * 9])
                         for i in range(reply[7])], struct.unpack_from("<I", reply, 16)[0])

            def query(scope=scope):
                return page(scope)[0]

            def hello(peer, token, flags, identity=room):
                request = packet(8, identity, token, scope=scope, flags=flags, code=code)
                struct.pack_into("<I", request, 32, 904)
                peer.send(request)
                return peer.recv(1200)

            self.assertEqual(query(), [])
            self.assertEqual(hello(host, 1, 1)[5], 0)
            self.assertEqual(query(), [(code, 1)])
            self.assertEqual(query(111), [])
            with self.assertRaises(socket.timeout):
                hello(guests[0], 2, 1)  # Create cannot hijack a listed room.
            for count, guest in enumerate(guests[:3], 1):
                self.assertEqual(hello(guest, count + 1, 2)[5], count)
            self.assertEqual(query(), [])
            with self.assertRaises(socket.timeout):
                hello(guests[3], 5, 2)
            guests[2].send(packet(4, room, 4))
            self.assertEqual(query(), [(code, 3)])
            self.assertEqual(hello(guests[2], 4, 2)[5], 3)
            self.assertEqual(query(), [])
            guests[2].send(packet(4, room, 4))
            self.assertEqual(query(), [(code, 3)])
            game = packet(3, room, 1, flags=1) + bytes(8) + b"GO" + bytes(14)
            host.send(game)
            self.assertEqual(query(), [])
            with self.assertRaises(socket.timeout):
                hello(guests[3], 5, 2)
            guests[3].send(packet(1, 333, 5, code=b"PRIVATE"))
            self.assertEqual(guests[3].recv(1200)[5], 0)
            self.assertEqual(query(), [])
            other_room = room + 1
            self.assertEqual(hello(observer, 6, 1, other_room)[5], 0)
            self.assertEqual(query(), [(code, 1)])
            observer.send(packet(4, other_room, 6))
            self.assertEqual(query(), [])

            labels = [("HALLAB2" + str(i)).encode() for i in range(2, 8)]
            for i, label in enumerate(labels):
                request = packet(8, 400 + i, 10 + i, scope=scope, flags=1, code=label)
                struct.pack_into("<I", request, 32, 904)
                observer.send(request)
                self.assertEqual(observer.recv(1200)[5], 0)
            first, next_cursor = page()
            second, last_cursor = page(cursor=next_cursor)
            self.assertEqual((len(first), len(second), last_cursor), (4, 2, 0))
            self.assertCountEqual(first + second, [(label, 1) for label in labels])

            def named_create(identity, token, label, title=b""):
                hello = packet(8, identity, token, scope=scope + 1, flags=1, code=label)
                struct.pack_into("<I", hello, 32, 904)
                hello[68:68 + len(title)] = title
                host.send(hello)
                while True:
                    reply = host.recv(1200)
                    if reply[4] == 2 and struct.unpack_from("<II", reply, 8) == (identity, token):
                        return reply[28:44].rstrip(b"\0")

            first = named_create(501, 50, b"ABCD2355")
            second = named_create(502, 51, b"ABCD2356")
            self.assertEqual((first, second), (b"ShiningSource-01", b"ShiningSource-02"))
            self.assertEqual(named_create(503, 52, b"ABCD2357", b"Friends"), b"Friends")
            self.assertEqual(named_create(504, 53, b"ABCD2358", b"Friends"), b"Friends")

            def named_list(version):
                request = packet(6, scope + 1, 99) + bytes(64)
                struct.pack_into("<I", request, 20, version)
                observer.send(request)
                reply = observer.recv(1200)
                self.assertEqual(len(reply), len(request))
                self.assertEqual(struct.unpack_from("<I", reply, 120)[0], 904)
                return [reply[29 + i * 25:45 + i * 25].rstrip(b"\0") for i in range(reply[7])]

            self.assertEqual(named_list(903), [])
            self.assertCountEqual(named_list(904), [first, second, b"Friends", b"Friends"])
            old = packet(8, 505, 54, scope=scope + 1, flags=1, code=b"ABCD2359")
            struct.pack_into("<I", old, 32, 903)
            browser = guests[3]
            browser.send(old)
            reply = browser.recv(1200)
            self.assertEqual(reply[4], 9)
            self.assertEqual(struct.unpack_from("<I", reply, 28)[0], 904)


if __name__ == "__main__":
    unittest.main()
