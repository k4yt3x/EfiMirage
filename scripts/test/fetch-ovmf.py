#!/usr/bin/env python3
"""Prepare a private QEMU + OVMF under .tooling/ without root.

The agent host has no /dev/kvm and no system QEMU, and pacman needs root, so the
packages are downloaded from the Arch mirror and unpacked into .tooling/root. The
firmware is only used by scripts/test/test-secureboot.sh.

    python3 scripts/test/fetch-ovmf.py

Idempotent: already downloaded packages are not fetched again. Re-run after a
QEMU version bump by deleting .tooling/.
"""
import hashlib
import os
import subprocess
import sys
import tarfile

MIRROR = "https://fastly.mirror.pkgbuild.com"
ARCH = "x86_64"
REPOS = ("core", "extra")
TARGETS = ("qemu-system-x86", "edk2-ovmf")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOLING = os.path.join(ROOT, ".tooling")
PKGS = os.path.join(TOOLING, "pkgs")
PREFIX = os.path.join(TOOLING, "root")


def fetch(url, path, sha256=""):
    if not os.path.exists(path):
        subprocess.run(["curl", "-fsSL", "--retry", "3", "-o", path + ".part", url], check=True)
        os.replace(path + ".part", path)
    if sha256:
        digest = hashlib.sha256(open(path, "rb").read()).hexdigest()
        if digest != sha256:
            os.unlink(path)
            sys.exit(f"checksum mismatch for {os.path.basename(path)}: {digest} != {sha256}")


def load_index():
    index = {}
    for repo in REPOS:
        db = os.path.join(TOOLING, repo + ".db")
        fetch(f"{MIRROR}/{repo}/os/{ARCH}/{repo}.db", db)
        with tarfile.open(db) as tar:
            for member in tar:
                if not member.name.endswith("/desc"):
                    continue
                fields = {}
                for block in tar.extractfile(member).read().decode().split("\n\n"):
                    lines = [line for line in block.splitlines() if line]
                    if lines:
                        fields[lines[0].strip("%")] = lines[1:]
                if "NAME" in fields:
                    index[fields["NAME"][0]] = {
                        "repo": repo,
                        "filename": fields["FILENAME"][0],
                        "sha256": fields.get("SHA256SUM", [""])[0],
                        "depends": fields.get("DEPENDS", []),
                        "provides": {p.split("=")[0] for p in fields.get("PROVIDES", [])},
                    }
    return index


def main():
    if not os.path.exists("/etc/arch-release"):
        sys.exit("this script assumes an Arch host with matching package versions")
    os.makedirs(PKGS, exist_ok=True)
    index = load_index()
    installed = set(subprocess.run(["pacman", "-Qq"], capture_output=True, text=True).stdout.split())
    providers = {prov: name for name, rec in index.items() for prov in rec["provides"]}

    resolved = {}
    queue = list(TARGETS)
    while queue:
        dep = queue.pop()
        base = dep.split("=")[0].split(">")[0].split("<")[0]
        # Always resolve the targets themselves: their files are what gets extracted, even on a
        # host that already has the packages installed.
        if base in resolved or (base in installed and base not in TARGETS):
            continue
        name = base if base in index else providers.get(base)
        if name is None:
            sys.exit(f"unresolved dependency: {dep}")
        if name in resolved:
            continue
        resolved[name] = index[name]
        queue.extend(index[name]["depends"])

    for name in sorted(resolved):
        record = resolved[name]
        print(f"fetch {name}")
        fetch(f"{MIRROR}/{record['repo']}/os/{ARCH}/{record['filename']}",
              os.path.join(PKGS, record["filename"]), record["sha256"])

    os.makedirs(PREFIX, exist_ok=True)
    for record in resolved.values():
        subprocess.run(["bsdtar", "-xf", os.path.join(PKGS, record["filename"]), "-C", PREFIX],
                       check=True, stderr=subprocess.DEVNULL)

    for path in ("usr/bin/qemu-system-x86_64", "usr/share/edk2/x64/OVMF_CODE.secboot.4m.fd"):
        if not os.path.exists(os.path.join(PREFIX, path)):
            sys.exit(f"missing after extraction: {path}")
    print(f"ready: {PREFIX}")


if __name__ == "__main__":
    main()
