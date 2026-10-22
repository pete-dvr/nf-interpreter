"""Contract tests; temporary P-256 keys are generated only inside test memory."""

import hashlib
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

import product_ota as ota


class PackageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        _, _, _, ec, _ = ota.crypto_modules()
        cls.key = ec.generate_private_key(ec.SECP256R1())
        cls.wrong_key = ec.generate_private_key(ec.SECP256R1())

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.runtime = self.root / "nanoCLR.bin"
        self.managed = self.root / "application.bin"
        self.output = self.root / "update.bin"
        # Include every separator and bytes that are not valid text.
        self.managed.write_bytes(b"MSSpot1\0" + bytes(reversed(range(256))) * 7)
        self.descriptor_offset = 1 + 256 * 17
        self.runtime.write_bytes(b"\xE9" + bytes(range(256)) * 17 + self.descriptor())

    def tearDown(self):
        self.temp.cleanup()

    def build(self, signed=True):
        return ota.build_package(self.runtime, self.managed, "v1.2.3", self.output,
                                 self.key if signed else None, allow_unsigned=not signed)

    def descriptor(self, release="v1.2.3"):
        # Serialize the actual C struct independently of the package builder.
        return struct.pack("<16sI16sI32s", ota.TARGET_ID.encode(), 2, release.encode(),
                           self.managed.stat().st_size, hashlib.sha256(self.managed.read_bytes()).digest())

    def write_external_package(self, runtime, signed):
        # Model a package made elsewhere: all outer hashes/signature are valid,
        # so descriptor disagreement must be caught by verification itself.
        managed = self.managed.read_bytes()
        header = ota.Header(int(signed), 128 + len(runtime) + len(managed) + (64 if signed else 0),
                            len(runtime), len(managed), 2, ota.TARGET_ID, "v1.2.3",
                            hashlib.sha256(runtime).digest(), hashlib.sha256(managed).digest()).encode()
        signature = ota.sign_header(header, self.key) if signed else b""
        self.output.write_bytes(header + runtime + managed + signature)

    def mutate(self, offset, data):
        package = bytearray(self.output.read_bytes())
        package[offset:offset + len(data)] = data
        self.output.write_bytes(package)

    def verify(self, **kwargs):
        return ota.verify_package(self.output, self.key.public_key(), **kwargs)

    def test_crc_iso_hdlc_vector_and_chunking(self):
        self.assertEqual(zlib.crc32(b"123456789"), 0xCBF43926)
        self.assertEqual(zlib.crc32(b"56789", zlib.crc32(b"1234")), 0xCBF43926)

    def test_signed_round_trip_and_exact_layout(self):
        manifest = self.build()
        data = self.output.read_bytes()
        self.assertEqual(data[:8], b"PRDOTA01")
        self.assertEqual(struct.unpack_from("<HHI", data, 8), (1, 128, 1))
        self.assertEqual(data[32:48], b"PR-S3-N16R8-01\0\0")
        self.assertEqual(data[48:64], b"v1.2.3" + b"\0" * 10)
        self.assertEqual(data[64:96], hashlib.sha256(self.runtime.read_bytes()).digest())
        self.assertEqual(data[96:128], hashlib.sha256(self.managed.read_bytes()).digest())
        self.assertEqual(data[128:128 + self.runtime.stat().st_size], self.runtime.read_bytes())
        result = self.verify(expected_crc=int(manifest["package"]["crc32"], 16))
        self.assertEqual(result["release_id"], "v1.2.3")
        self.assertEqual(result["package"]["length"], 128 + self.runtime.stat().st_size + self.managed.stat().st_size + 64)
        self.assertFalse(manifest["provenance_complete"])
        self.assertEqual(json.loads(self.output.with_name("update.bin.manifest.json").read_text()), manifest)

    def test_signature_is_domain_separated_raw_rs(self):
        self.build()
        data = self.output.read_bytes()
        InvalidSignature, hashes, _, ec, utils = ota.crypto_modules()
        signature = data[-64:]
        der = utils.encode_dss_signature(int.from_bytes(signature[:32], "big"), int.from_bytes(signature[32:], "big"))
        self.key.public_key().verify(der, b"PRDOTA-SIGN-v1" + data[:128], ec.ECDSA(hashes.SHA256()))
        with self.assertRaises(InvalidSignature):
            self.key.public_key().verify(der, data[:128], ec.ECDSA(hashes.SHA256()))

    def test_production_rejects_unsigned_even_when_structurally_valid(self):
        self.build(False)
        with self.assertRaisesRegex(ota.PackageError, "requires a signature"):
            self.verify()
        self.assertFalse(self.verify(allow_unsigned=True)["signed"])

    def test_missing_key_and_wrong_key_rejected_even_development(self):
        self.build()
        with self.assertRaisesRegex(ota.PackageError, "trusted public key"):
            ota.verify_package(self.output, allow_unsigned=True)
        with self.assertRaisesRegex(ota.PackageError, "signature verification"):
            ota.verify_package(self.output, self.wrong_key.public_key(), allow_unsigned=True)

    def test_no_implicit_development_packaging(self):
        with self.assertRaisesRegex(ota.PackageError, "requires --private-key"):
            ota.build_package(self.runtime, self.managed, "v1", self.output)
        self.assertFalse(self.output.exists())

    def test_crc_covers_signature_trailer(self):
        report = self.build()
        old_crc = int(report["package"]["crc32"], 16)
        data = self.output.read_bytes()
        self.mutate(len(data) - 1, bytes([data[-1] ^ 1]))
        with self.assertRaisesRegex(ota.PackageError, "CRC32"):
            self.verify(expected_crc=old_crc)

    def test_byte_flipped_header_payloads_signature_rejected(self):
        for offset, expected in ((48, "signature"), (128, "native payload"),
                                 (128 + self.runtime.stat().st_size, "managed payload"), (-1, "signature")):
            with self.subTest(offset=offset):
                self.build()
                data = self.output.read_bytes()
                index = offset if offset >= 0 else len(data) + offset
                self.mutate(index, bytes([data[index] ^ 1]))
                with self.assertRaisesRegex(ota.PackageError, expected):
                    self.verify()

    def test_header_fixed_fields_and_unknown_flags(self):
        changes = ((0, b"X", "magic"), (8, b"\x02\0", "formatVersion"),
                   (10, b"\x81\0", "headerLength"), (12, b"\x03\0\0\0", "flags"),
                   (28, struct.pack("<I", 1), "layoutId"), (32, b"X", "targetId"),
                   (48, b"\0ABC", "releaseId"), (48, b"\xFF", "releaseId"),
                   (48, b"\0" * 16, "releaseId"))
        for offset, value, message in changes:
            with self.subTest(offset=offset, value=value):
                self.build()
                self.mutate(offset, value)
                with self.assertRaisesRegex(ota.PackageError, message):
                    self.verify()

    def test_no_truncation_of_identifiers(self):
        for value in ("", "X" * 17, "nul\0id", "with space", "é"):
            with self.subTest(value=value):
                with self.assertRaises(ota.PackageError):
                    ota.identifier_bytes(value, "releaseId")
        self.assertEqual(ota.identifier_bytes("X" * 16, "releaseId"), b"X" * 16)

    def test_empty_and_oversized_payloads_rejected(self):
        for file, limit, expected in ((self.runtime, ota.NATIVE_LIMIT, "native"),
                                      (self.managed, ota.MANAGED_LIMIT, "managed")):
            original = file.read_bytes()
            for length in (0, limit + 1):
                with self.subTest(file=file.name, length=length):
                    with file.open("wb") as stream:
                        stream.truncate(length)
                    with self.assertRaisesRegex(ota.PackageError, expected):
                        self.build()
            file.write_bytes(original)

    def test_uint32_addition_overflow_is_rejected(self):
        self.build()
        data = bytearray(self.output.read_bytes()[:128])
        struct.pack_into("<I", data, 20, 0xFFFFFFF0)
        struct.pack_into("<I", data, 24, 0x100)
        wrapped_total = (128 + 0xFFFFFFF0 + 0x100 + 64) & 0xFFFFFFFF
        struct.pack_into("<I", data, 16, wrapped_total)
        with self.assertRaisesRegex(ota.PackageError, "overflows"):
            ota.Header.parse(bytes(data), wrapped_total)

    def test_short_files_and_trailing_data_rejected(self):
        self.build()
        complete = self.output.read_bytes()
        for data in (b"", complete[:8], complete[:127], complete[:-1], complete + b"\0"):
            with self.subTest(length=len(data)):
                self.output.write_bytes(data)
                with self.assertRaises(ota.PackageError):
                    self.verify()

    def test_signature_flag_cannot_remove_requirement(self):
        self.build()
        original = self.output.read_bytes()
        data = bytearray(original[:-64])
        struct.pack_into("<I", data, 12, 0)
        struct.pack_into("<I", data, 16, len(data))
        self.output.write_bytes(data)
        with self.assertRaisesRegex(ota.PackageError, "requires a signature"):
            self.verify()

    def test_truncated_payload_hash_does_not_accept_short_read(self):
        with self.assertRaisesRegex(ota.PackageError, "short payload"):
            ota.hash_payload(io.BytesIO(b"short"), 6)

    def test_descriptor_and_key_match_native_contract(self):
        path = self.root / "product_release_descriptor.cpp"
        report = ota.generate_descriptor(self.managed, "v1.2.3", path, self.key.public_key())
        text = path.read_text()
        self.assertIn('#include "ProductFirmwareUpdate.h"', text)
        self.assertIn("const ProductReleaseDescriptor g_ProductReleaseDescriptor", text)
        self.assertIn("const size_t g_ProductOtaPublicKeyLength = 65u;", text)
        self.assertIn(ota.c_bytes(hashlib.sha256(self.managed.read_bytes()).digest()), text)
        self.assertEqual(report["managed"]["length"], self.managed.stat().st_size)
        self.assertTrue(report["public_key_sec1"].startswith("04"))
        with self.assertRaisesRegex(ota.PackageError, "requires --public-key"):
            ota.generate_descriptor(self.managed, "v1", path)
        ota.generate_descriptor(self.managed, "v1", path, allow_unsigned=True)
        self.assertIn("g_ProductOtaPublicKeyLength = 0u", path.read_text())

    def test_descriptor_output_cannot_silently_truncate_release(self):
        with self.assertRaises(ota.PackageError):
            ota.generate_descriptor(self.managed, "x" * 17, self.root / "generated.cpp", self.key.public_key())

    def test_packaging_cannot_rename_an_existing_native_release(self):
        self.output.write_bytes(b"existing release artifact")
        with self.assertRaisesRegex(ota.PackageError, "native release descriptor releaseId.*v1.2.3.*v1.2.4"):
            ota.build_package(self.runtime, self.managed, "v1.2.4", self.output, self.key)
        self.assertEqual(self.output.read_bytes(), b"existing release artifact")
        self.assertFalse(self.output.with_name("update.bin.manifest.json").exists())
        self.assertEqual(list(self.root.glob("update.bin.*.tmp")), [])

    def test_descriptor_disagreement_rejected_when_packaging_and_verifying(self):
        original = self.runtime.read_bytes()
        changes = (
            (0, b"WRONG-TARGET\0\0\0\0", "targetId/layoutId"),
            (16, struct.pack("<I", 1), "targetId/layoutId"),
            (20, b"older-release".ljust(16, b"\0"), "releaseId"),
            (20, b"bad\0padding".ljust(16, b"\0"), "releaseId"),
            (36, struct.pack("<I", self.managed.stat().st_size - 1), "managedLength"),
            (40, bytes([original[self.descriptor_offset + 40] ^ 1]), "managed SHA256"),
        )
        for offset, replacement, message in changes:
            for signed in (False, True):
                with self.subTest(field=message, offset=offset, signed=signed):
                    runtime = bytearray(original)
                    start = self.descriptor_offset + offset
                    runtime[start:start + len(replacement)] = replacement
                    self.runtime.write_bytes(runtime)
                    with self.assertRaisesRegex(ota.PackageError, message):
                        self.build(signed)
                    self.write_external_package(runtime, signed)
                    with self.assertRaisesRegex(ota.PackageError, message):
                        self.verify(allow_unsigned=not signed)

    def test_descriptor_missing_truncated_and_ambiguous_rejected(self):
        original = self.runtime.read_bytes()
        bad_images = (
            (original[:self.descriptor_offset], "descriptor missing"),
            (original[:-1], "truncated native release descriptor"),
            (original + self.descriptor(), "ambiguous native release descriptors"),
            (original + self.descriptor("older-release"), "ambiguous native release descriptors"),
        )
        for runtime, message in bad_images:
            with self.subTest(message=message):
                self.runtime.write_bytes(runtime)
                with self.assertRaisesRegex(ota.PackageError, message):
                    self.build()
                self.write_external_package(runtime, True)
                with self.assertRaisesRegex(ota.PackageError, message):
                    self.verify()

    def test_changed_managed_image_requires_matching_native_rebuild(self):
        application = self.managed.read_bytes()
        for replacement, message in ((application + b"extra assembly", "managedLength"),
                                      (bytes([application[0] ^ 1]) + application[1:], "managed SHA256")):
            with self.subTest(message=message):
                self.managed.write_bytes(replacement)
                with self.assertRaisesRegex(ota.PackageError, message):
                    self.build()
        self.runtime.write_bytes(self.runtime.read_bytes()[:self.descriptor_offset] + self.descriptor())
        self.assertEqual(self.build()["managed"]["sha256"], hashlib.sha256(replacement).hexdigest())

    def test_partition_layout_exact_and_corruption(self):
        path = self.root / "partitions.csv"
        rows = ["# Name,Type,SubType,Offset,Size"]
        for name, kind, subtype, address, size in ota.PARTITIONS:
            rows.append(f"{name},{kind},{subtype},0x{address:X},0x{size:X},")
        valid = "\n".join(rows) + "\n"
        path.write_text(valid)
        self.assertEqual(ota.check_layout(path)["partition_count"], 8)
        for invalid in (valid.replace("0x350000", "0x360000"),
                        valid.replace("deploy_a", "deploy"),
                        valid.replace("0x84", "0x85", 1),
                        valid.replace("0x6EE000", "0x6EF000"),
                        valid.replace("0x1A0000", "0x190000", 1),
                        valid + "factory,app,factory,0x10000,0x10000\n"):
            with self.subTest(invalid=invalid):
                path.write_text(invalid)
                with self.assertRaises(ota.PackageError):
                    ota.check_layout(path)

    def test_provenance_completeness_is_explicit(self):
        metadata = {name: "recorded" for name in (
            "fork_commit", "idf_tag", "idf_commit", "build_container", "compiler_version",
            "managed_packages", "bootloader_sha256", "partition_table_sha256",
        )}
        report = ota.build_package(self.runtime, self.managed, "v1.2.3", self.output, self.key, provenance=metadata)
        self.assertTrue(report["provenance_complete"])
        self.assertEqual(report["provenance"], metadata)

    def test_cli_signed_verify_corruption_exit_code(self):
        self.build()
        _, _, serialization, _, _ = ota.crypto_modules()
        public = self.root / "public.pem"
        public.write_bytes(self.key.public_key().public_bytes(serialization.Encoding.PEM,
                                                            serialization.PublicFormat.SubjectPublicKeyInfo))
        command = [sys.executable, str(Path(ota.__file__).resolve()), "verify",
                   "--package", str(self.output), "--public-key", str(public)]
        valid = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(valid.returncode, 0, valid.stderr)
        self.assertTrue(json.loads(valid.stdout)["signed"])
        self.mutate(128, b"X")
        corrupt = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(corrupt.returncode, 2)
        self.assertIn("native payload SHA256 mismatch", corrupt.stderr)
        self.assertEqual(corrupt.stdout, "")


if __name__ == "__main__":
    unittest.main()
