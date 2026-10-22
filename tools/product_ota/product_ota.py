#!/usr/bin/env python3
"""Build and verify the PRDOTA01 package used by PRODUCT_S3_OTA.

The format is intentionally independent of C structure packing. Production is
signed by default. The development exception is always an explicit CLI option.
"""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
from typing import BinaryIO
import zlib


MAGIC = b"PRDOTA01"
FORMAT_VERSION = 1
HEADER_LENGTH = 128
SIGNATURE_LENGTH = 64
SIGN_DOMAIN = b"PRDOTA-SIGN-v1"
LAYOUT_ID = 2
TARGET_ID = "PR-S3-N16R8-01"
NATIVE_LIMIT = 0x1A0000
MANAGED_LIMIT = 0x2E0000
FLASH_SIZE = 0x1000000
UINT32_MAX = 0xFFFFFFFF
IO_CHUNK = 65536
NATIVE_DESCRIPTOR_LENGTH = 72

PARTITIONS = (
    ("nvs", "data", "nvs", 0x9000, 0x6000),
    ("phy_init", "data", "phy", 0xF000, 0x1000),
    ("ota_0", "app", "ota_0", 0x10000, NATIVE_LIMIT),
    ("ota_1", "app", "ota_1", 0x1B0000, NATIVE_LIMIT),
    ("deploy_a", "data", "0x84", 0x350000, MANAGED_LIMIT),
    ("deploy_b", "data", "0x84", 0x630000, MANAGED_LIMIT),
    ("otadata", "data", "ota", 0x910000, 0x2000),
    ("config", "data", "littlefs", 0x912000, 0x6EE000),
)


class PackageError(ValueError):
    """A package or release input violates the product contract."""


def identifier_bytes(value: str, field: str) -> bytes:
    try:
        encoded = value.encode("ascii")
    except UnicodeEncodeError as exc:
        raise PackageError(f"{field} must be ASCII") from exc
    if not 1 <= len(encoded) <= 16 or any(c < 0x21 or c > 0x7E for c in encoded):
        raise PackageError(f"{field} must contain 1..16 printable non-space ASCII bytes")
    return encoded.ljust(16, b"\0")


def decode_identifier(value: bytes, field: str) -> str:
    unpadded = value.rstrip(b"\0")
    try:
        text = unpadded.decode("ascii")
    except UnicodeDecodeError as exc:
        raise PackageError(f"{field} must be ASCII") from exc
    if identifier_bytes(text, field) != value:
        raise PackageError(f"{field} contains embedded NUL or nonzero padding")
    return text


def check_length(length: int, limit: int, name: str) -> None:
    if not 0 < length <= limit:
        raise PackageError(f"{name} length {length} must be in 1..{limit} (0x{limit:X})")


def file_identity(path: Path, limit: int | None = None, name: str = "file") -> dict:
    digest = hashlib.sha256()
    crc = 0
    length = 0
    with path.open("rb") as source:
        while block := source.read(IO_CHUNK):
            length += len(block)
            digest.update(block)
            crc = zlib.crc32(block, crc)
    if limit is not None:
        check_length(length, limit, name)
    return {"length": length, "sha256": digest.hexdigest(), "crc32": f"{crc & UINT32_MAX:08X}"}


