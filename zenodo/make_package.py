#!/usr/bin/env python3
"""make_package.py : fill zenodo/upload/ with the files for the Zenodo record.

  HP40_hpd_paper.pdf                      the paper (Parts I and II as one), built by
                                          paper/hp40/scripts/build_pdf.sh
  hpd-<version>-src.zip                   the hpd source package (dist/, cited in §18)
  HP40-hpd-paper-materials-<version>.zip  the paper's sources, data, figures, scripts
                                          and patches (paper/hp40/ from the git index),
                                          plus README.md and LICENSE
  SHA256SUMS.txt                          checksums of the three files above

The materials zip is reproducible like the source zip: files come from the git
index, entries are stored uncompressed with a fixed timestamp, so the same tree
gives the same bytes. Run from anywhere after `git add`.
"""
import hashlib
import os
import re
import shutil
import subprocess
import zipfile

ROOT = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True, check=True).stdout.strip()
OUT = os.path.join(ROOT, "zenodo", "upload")


def git(*args):
    return subprocess.run(["git", "-C", ROOT, *args], capture_output=True, check=True).stdout


src = git("show", ":compress/hpd/src/hpd.cpp").decode()
version = re.search(r'kVersionText = "hpd ([0-9.]+)', src).group(1)
os.makedirs(OUT, exist_ok=True)
for f in os.listdir(OUT):
    os.remove(os.path.join(OUT, f))

pdf = os.path.join(ROOT, "paper", "hp40", "HP40_hpd_paper.pdf")
src_zip = os.path.join(ROOT, "dist", "hpd-%s-src.zip" % version)
for p in (pdf, src_zip):
    if not os.path.exists(p):
        raise SystemExit("missing " + p)
shutil.copyfile(pdf, os.path.join(OUT, "HP40_hpd_paper.pdf"))
shutil.copyfile(src_zip, os.path.join(OUT, os.path.basename(src_zip)))

mat = os.path.join(OUT, "HP40-hpd-paper-materials-%s.zip" % version)
prefix = "HP40-hpd-paper-%s/" % version
entries = []
for line in git("ls-files", "-s", "--", "paper/hp40", "README.md", "LICENSE").decode().splitlines():
    meta, path = line.split("\t", 1)
    entries.append((meta.split()[0], path))
with zipfile.ZipFile(mat, "w", zipfile.ZIP_STORED) as z:
    for mode, path in sorted(entries, key=lambda e: e[1]):
        info = zipfile.ZipInfo(prefix + path, date_time=(2026, 11, 5, 0, 0, 0))
        info.compress_type = zipfile.ZIP_STORED
        info.create_system = 3
        info.external_attr = (0o100755 if mode == "100755" else 0o100644) << 16
        z.writestr(info, git("show", ":" + path))

lines = []
for name in sorted(os.listdir(OUT)):
    data = open(os.path.join(OUT, name), "rb").read()
    lines.append("%s  %s" % (hashlib.sha256(data).hexdigest(), name))
    print("%-42s %10d B" % (name, len(data)))
open(os.path.join(OUT, "SHA256SUMS.txt"), "w", newline="\n").write("\n".join(lines) + "\n")
print("\n".join(lines))
print("package ready in", OUT)
