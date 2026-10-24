#!/usr/bin/env python3
"""The compiler the first start builds the game with (src/setup/firstrun.cpp, tools/llvm/ beside the program): clang and lld
from LLVM's own release for Linux x86-64, checked against a pinned SHA-256.

    python3 tools/linux/fetch_llvm.py OUT_DIR          # writes OUT_DIR/clang, OUT_DIR/ld.lld, OUT_DIR/LICENSE.TXT

The release archive is about 2 GB, so it is streamed and only the two programs are kept (clang is the release's clang-20 and
ld.lld is lld, which picks its flavour from the name it is started as). They link LLVM statically and need only glibc 2.34,
libm, libz and, for lld, libxml2.so.2 (present on every desktop distribution; the setup falls back to the system's linker
when it is not). Only the standard library is used. Idempotent: nothing is fetched when OUT_DIR holds the right files."""
import hashlib
import lzma
import os
import shutil
import sys
import tarfile
import urllib.request

VERSION = "20.1.8"
URL = f"https://github.com/llvm/llvm-project/releases/download/llvmorg-{VERSION}/LLVM-{VERSION}-Linux-X64.tar.xz"
SHA256 = "1ead36b3dfcb774b57be530df42bec70ab2d239fbce9889447c7a29a4ddc1ae6"  # GitHub's own digest of the asset
# LLVM's licence (Apache-2.0 with LLVM exceptions) is not inside the binary archive: from the tag's source tree.
LICENSE_URL = f"https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-{VERSION}/LICENSE.TXT"
LICENSE_SHA256 = "8d85c1057d742e597985c7d4e6320b015a9139385cff4cbae06ffc0ebe89afee"
WANT = {"bin/clang-20": "clang", "bin/lld": "ld.lld"}
STAMP = f"llvm {VERSION} {SHA256}\n"


class Hashing:
    """A file-like reader that hashes what is read."""
    def __init__(self, f):
        self.f, self.h = f, hashlib.sha256()

    def read(self, n=-1):
        b = self.f.read(n)
        self.h.update(b)
        return b


def main(argv):
    if len(argv) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    out = argv[0]
    stamp = os.path.join(out, ".stamp")
    if os.path.isfile(stamp) and open(stamp).read() == STAMP and all(os.path.isfile(os.path.join(out, n)) for n in [*WANT.values(), "LICENSE.TXT"]):
        print(f"fetch_llvm: {out} is current")
        return 0
    os.makedirs(out, exist_ok=True)
    print(f"fetch_llvm: streaming {URL} (about 2 GB; only clang and lld are kept)", flush=True)
    req = urllib.request.Request(URL, headers={"User-Agent": "boat-of-daphnes fetch_llvm"})
    src = Hashing(urllib.request.urlopen(req, timeout=120))
    found = set()
    with tarfile.open(fileobj=lzma.LZMAFile(src), mode="r|") as tf:
        for m in tf:
            rel = m.name.split("/", 1)[1] if "/" in m.name else m.name
            if rel in WANT and m.isreg():
                dest = os.path.join(out, WANT[rel] + ".part")
                with open(dest, "wb") as f:
                    shutil.copyfileobj(tf.extractfile(m), f)
                os.chmod(dest, 0o755)
                found.add(rel)
                print(f"fetch_llvm: kept {rel} ({m.size >> 20} MB)", flush=True)
        while src.read(1 << 20):  # (the tail of the archive, so the digest covers all of it)
            pass
    if src.h.hexdigest() != SHA256:
        for n in WANT.values():
            if os.path.exists(os.path.join(out, n + ".part")):
                os.remove(os.path.join(out, n + ".part"))
        print(f"fetch_llvm: SHA-256 mismatch\n  expected {SHA256}\n  got      {src.h.hexdigest()}", file=sys.stderr)
        return 1
    if found != set(WANT):
        print(f"fetch_llvm: the archive lacks {sorted(set(WANT) - found)}", file=sys.stderr)
        return 1
    for n in WANT.values():
        os.replace(os.path.join(out, n + ".part"), os.path.join(out, n))
    lic = urllib.request.urlopen(urllib.request.Request(LICENSE_URL, headers={"User-Agent": "boat-of-daphnes fetch_llvm"}), timeout=60).read()
    if hashlib.sha256(lic).hexdigest() != LICENSE_SHA256:
        print("fetch_llvm: LICENSE.TXT does not match its pinned SHA-256", file=sys.stderr)
        return 1
    with open(os.path.join(out, "LICENSE.TXT"), "wb") as f:
        f.write(lic)
    with open(stamp, "w") as f:
        f.write(STAMP)
    print(f"fetch_llvm: {out} ready")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