@dataclass(frozen=True)
class Header:
    flags: int
    total_length: int
    runtime_length: int
    managed_length: int
    layout_id: int
    target_id: str
    release_id: str
    runtime_sha256: bytes
    managed_sha256: bytes

    def encode(self) -> bytes:
        fields = (
            MAGIC,
            struct.pack("<H", FORMAT_VERSION),
            struct.pack("<H", HEADER_LENGTH),
            struct.pack("<I", self.flags),
            struct.pack("<I", self.total_length),
            struct.pack("<I", self.runtime_length),
            struct.pack("<I", self.managed_length),
            struct.pack("<I", self.layout_id),
            identifier_bytes(self.target_id, "targetId"),
            identifier_bytes(self.release_id, "releaseId"),
            self.runtime_sha256,
            self.managed_sha256,
        )
        result = b"".join(fields)
        if len(result) != HEADER_LENGTH:
            raise PackageError("invalid SHA256 field length")
        return result

    @staticmethod
    def parse(data: bytes, actual_length: int, require_signature: bool = True) -> "Header":
        if len(data) != HEADER_LENGTH:
            raise PackageError("short header")
        if data[:8] != MAGIC:
            raise PackageError("invalid magic")
        if struct.unpack_from("<H", data, 8)[0] != FORMAT_VERSION:
            raise PackageError("unsupported formatVersion")
        if struct.unpack_from("<H", data, 10)[0] != HEADER_LENGTH:
            raise PackageError("invalid headerLength")
        flags, total, runtime, managed, layout = struct.unpack_from("<IIIII", data, 12)
        if flags & ~1:
            raise PackageError("unrecognized flags")
        if require_signature and flags != 1:
            raise PackageError("production policy requires a signature")

        # Python integers do not overflow. Keep the explicit uint32 bound so a
        # native parser cannot accidentally accept a wrapped small total.
        expected = HEADER_LENGTH + runtime + managed + (SIGNATURE_LENGTH if flags & 1 else 0)
        if expected > UINT32_MAX:
            raise PackageError("totalLength overflows uint32")
        if total != expected or actual_length != expected:
            raise PackageError("totalLength does not exactly match payloads and file")
        check_length(runtime, NATIVE_LIMIT, "native image")
        check_length(managed, MANAGED_LIMIT, "managed image")
        if layout != LAYOUT_ID:
            raise PackageError(f"layoutId must be {LAYOUT_ID}")
        target = decode_identifier(data[32:48], "targetId")
        if target != TARGET_ID:
            raise PackageError(f"targetId must be {TARGET_ID}")
        release = decode_identifier(data[48:64], "releaseId")
        return Header(flags, total, runtime, managed, layout, target, release, data[64:96], data[96:128])


def crypto_modules():
    try:
        from cryptography.exceptions import InvalidSignature
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import ec, utils
    except ImportError as exc:
        raise PackageError("install tools/product_ota/requirements.txt for signing/verification") from exc
    return InvalidSignature, hashes, serialization, ec, utils


def load_public_key(path: Path):
    _, _, serialization, ec, _ = crypto_modules()
    try:
        key = serialization.load_pem_public_key(path.read_bytes())
    except (ValueError, TypeError) as exc:
        raise PackageError("public key must be a PEM SubjectPublicKeyInfo public key") from exc
    if not isinstance(key, ec.EllipticCurvePublicKey) or not isinstance(key.curve, ec.SECP256R1):
        raise PackageError("public key must use NIST P-256")
    return key


def load_private_key(path: Path):
    _, _, serialization, ec, _ = crypto_modules()
    try:
        key = serialization.load_pem_private_key(path.read_bytes(), password=None)
    except (ValueError, TypeError) as exc:
        raise PackageError("private key must be an unencrypted PEM P-256 signing key") from exc
    if not isinstance(key, ec.EllipticCurvePrivateKey) or not isinstance(key.curve, ec.SECP256R1):
        raise PackageError("private key must use NIST P-256")
    return key


def sign_header(header: bytes, private_key) -> bytes:
    _, hashes, _, ec, utils = crypto_modules()
    der = private_key.sign(SIGN_DOMAIN + header, ec.ECDSA(hashes.SHA256()))
    r, s = utils.decode_dss_signature(der)
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


def verify_signature(header: bytes, signature: bytes, public_key) -> None:
    InvalidSignature, hashes, _, ec, utils = crypto_modules()
    if len(signature) != SIGNATURE_LENGTH:
        raise PackageError("short signature")
    der = utils.encode_dss_signature(int.from_bytes(signature[:32], "big"), int.from_bytes(signature[32:], "big"))
    try:
        public_key.verify(der, SIGN_DOMAIN + header, ec.ECDSA(hashes.SHA256()))
    except (InvalidSignature, ValueError) as exc:
        raise PackageError("signature verification failed") from exc


