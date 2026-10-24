#!/bin/sh
# Makes the Linux release: dist/boat-of-daphnes-<version>-linux-x86_64.tar.gz (the counterpart of package.ps1).
#   tools/package.sh [--version 1.0.0] [--build build/linux-clang] [--runtime build/linux/rando_runtime] [--llvm DIR] [--no-tar]
# Gets what is not built yet: the compiler the first start uses (tools/linux/fetch_llvm.py: clang and ld.lld from LLVM's
# release, checked against a pinned hash) and the randomizer's runtime (tools/rando/rando_runtime.py: python-build-standalone,
# wwrando, gclib). Then tools/linux/package.py puts the release together and checks that nothing of the game is in it.
set -eu
cd "$(dirname "$0")/.."
llvm=build/linux/llvm
runtime=build/linux/rando_runtime
prev=
for a in "$@"; do
    case "$prev" in --llvm) llvm=$a ;; --runtime) runtime=$a ;; esac
    prev=$a
done
python3 tools/linux/fetch_llvm.py "$llvm"
python3 tools/rando/rando_runtime.py --runtime "$runtime"
exec python3 tools/linux/package.py --llvm "$llvm" --runtime "$runtime" "$@"
