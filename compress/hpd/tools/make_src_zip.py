#!/usr/bin/env python3
"""make_src_zip.py [OUT_DIR] : package the hpd source as dist/hpd-<version>-src.zip.

Takes every file of compress/hpd/ (plus the repository LICENSE) from the git
index, so the zip holds exactly what is committed or staged, with LF line
endings, executable bits kept, a fixed timestamp and no compression (zlib
builds differ in their output, stored entries do not): the same tree gives
the same zip bytes on every platform. Prints the zip path, size and SHA-256.
"""
import hashlib
import os
import re
import subprocess
import sys
import zipfile

ROOT = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True, check=True).stdout.strip()


def git(*args):
    return subprocess.run(["git", "-C", ROOT, *args], capture_output=True, check=True).stdout


src = git("show", ":compress/hpd/src/hpd.cpp").decode()
version = re.search(r'kVersionText = "hpd ([0-9.]+)', src).group(1)
prefix = f"hpd-{version}/"
out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "dist")
os.makedirs(out_dir, exist_ok=True)
out = os.path.join(out_dir, f"hpd-{version}-src.zip")

entries = []  # (mode, path in repo, path in zip)
for line in git("ls-files", "-s", "--", "compress/hpd", "LICENSE").decode().splitlines():
    meta, path = line.split("\t", 1)
    mode = meta.split()[0]
    name = path[len("compress/hpd/"):] if path.startswith("compress/hpd/") else path
    entries.append((mode, path, prefix + name))

with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as z:
    for mode, path, name in sorted(entries, key=lambda e: e[2]):
        info = zipfile.ZipInfo(name, date_time=(2026, 11, 4, 0, 0, 0))
        info.compress_type = zipfile.ZIP_STORED
        info.create_system = 3  # unix, so the permission bits are honoured
        info.external_attr = (0o100755 if mode == "100755" else 0o100644) << 16
        z.writestr(info, git("show", ":" + path))

data = open(out, "rb").read()
print(f"{os.path.relpath(out, ROOT)}  {len(entries)} files  {len(data)} B  sha256 {hashlib.sha256(data).hexdigest()}")