def hash_payload(source: BinaryIO, length: int) -> bytes:
    digest = hashlib.sha256()
    remaining = length
    while remaining:
        data = source.read(min(IO_CHUNK, remaining))
        if not data:
            raise PackageError("short payload")
        remaining -= len(data)
        digest.update(data)
    return digest.digest()


def verify_runtime_descriptor(runtime: bytes, header: Header) -> None:
    """Bind the package to the descriptor the native runtime uses at boot.

    ProductReleaseDescriptor is 72 bytes on ESP32: target[16], uint32 layout,
    release[16], uint32 managed length, SHA256[32]. Its existing target/layout
    prefix identifies the constant without changing the package/image format.
    Ambiguous images are rejected instead of choosing a convenient descriptor.
    """
    marker = identifier_bytes(header.target_id, "targetId") + struct.pack("<I", header.layout_id)
    offset = runtime.find(marker)
    if offset < 0:
        raise PackageError("native release descriptor missing or incompatible targetId/layoutId; rebuild the native image")
    if runtime.find(marker, offset + 1) >= 0:
        raise PackageError("ambiguous native release descriptors: targetId/layoutId occurs more than once")
    if offset + NATIVE_DESCRIPTOR_LENGTH > len(runtime):
        raise PackageError("truncated native release descriptor")
    release = decode_identifier(runtime[offset + 20:offset + 36], "native release descriptor releaseId")
    if release != header.release_id:
        raise PackageError(f"native release descriptor releaseId {release!r} does not match package releaseId "
                           f"{header.release_id!r}; rebuild the native image for this release")
    managed_length = struct.unpack_from("<I", runtime, offset + 36)[0]
    if managed_length != header.managed_length:
        raise PackageError("native release descriptor managedLength does not match the managed payload; rebuild the native image")
    if runtime[offset + 40:offset + 72] != header.managed_sha256:
        raise PackageError("native release descriptor managed SHA256 does not match the managed payload; rebuild the native image")


def verify_package(path: Path, public_key=None, *, allow_unsigned: bool = False, expected_crc: int | None = None) -> dict:
    identity = file_identity(path)
    if expected_crc is not None and int(identity["crc32"], 16) != expected_crc:
        raise PackageError("complete-file CRC32 mismatch")
    with path.open("rb") as source:
        raw_header = source.read(HEADER_LENGTH)
        header = Header.parse(raw_header, identity["length"], require_signature=not allow_unsigned)
        if hash_payload(source, header.runtime_length) != header.runtime_sha256:
            raise PackageError("native payload SHA256 mismatch")
        if hash_payload(source, header.managed_length) != header.managed_sha256:
            raise PackageError("managed payload SHA256 mismatch")
        if header.flags & 1:
            if public_key is None:
                raise PackageError("a trusted public key is required for signed packages")
            verify_signature(raw_header, source.read(SIGNATURE_LENGTH), public_key)
        if source.read(1):
            raise PackageError("unexpected trailing bytes")
        source.seek(HEADER_LENGTH)
        verify_runtime_descriptor(source.read(header.runtime_length), header)
    # A second identity also detects most accidental concurrent changes between
    # the CRC pass and parse pass. Release inputs must remain immutable.
    if file_identity(path) != identity:
        raise PackageError("package changed while verifying")
    return {
        "format": MAGIC.decode("ascii"),
        "format_version": FORMAT_VERSION,
        "layout_id": header.layout_id,
        "target_id": header.target_id,
        "release_id": header.release_id,
        "signed": bool(header.flags & 1),
        "package": identity,
        "runtime": {"length": header.runtime_length, "sha256": header.runtime_sha256.hex()},
        "managed": {"length": header.managed_length, "sha256": header.managed_sha256.hex()},
    }


