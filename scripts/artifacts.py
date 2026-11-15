#!/usr/bin/env python3
"""Validate and publish the three-file deployment bundle; no third-party tools."""

import hashlib
from pathlib import Path
import re
import shutil
import struct
import sys
import tempfile


PROFILES = ("release", "debug", "noopt")
ARTIFACTS = (
    ("EfiMirageDxe.efi", "efi/{profile}/EfiMirageDxe.efi", 12),
    ("Loader.efi", "efi/{profile}/Loader.efi", 10),
    ("EfiDSEFix.exe", "windows/{profile}/console/EfiDSEFix.exe", 3),
)
NAMES = {name for name, _, _ in ARTIFACTS}


class ArtifactError(Exception):
    """An input or bundle does not satisfy the deployment contract."""


def validate_pe(path, subsystem):
    """Check x64 PE32+ headers, section bounds, and EFI 4 KiB alignment."""
    path = Path(path)

    def require(condition, message):
        if not condition:
            raise ArtifactError(f"{path}: {message}")

    with path.open("rb") as image:
        size = image.seek(0, 2)

        def read_at(offset, length):
            require(0 <= offset <= size and length <= size - offset,
                    "truncated PE image (header or section lies outside the file)")
            image.seek(offset)
            data = image.read(length)
            require(len(data) == length, "truncated PE image")
            return data

        dos = read_at(0, 64)
        require(dos[:2] == b"MZ", "invalid DOS signature")
        pe_offset = struct.unpack_from("<I", dos, 60)[0]
        require(pe_offset >= 64, "invalid PE header offset")
        coff = read_at(pe_offset, 24)
        require(coff[:4] == b"PE\0\0", "invalid PE signature")
        machine, section_count = struct.unpack_from("<HH", coff, 4)
        require(machine == 0x8664, "expected x64 machine (0x8664)")
        require(section_count > 0, "PE image has no sections")
        optional_size = struct.unpack_from("<H", coff, 20)[0]
        require(optional_size >= 112, "truncated PE32+ optional header")
        optional = read_at(pe_offset + 24, optional_size)
        require(struct.unpack_from("<H", optional)[0] == 0x20B,
                "expected PE32+ optional-header magic (0x20b)")
        actual_subsystem = struct.unpack_from("<H", optional, 68)[0]
        require(actual_subsystem == subsystem,
                f"expected subsystem {subsystem}, got {actual_subsystem}")
        directory_count = struct.unpack_from("<I", optional, 108)[0]
        require(directory_count <= (optional_size - 112) // 8,
                "truncated PE data-directory table")
        section_alignment, file_alignment = struct.unpack_from("<II", optional, 32)
        image_size, headers_size = struct.unpack_from("<II", optional, 56)
        section_table = pe_offset + 24 + optional_size
        require(section_table + section_count * 40 <= headers_size <= size,
                "invalid or truncated PE headers size")
        require(image_size >= headers_size, "invalid PE image size")
        if subsystem in (10, 12):
            require(section_alignment == file_alignment == 4096,
                    "EFI file and section alignment must both be 4096")
            require(headers_size % 4096 == image_size % 4096 == 0,
                    "EFI header and image sizes must be 4096-byte aligned")

        for index in range(section_count):
            section = read_at(section_table + index * 40, 40)
            virtual_size, address, raw_size, raw_offset = struct.unpack_from(
                "<IIII", section, 8)
            require(address + max(virtual_size, raw_size) <= image_size,
                    f"section {index} lies outside the PE image")
            if raw_size:
                require(headers_size <= raw_offset <= size and raw_size <= size - raw_offset,
                        f"section {index} has truncated or invalid raw data")
            if subsystem in (10, 12):
                require(address % 4096 == raw_size % 4096 == raw_offset % 4096 == 0,
                        f"EFI section {index} must be 4096-byte aligned")


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _check_profile(profile):
    if profile not in PROFILES:
        raise ArtifactError(f"unknown profile {profile!r}; expected release, debug, or noopt")


