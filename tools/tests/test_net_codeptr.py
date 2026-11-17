"""Wire callbacks use stable GBA identities, never another process's native PC.

Compiles the actual codeptr.c codec against artificial per-process code tables;
no ROM, GBA mappings, SDL, network, emulators or full game builds are used.
Run: python tools/tests/test_net_codeptr.py
"""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import build

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct ss_code_entry { uint32_t gba; void (*native)(void); };
struct ss_code_entry ss_code_table[3];
const unsigned ss_code_count = 3;
const uint32_t ss_code_ranges[][2] = {{0, 0}};
const unsigned ss_code_range_count = 0;
extern int ss_code_encode(uint32_t native, uint32_t *outgba);
extern int ss_code_decode(uint32_t gba, uint32_t *outnative);
extern int ss_net_code_message(void *message, int size, int receiving);
int main(int argc, char **argv) {
    uint32_t base, input, output = 0xdeadbeef;
    int result;
    if (argc != 4) return 2;
    base = (uint32_t)strtoul(argv[1], NULL, 0);
    input = (uint32_t)strtoul(argv[3], NULL, 0);
    /* Sorted by original code identity, deliberately not native address. */
    ss_code_table[0].gba = 0x08000100;
    ss_code_table[0].native = (void (*)(void))(uintptr_t)(base + 0x300);
    ss_code_table[1].gba = 0x0803e5d9;
    ss_code_table[1].native = (void (*)(void))(uintptr_t)(base + 0x100);
    ss_code_table[2].gba = 0x080ffff1;
    ss_code_table[2].native = (void (*)(void))(uintptr_t)(base + 0x200);
    if (argv[2][0] == 's' || argv[2][0] == 'r') {
        uint8_t message[64];
        size_t i, length = strlen(argv[3]) / 2;
        if (length > sizeof message || strlen(argv[3]) % 2) return 3;
        for (i = 0; i < length; i++) {
            unsigned byte;
            if (sscanf(argv[3] + i * 2, "%2x", &byte) != 1) return 4;
            message[i] = (uint8_t)byte;
        }
        result = ss_net_code_message(message, (int)length, argv[2][0] == 'r');
        printf("%d ", result);
        for (i = 0; i < length; i++) printf("%02x", message[i]);
        printf("\n");
        return 0;
    }
    result = argv[2][0] == 'e' ? ss_code_encode(input, &output) : ss_code_decode(input, &output);
    printf("%d %08x\n", result, output);
    return 0;
}
'''


class NetCodePointer(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        directory = Path(cls.tmp.name)
        source = directory / "net_codeptr.c"
        source.write_text(HARNESS, encoding="utf-8")
        cls.exe = directory / "net_codeptr.exe"
        subprocess.run([build.clang(), "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "runtime/include"), str(source),
                        str(ROOT / "runtime/core/codeptr.c"), str(ROOT / "runtime/core/netptr.c"),
                        "-o", str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def codec(self, base, operation, address):
        result = subprocess.run([str(self.exe), hex(base), operation, hex(address)],
                                check=True, capture_output=True, text=True)
        accepted, output = result.stdout.split()
        return int(accepted), int(output, 16)

    def test_different_native_addresses_encode_the_same_original_identity(self):
        for base in (0x0A04D430, 0x21000000, 0x70000000, 0x21000001):
            with self.subTest(base=hex(base)):
                self.assertEqual(self.codec(base, "encode", base + 0x100), (1, 0x0803E5D9))

    def test_wire_identity_decodes_to_receiver_native_address_after_slide(self):
        sender, receiver = 0x0A04D430, 0x71000001
        accepted, wire = self.codec(sender, "encode", sender + 0x100)
        self.assertEqual(accepted, 1)
        self.assertEqual(self.codec(receiver, "decode", wire), (1, receiver + 0x100))
        self.assertNotEqual(receiver + 0x100, sender + 0x100)

    def test_all_entries_resolve_by_table_identity_without_native_sort_assumption(self):
        for offset, gba in ((0x300, 0x08000100), (0x100, 0x0803E5D9), (0x200, 0x080FFFF1)):
            with self.subTest(gba=hex(gba)):
                self.assertEqual(self.codec(0x60000000, "encode", 0x60000000 + offset), (1, gba))
                self.assertEqual(self.codec(0x61000001, "decode", gba), (1, 0x61000001 + offset))

    def test_thumb_and_arm_wire_bits_are_exact_table_values(self):
        self.assertEqual(self.codec(0x31000000, "encode", 0x31000100), (1, 0x0803E5D9))
        self.assertEqual(self.codec(0x31000001, "encode", 0x31000301), (1, 0x08000100))
        self.assertEqual(self.codec(0x31000001, "decode", 0x0803E5D9), (1, 0x31000101))

    def test_unknown_native_and_data_addresses_are_rejected(self):
        for address in (0, 0x03003478, 0x0803E5D9, 0x31000102, 0xFFFFFFFF):
            with self.subTest(address=hex(address)):
                self.assertEqual(self.codec(0x31000000, "encode", address)[0], 0)

    def test_unknown_wire_identity_and_foreign_native_pc_are_rejected(self):
        for address in (0, 0x03003478, 0x0803E5D8, 0x0803E5DB, 0x0A04D530, 0xFFFFFFFF):
            with self.subTest(address=hex(address)):
                self.assertEqual(self.codec(0x61000000, "decode", address)[0], 0)

    def message(self, base, operation, message):
        result = subprocess.run([str(self.exe), hex(base), operation, bytes(message).hex()],
                                check=True, capture_output=True, text=True)
        accepted, _, output = result.stdout.strip().partition(" ")
        return int(accepted), bytes.fromhex(output)

    @staticmethod
    def packet(tag, callback):
        size = 10 if tag == 0xA3 else 18
        data = bytearray((i * 29 + 7) & 255 for i in range(size))
        data[0], data[1] = tag, (size - 2) // 8
        data[2:6] = callback.to_bytes(4, "little")
        return bytes(data)

    def test_a3_a4_messages_have_identical_wire_bytes_across_native_tables(self):
        for tag in (0xA3, 0xA4):
            with self.subTest(tag=hex(tag)):
                left = self.message(0x0A04D430, "send", self.packet(tag, 0x0A04D530))
                right = self.message(0x71000001, "send", self.packet(tag, 0x71000101))
                self.assertEqual(left, right)
                self.assertEqual(left, (1, self.packet(tag, 0x0803E5D9)))

    def test_a3_a4_receive_uses_own_slid_native_function_not_sender_pc(self):
        for tag in (0xA3, 0xA4):
            with self.subTest(tag=hex(tag)):
                accepted, wire = self.message(0x21000000, "send", self.packet(tag, 0x21000100))
                self.assertEqual(accepted, 1)
                self.assertEqual(self.message(0x71000001, "receive", wire),
                                 (1, self.packet(tag, 0x71000101)))

    def test_message_conversion_preserves_args_padding_and_exact_arm_thumb_identity(self):
        for tag in (0xA3, 0xA4):
            for offset, gba in ((0x300, 0x08000100), (0x100, 0x0803E5D9), (0x200, 0x080FFFF1)):
                with self.subTest(tag=hex(tag), gba=hex(gba)):
                    original = self.packet(tag, 0x61000001 + offset)
                    accepted, wire = self.message(0x61000001, "send", original)
                    self.assertEqual(accepted, 1)
                    self.assertEqual(wire[:2] + wire[6:], original[:2] + original[6:])
                    self.assertEqual(wire[2:6], gba.to_bytes(4, "little"))
                    self.assertEqual(self.message(0x61000001, "receive", wire), (1, original))

    def test_malformed_message_lengths_and_chunk_counts_reject_without_mutation(self):
        for tag in (0xA3, 0xA4):
            for direction, callback in (("send", 0x61000100), ("receive", 0x0803E5D9)):
                valid = self.packet(tag, callback)
                for length in range(21):
                    if length == len(valid):
                        continue
                    malformed = (valid + bytes(21))[:length]
                    with self.subTest(tag=hex(tag), direction=direction, length=length):
                        self.assertEqual(self.message(0x61000000, direction, malformed), (0, malformed))
                for chunks in (0, 3, 255, 2 if tag == 0xA3 else 1):
                    malformed = valid[:1] + bytes([chunks]) + valid[2:]
                    with self.subTest(tag=hex(tag), direction=direction, chunks=chunks):
                        self.assertEqual(self.message(0x61000000, direction, malformed), (0, malformed))

    def test_unknown_callback_rejects_before_mutating_any_message_bytes(self):
        for tag in (0xA3, 0xA4):
            for direction, callback in (("send", 0x0A04D530), ("send", 0),
                                        ("receive", 0x0A04D530), ("receive", 0x0803E5D8)):
                malformed = self.packet(tag, callback)
                with self.subTest(tag=hex(tag), direction=direction, callback=hex(callback)):
                    self.assertEqual(self.message(0x61000000, direction, malformed), (0, malformed))

    def test_noncallback_messages_are_unchanged_even_when_payload_looks_like_pointer(self):
        for tag in (0, 0xA2, 0xA5, 0xFF):
            message = bytes([tag, 1]) + (0x0A04D530).to_bytes(4, "little") + b"\xff\x80\x01\x00"
            for direction in ("send", "receive"):
                with self.subTest(tag=hex(tag), direction=direction):
                    self.assertEqual(self.message(0x61000000, direction, message), (1, message))


if __name__ == "__main__":
    unittest.main()