def write_json(path: Path, value: dict) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def build_package(runtime: Path, managed: Path, release_id: str, output: Path, private_key=None,
                  *, allow_unsigned: bool = False, provenance: dict | None = None) -> dict:
    identifier_bytes(release_id, "releaseId")
    if private_key is None and not allow_unsigned:
        raise PackageError("production packaging requires --private-key")
    for source in (runtime, managed):
        if source.resolve() == output.resolve():
            raise PackageError("package output cannot replace a payload")
    native = file_identity(runtime, NATIVE_LIMIT, "native image")
    application = file_identity(managed, MANAGED_LIMIT, "managed image")
    signed = private_key is not None
    fields = Header(
        int(signed), HEADER_LENGTH + native["length"] + application["length"] + (SIGNATURE_LENGTH if signed else 0),
        native["length"], application["length"], LAYOUT_ID, TARGET_ID, release_id,
        bytes.fromhex(native["sha256"]), bytes.fromhex(application["sha256"]),
    )
    with runtime.open("rb") as source:
        verify_runtime_descriptor(source.read(NATIVE_LIMIT + 1), fields)
    header = fields.encode()
    signature = sign_header(header, private_key) if signed else b""
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(prefix=output.name + ".", suffix=".tmp", dir=output.parent, delete=False) as destination:
            temporary = Path(destination.name)
            destination.write(header)
            for payload in (runtime, managed):
                with payload.open("rb") as source:
                    while block := source.read(IO_CHUNK):
                        destination.write(block)
            destination.write(signature)
        report = verify_package(temporary, private_key.public_key() if signed else None, allow_unsigned=allow_unsigned)
        temporary.replace(output)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    report["provenance"] = provenance or {}
    report["provenance_complete"] = all(report["provenance"].get(name) for name in (
        "fork_commit", "idf_tag", "idf_commit", "build_container", "compiler_version",
        "managed_packages", "bootloader_sha256", "partition_table_sha256",
    ))
    write_json(output.with_name(output.name + ".manifest.json"), report)
    return report


def c_bytes(data: bytes) -> str:
    return "{ " + ", ".join(f"0x{byte:02X}" for byte in data) + " }"


