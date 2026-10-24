#!/bin/sh
# What the first start does to the game's code, on the one file that needs no disc: compiles src/game/glue.cpp with the
# flags of src/setup/firstrun.cpp and the bundled clang, links it into a module with the bundled ld.lld, and checks that the
# module exports the entry point and that every function it asks of the program is one the program exports.
#   tools/linux/toolchain_selftest.sh <folder with clang and ld.lld> <boat-of-daphnes>
set -eu
llvm=$1
exe=$2
root=$(cd "$(dirname "$0")/../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/include/runtime"
cp "$root"/src/runtime/ctx.h "$root"/src/runtime/modules.h "$root"/src/runtime/abi.h "$tmp/include/runtime/"
export LD_LIBRARY_PATH="$llvm/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
"$llvm/clang" --version | head -1
"$llvm/clang" --target=x86_64-unknown-linux-gnu -fPIC -fvisibility=hidden -fno-stack-protector -fcf-protection=none \
    -std=c++20 -O2 -g0 -ffreestanding -nostdinc -fno-exceptions -fno-rtti -fno-threadsafe-statics -DSA2B_GAME_DLL \
    -I"$tmp/include" -w -c "$root/src/game/glue.cpp" -o "$tmp/glue.o"
"$llvm/ld.lld" -shared -Bsymbolic --gc-sections -z noexecstack --build-id=none -soname=tww_game.so -o "$tmp/tww_game.so" "$tmp/glue.o"
readelf --dyn-syms -W "$tmp/tww_game.so" | awk '$7 != "UND" && $5 == "GLOBAL" {print $8}' | grep -qx sa2b_game_module \
    || { echo "selftest: the module does not export sa2b_game_module" >&2; exit 1; }
readelf --dyn-syms -W "$tmp/tww_game.so" | awk '$7 == "UND" && $8 != "" {sub(/@.*/, "", $8); print $8}' | sort -u > "$tmp/wants"
readelf --dyn-syms -W "$exe" | awk '$7 != "UND" && $8 != "" {sub(/@.*/, "", $8); print $8}' | sort -u > "$tmp/has"
# (the tables the recompiled code defines - kHookTable, g_fnOverride and the like - are the module's own, not the program's)
missing=$(comm -23 "$tmp/wants" "$tmp/has" | grep -Ev '^_ZN2rt[0-9]+(k[A-Z][A-Za-z]*|g_fnOverride|g_hooks|g_patchPoints)E$' || true)
if [ -n "$missing" ]; then
    echo "selftest: the module asks for names the program does not export:" >&2
    echo "$missing" >&2
    exit 1
fi
echo "selftest: ok ($(wc -l < "$tmp/wants") names bound to the program, entry point exported)"