def _verify_bundle(bundle):
    if bundle.is_symlink() or not bundle.is_dir():
        raise ArtifactError(f"{bundle}: bundle directory is missing or is not a regular directory")
    actual = {path.name for path in bundle.iterdir()}
    expected = NAMES | {"SHA256SUMS"}
    if actual != expected:
        missing = ", ".join(sorted(expected - actual)) or "none"
        unexpected = ", ".join(sorted(actual - expected)) or "none"
        raise ArtifactError(f"{bundle}: wrong file set (missing: {missing}; unexpected: {unexpected})")
    for name in expected:
        path = bundle / name
        if path.is_symlink() or not path.is_file():
            raise ArtifactError(f"{path}: expected a regular file, not a symlink or directory")

    manifest = bundle / "SHA256SUMS"
    max_length = sum(67 + len(name) for name in NAMES)
    with manifest.open(encoding="ascii") as source:
        text = source.read(max_length + 1)
    if len(text) > max_length:
        raise ArtifactError(f"{manifest}: invalid checksum manifest")
    checksums = {}
    for line in text.splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        if not match or match[2] not in NAMES or match[2] in checksums:
            raise ArtifactError(f"{manifest}: invalid or duplicate checksum entry")
        checksums[match[2]] = match[1]
    if set(checksums) != NAMES:
        raise ArtifactError(f"{manifest}: missing checksum entries")
    for name, _, subsystem in ARTIFACTS:
        path = bundle / name
        print(f"Verifying {path}", file=sys.stderr)
        validate_pe(path, subsystem)
        if sha256(path) != checksums[name]:
            raise ArtifactError(f"{path}: SHA-256 checksum mismatch")


def verify(root, profile):
    """Validate the complete published bundle for one profile."""
    _check_profile(profile)
    bundle = Path(root) / "dist" / profile
    _verify_bundle(bundle)
    return bundle


def stage(root, profile):
    """Publish a validated bundle using two renames with rollback, not an atomic swap."""
    _check_profile(profile)
    root = Path(root)
    sources = [(name, root / "out" / source.format(profile=profile), subsystem)
               for name, source, subsystem in ARTIFACTS]
    for _, source, subsystem in sources:
        validate_pe(source, subsystem)

    dist = root / "dist"
    dist.mkdir(parents=True, exist_ok=True)
    bundle = dist / profile
    if bundle.is_symlink() or (bundle.exists() and not bundle.is_dir()):
        raise ArtifactError(f"{bundle}: refusing to replace a non-directory bundle")
    staging = Path(tempfile.mkdtemp(prefix=f".{profile}-stage-", dir=dist))
    backup = None
    try:
        for name, source, _ in sources:
            shutil.copyfile(source, staging / name)
        manifest = "".join(f"{sha256(staging / name)}  {name}\n" for name, _, _ in ARTIFACTS)
        (staging / "SHA256SUMS").write_text(manifest, encoding="ascii")
        _verify_bundle(staging)
        if bundle.exists():
            backup = Path(tempfile.mkdtemp(prefix=f".{profile}-previous-", dir=dist))
            backup.rmdir()
            bundle.rename(backup)
        try:
            staging.rename(bundle)
        except OSError as publish_error:
            if backup is not None:
                try:
                    backup.rename(bundle)
                except OSError as rollback_error:
                    raise ArtifactError(
                        f"publication failed: {publish_error}; rollback failed: {rollback_error}; "
                        f"previous bundle retained at {backup}"
                    ) from publish_error
            raise
        if backup is not None:
            try:
                shutil.rmtree(backup)
            except OSError as error:
                # Publication is committed; report cleanup failure without pretending it rolled back.
                print(f"warning: bundle published, but cannot remove {backup}: {error}", file=sys.stderr)
        return bundle
    finally:
        if staging.exists():
            shutil.rmtree(staging)


def main(argv=None, root=None):
    args = sys.argv[1:] if argv is None else argv
    root = Path(__file__).resolve().parent.parent if root is None else Path(root)
    try:
        if len(args) != 2 or args[0] not in ("stage", "verify"):
            raise ArtifactError("usage: artifacts.py {stage,verify} {release,debug,noopt}")
        if args[0] == "stage":
            print(stage(root, args[1]))
        else:
            verify(root, args[1])
        return 0
    except (ArtifactError, OSError, UnicodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