def generate_descriptor(managed: Path, release_id: str, output: Path, public_key=None, *, allow_unsigned=False) -> dict:
    target = identifier_bytes(TARGET_ID, "targetId")
    release = identifier_bytes(release_id, "releaseId")
    identity = file_identity(managed, MANAGED_LIMIT, "managed image")
    if public_key is None and not allow_unsigned:
        raise PackageError("production descriptor generation requires --public-key")
    key_bytes = b""
    if public_key is not None:
        _, _, serialization, _, _ = crypto_modules()
        key_bytes = public_key.public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
    source = (
        "// Generated by tools/product_ota/product_ota.py; do not edit.\n"
        '#include "ProductFirmwareUpdate.h"\n\n'
        "const ProductReleaseDescriptor g_ProductReleaseDescriptor = {\n"
        f"    {c_bytes(target)},\n"
        f"    {LAYOUT_ID}u,\n"
        f"    {c_bytes(release)},\n"
        f"    {identity['length']}u,\n"
        f"    {c_bytes(bytes.fromhex(identity['sha256']))}\n"
        "};\n\n"
        # ISO C/C++ does not allow zero-size arrays. Length zero signals the
        # absence of a pinned key in the explicit unsigned development build.
        f"const uint8_t g_ProductOtaPublicKey[] = {c_bytes(key_bytes or bytes([0]))};\n"
        f"const size_t g_ProductOtaPublicKeyLength = {len(key_bytes)}u;\n"
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(source, encoding="utf-8", newline="\n")
    report = {"target_id": TARGET_ID, "layout_id": LAYOUT_ID, "release_id": release_id,
              "managed": identity, "public_key_sec1": key_bytes.hex(), "development_unsigned": public_key is None}
    write_json(output.with_name(output.name + ".json"), report)
    return report


def parse_size(value: str) -> int:
    value = value.strip()
    multiplier = 1
    if value[-1:].upper() in ("K", "M"):
        multiplier = 1024 if value[-1:].upper() == "K" else 1024 * 1024
        value = value[:-1]
    try:
        return int(value, 0) * multiplier
    except ValueError as exc:
        raise PackageError("partition offsets and sizes must be explicit numbers") from exc


def check_layout(path: Path) -> dict:
    actual = []
    with path.open(encoding="utf-8-sig", newline="") as source:
        for row in csv.reader(source):
            if not row or not row[0].strip() or row[0].lstrip().startswith("#"):
                continue
            row = [field.strip() for field in row]
            if len(row) not in (5, 6) or (len(row) == 6 and row[5]):
                raise PackageError("partition rows must contain five fields and no flags")
            actual.append((row[0], row[1], row[2].lower(), parse_size(row[3]), parse_size(row[4])))
    if tuple(actual) != PARTITIONS:
        raise PackageError("partition CSV does not exactly match product layoutId 2")
    previous_end = 0
    for label, kind, _, address, size in actual:
        alignment = 0x10000 if kind == "app" else 0x1000
        if address % alignment or size % 0x1000 or address < previous_end:
            raise PackageError(f"partition {label} overlaps or is misaligned")
        previous_end = address + size
    if previous_end != FLASH_SIZE:
        raise PackageError("product partitions must end at 16 MiB")
    return {"layout_id": LAYOUT_ID, "flash_size": FLASH_SIZE, "partition_count": len(actual),
            "csv": file_identity(path)}


def crc_argument(value: str) -> int:
    try:
        # Protocol/manifest CRCs use hex; a prefix is optional.
        crc = int(value, 16)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("CRC32 must be hexadecimal") from exc
    if not 0 <= crc <= UINT32_MAX:
        raise argparse.ArgumentTypeError("CRC32 must fit uint32")
    return crc


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    descriptor = commands.add_parser("descriptor", help="generate native descriptor and pinned public key source")
    descriptor.add_argument("--managed", type=Path, required=True)
    descriptor.add_argument("--release-id", required=True)
    descriptor.add_argument("--output", type=Path, required=True)
    descriptor.add_argument("--public-key", type=Path)
    descriptor.add_argument("--allow-unsigned-development", action="store_true")
    package = commands.add_parser("package", help="package final native and managed payloads")
    package.add_argument("--runtime", type=Path, required=True)
    package.add_argument("--managed", type=Path, required=True)
    package.add_argument("--release-id", required=True)
    package.add_argument("--output", type=Path, required=True)
    signing = package.add_mutually_exclusive_group()
    signing.add_argument("--private-key", type=Path)
    signing.add_argument("--allow-unsigned-development", action="store_true")
    package.add_argument("--provenance", type=Path, help="JSON containing pinned build and provisioning identities")
    verify = commands.add_parser("verify", help="verify exact package structure, hashes, signature and optional CRC")
    verify.add_argument("--package", type=Path, required=True)
    verify.add_argument("--public-key", type=Path)
    verify.add_argument("--allow-unsigned-development", action="store_true")
    verify.add_argument("--expected-crc", type=crc_argument)
    runtime = commands.add_parser("check-runtime", help="fail if final native image exceeds its partition")
    runtime.add_argument("--runtime", type=Path, required=True)
    layout = commands.add_parser("check-layout", help="require exact product layoutId 2 CSV")
    layout.add_argument("--csv", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "descriptor":
            report = generate_descriptor(args.managed, args.release_id, args.output,
                                         load_public_key(args.public_key) if args.public_key else None,
                                         allow_unsigned=args.allow_unsigned_development)
        elif args.command == "package":
            provenance = json.loads(args.provenance.read_text(encoding="utf-8")) if args.provenance else None
            if provenance is not None and not isinstance(provenance, dict):
                raise PackageError("provenance must be a JSON object")
            report = build_package(args.runtime, args.managed, args.release_id, args.output,
                                   load_private_key(args.private_key) if args.private_key else None,
                                   allow_unsigned=args.allow_unsigned_development, provenance=provenance)
        elif args.command == "verify":
            report = verify_package(args.package, load_public_key(args.public_key) if args.public_key else None,
                                    allow_unsigned=args.allow_unsigned_development, expected_crc=args.expected_crc)
        elif args.command == "check-runtime":
            report = file_identity(args.runtime, NATIVE_LIMIT, "native image")
        else:
            report = check_layout(args.csv)
        print(json.dumps(report, indent=2, sort_keys=True))
        return 0
    except (PackageError, OSError, ValueError) as exc:
        print(f"product-ota: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
